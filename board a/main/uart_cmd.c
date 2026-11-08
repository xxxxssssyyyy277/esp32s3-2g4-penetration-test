#include "uart_cmd.h"

#include "ap_measure.h"
#include "app_config.h"
#include "console.h"
#include "display.h"
#include "wifi_link.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* L 命令序号：每次执行 +1，写进 #STALIST_BEGIN i=... */
static uint32_t s_l_seq;
/* 防重入保护（正常路径下单任务串行执行，这里只是兜底） */
static bool s_l_busy;

static char *trim_ws(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
    return s;
}

static void to_upper(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    for (; src[i] != '\0' && (i + 1) < dst_size; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static bool parse_float_arg(const char *text, float *out)
{
    char *end = NULL;
    float value = strtof(text, &end);
    if (end == text || *end != '\0' || !isfinite(value)) {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_int_arg(const char *text, long *out)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

static void cmd_p(void)
{
    wifi_link_info_t info;
    WifiLink_GetInfo(&info);

    float dbm = 0.0f;
    int8_t raw = 0;
    if (ApMeasure_GetTxPower(&dbm, &raw) != ESP_OK) {
        dbm = 0.0f;
    }

    Console_Line("#APINFO ssid=%s bssid=%s ch=%u txp=%.1f",
                 info.ssid, info.bssid_str, info.channel, dbm);
}


static void cmd_l(void)
{
    wifi_sta_list_t list;
    esp_err_t err = ApMeasure_GetStaList(&list);

    if (err != ESP_OK) {
        /* 出错也只是「没有 STA 列表」，数据块本身仍按格式输出，方便上位机解析 */
        Console_Line("#ERR stalist err=%d", (int)err);
    }

    /* 关键：先暂停刷屏，并等显示任务退出渲染临界区，
     * 保证 [DISP] 不会插进 #STALIST_BEGIN 与数据行之间。 */
    g_ui_paused = true;
    (void)Disp_WaitIdle(APP_L_UI_SETTLE_MS);

    s_l_seq++;
    Console_Line("#STALIST_BEGIN i=%lu t_ms=%lld",
                 (unsigned long)s_l_seq,
                 (long long)(esp_timer_get_time() / 1000));

    if (err != ESP_OK) {
        Console_Line("#STALIST n=0");
        Console_Line("mac,rssi");
    } else {
        int count = list.num;
        if (count < 0) {
            count = 0;
        }
        if (count > ESP_WIFI_MAX_CONN_NUM) {
            count = ESP_WIFI_MAX_CONN_NUM;
        }

        Console_Line("#STALIST n=%d", count);
        Console_Line("mac,rssi");
        for (int i = 0; i < count; i++) {
            char mac[18];
            ApMeasure_FormatMac(list.sta[i].mac, mac);
            Console_Line("%s,%d", mac, list.sta[i].rssi);
        }
    }

    /* L 执行期间送进来的命令按规格回 #ERR busy 并丢弃 */
    size_t buffered = 0;
    if (uart_get_buffered_data_len(APP_UART_PORT, &buffered) == ESP_OK && buffered > 0) {
        uint8_t drain[32];
        while (uart_read_bytes(APP_UART_PORT, drain, sizeof(drain), 0) > 0) {
            /* 读空即丢 */
        }
        Console_Line("#ERR busy");
    }

    g_ui_paused = false;
    Disp_RequestRefresh();
}

static bool cmd_t(const char *arg)
{
    float dbm = 0.0f;
    if (!parse_float_arg(arg, &dbm)) {
        Console_Line("#ERR bad_arg %s", arg);
        return false;
    }
    if (!(dbm >= APP_TXP_MIN_DBM && dbm <= APP_TXP_MAX_DBM)) {
        Console_Line("#ERR txpower_range");
        return false;
    }

    int8_t raw = 0;
    esp_err_t err = ApMeasure_SetTxPowerDbm(dbm, &raw);
    if (err != ESP_OK) {
        Console_Line("#ERR txpower_set err=%d", (int)err);
        return false;
    }

    /* 等 beacon 用新功率发出后再回，避免紧接着的 L 读到的还是旧功率下的值 */
    vTaskDelay(pdMS_TO_TICKS(APP_TXP_SETTLE_MS));

    Console_Line("#TXP set=%.1f raw=%d", (float)raw * APP_TXP_STEP_DBM, raw);
    Disp_RequestRefresh();
    return true;
}

static void cmd_g(void)
{
    float dbm = 0.0f;
    int8_t raw = 0;
    if (ApMeasure_GetTxPower(&dbm, &raw) != ESP_OK) {
        Console_Line("#ERR txpower_get");
        return;
    }
    Console_Line("#TXP get=%.1f raw=%d", dbm, raw);
}

static bool cmd_bl(const char *arg)
{
    long value = 0;
    if (!parse_int_arg(arg, &value)) {
        Console_Line("#ERR bad_arg %s", arg);
        return false;
    }
    if (value < 0 || value > 100) {
        Console_Line("#ERR bad_arg %s", arg);
        return false;
    }

    /* 板 A 的 OLED 没有背光控制脚 */
    Console_Line("#ERR not_supported");
    return false;
}

static void cmd_h(void)
{
    Console_Line("#HELP P            print AP info (ssid/bssid/ch/txp)");
    Console_Line("#HELP L            dump connected STA list with uplink rssi");
    Console_Line("#HELP T=<dBm>      set max tx power, 2..20, e.g. T=8.5");
    Console_Line("#HELP G            get max tx power");
    Console_Line("#HELP BL=<0..100>  backlight (not supported on board A)");
    Console_Line("#HELP H            this help");
}


static void dispatch(char *raw_line)
{
    char *line = trim_ws(raw_line);
    if (*line == '\0') {
        return;
    }

    if (s_l_busy) {
        Console_Line("#ERR busy");
        return;
    }

    char upper[APP_CMD_LINE_MAX];
    to_upper(upper, line, sizeof(upper));

    if (strcmp(upper, "P") == 0) {
        cmd_p();
        Console_Line("#OK P");
    } else if (strcmp(upper, "L") == 0) {
        s_l_busy = true;
        cmd_l();
        s_l_busy = false;
        Console_Line("#OK L");
    } else if (strcmp(upper, "G") == 0) {
        cmd_g();
        Console_Line("#OK G");
    } else if (strcmp(upper, "H") == 0) {
        cmd_h();
        Console_Line("#OK H");
    } else if (strncmp(upper, "T=", 2) == 0) {
        if (cmd_t(line + 2)) {
            Console_Line("#OK T");
        }
    } else if (strncmp(upper, "BL=", 3) == 0) {
        (void)cmd_bl(line + 3);
    } else {
        Console_Line("#ERR unknown_cmd %s", line);
    }
}

void UartCmd_Task(void *arg)
{
    (void)arg;

    char line[APP_CMD_LINE_MAX];
    size_t idx = 0;
    bool overflow = false;
    uint8_t ch = 0;

    for (;;) {
        int n = uart_read_bytes(APP_UART_PORT, &ch, 1, pdMS_TO_TICKS(APP_CMD_READ_TIMEOUT_MS));
        if (n != 1) {
            continue;
        }

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            if (overflow) {
                Console_Line("#ERR line_too_long");
                overflow = false;
                idx = 0;
                continue;
            }
            if (idx == 0) {
                continue;
            }
            line[idx] = '\0';
            idx = 0;
            dispatch(line);
            continue;
        }

        if (idx < (APP_CMD_LINE_MAX - 1)) {
            line[idx++] = (char)ch;
        } else {
            /* 行缓冲固定 64 字节：超出后丢弃整行，等换行再报错 */
            overflow = true;
        }
    }
}
