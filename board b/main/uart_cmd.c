#include "uart_cmd.h"

#include "app_config.h"
#include "console.h"
#include "measure.h"
#include "udp_keepalive.h"
#include "wifi_link.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(DISP_KIND_LCD)
#include "backlight.h"
#endif

/* 采集窗口等待状态：C= 启动后，任务继续读串口只为「发现并忽略」新命令 */
static bool s_waiting_measure;
static uint32_t s_deferred_busy;

static void to_upper(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dst_size; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static bool parse_int(const char *text, int *out)
{
    char *end = NULL;
    const long v = strtol(text, &end, 10);

    if (end == text) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    if (v < -100000L || v > 100000L) {
        return false;
    }
    *out = (int)v;
    return true;
}

static void cmd_caselist(void)
{
    Console_Line("#CASELIST n=%u", (unsigned)Measure_CaseCount());
    for (uint8_t i = 0; i < Measure_CaseCount(); i++) {
        Console_Line("%u,%s", (unsigned)i, Measure_CaseLabel(i));
    }
    Console_Line("#OK CL");
}

static void cmd_set_n(const char *line)
{
    int n = 0;
    if (!parse_int(line + 2, &n) || !Measure_SetN(n)) {
        Console_Line("#ERR bad_arg %s", line);
        return;
    }
    Console_Line("#OK N");
}

static void cmd_set_interval(const char *line)
{
    int ms = 0;
    if (!parse_int(line + 2, &ms) || !Measure_SetIntervalMs(ms)) {
        Console_Line("#ERR bad_arg %s", line);
        return;
    }
    Console_Line("#OK I");
}

static void cmd_udp(const char *line)
{
    int on = 0;
    if (!parse_int(line + 2, &on) || (on != 0 && on != 1)) {
        Console_Line("#ERR bad_arg %s", line);
        return;
    }
    UdpKa_SetEnabled(on != 0);
    Console_Line("#OK U");
}

static void cmd_help(void)
{
    Console_Line("#HELP C=<0..10> capture one case");
    Console_Line("#HELP CL list cases");
    Console_Line("#HELP N=<10..500> samples per case");
    Console_Line("#HELP I=<20..1000> sample interval ms");
    Console_Line("#HELP U=<0|1> UDP keepalive to gateway:%u", (unsigned)APP_UDP_PORT);
    Console_Line("#HELP P print AP info");
#if defined(DISP_KIND_LCD)
    Console_Line("#HELP BL=<0..100> backlight brightness");
    Console_Line("#HELP BLDIM=<0..100> brightness during capture");
#endif
    Console_Line("#HELP H this help");
    Console_Line("#OK H");
}

#if defined(DISP_KIND_LCD)
static void cmd_backlight(const char *line, bool capture)
{
    const char *arg = line + (capture ? 6 : 3);   /* "BLDIM=" / "BL=" */
    int percent = 0;
    if (!parse_int(arg, &percent) || percent < 0 || percent > 100) {
        Console_Line("#ERR bad_arg %s", line);
        return;
    }
    if (capture) {
        Backlight_SetCapturePercent(percent);
        Console_Line("#OK bldim");
    } else {
        Backlight_SetPercent(percent);
        Console_Line("#OK bl");
    }
}
#endif

static void handle_line(const char *line)
{
    char up[APP_CMD_LINE_MAX];
    to_upper(up, line, sizeof(up));

    if (strncmp(up, "C=", 2) == 0) {
        int case_id = 0;
        if (!parse_int(line + 2, &case_id)) {
            Console_Line("#ERR bad_arg %s", line);
            return;
        }
        if (case_id < 0 || case_id >= APP_CASE_COUNT) {
            Console_Line("#ERR bad_case %s", line);
            return;
        }
        if (!Measure_Start((uint8_t)case_id)) {
            Console_Line("#ERR busy");
            return;
        }
        s_waiting_measure = true;   /* #OK C 等 #END 之后由任务主循环补发 */
        return;
    }

    if (strcmp(up, "CL") == 0) {
        cmd_caselist();
        return;
    }
    if (strncmp(up, "N=", 2) == 0) {
        cmd_set_n(line);
        return;
    }
    if (strncmp(up, "I=", 2) == 0) {
        cmd_set_interval(line);
        return;
    }
    if (strncmp(up, "U=", 2) == 0) {
        cmd_udp(line);
        return;
    }
    if (strcmp(up, "P") == 0) {
        WifiLink_PrintApInfo();
        Console_Line("#OK P");
        return;
    }
    if (strcmp(up, "H") == 0) {
        cmd_help();
        return;
    }

#if defined(DISP_KIND_LCD)
    if (strncmp(up, "BLDIM=", 6) == 0) {
        cmd_backlight(line, true);
        return;
    }
    if (strncmp(up, "BL=", 3) == 0) {
        cmd_backlight(line, false);
        return;
    }
#endif

#if defined(APP_ROLE_AP)
    /* TODO: 由板 A 提示词补全（#STALIST / 发射功率 / 采集参数） */
    if (strcmp(up, "L") == 0 || strncmp(up, "T=", 2) == 0 || strncmp(up, "G=", 2) == 0) {
        Console_Line("#ERR not_supported role_ap");
        return;
    }
#endif

    Console_Line("#ERR unknown_cmd %s", line);
}


static void finish_measure_window(void)
{
    if (!s_waiting_measure || Measure_IsBusy()) {
        return;
    }

    /* 采集期间收到的命令统一在 #END 之后回 busy，避免插进 CSV 块 */
    for (uint32_t i = 0; i < s_deferred_busy; i++) {
        Console_Line("#ERR busy");
    }
    s_deferred_busy = 0;
    Console_Line("#OK C");
    s_waiting_measure = false;
}

void UartCmd_Task(void *arg)
{
    (void)arg;

    char line[APP_CMD_LINE_MAX];
    size_t len = 0;
    bool overflow = false;
    uint8_t chunk[32];

    for (;;) {
        /* #END 之后这里补发 deferred busy 与 #OK C */
        finish_measure_window();

        const int got = uart_read_bytes(APP_UART_PORT, chunk, sizeof(chunk),
                                        pdMS_TO_TICKS(APP_CMD_READ_TIMEOUT_MS));
        for (int i = 0; i < got; i++) {
            const char c = (char)chunk[i];

            if (c == '\r') {
                continue;
            }

            if (c == '\n') {
                line[len] = '\0';

                if (overflow) {
                    if (s_waiting_measure && Measure_IsBusy()) {
                        if (s_deferred_busy < 8) {
                            s_deferred_busy++;
                        }
                    } else if (s_waiting_measure) {
                        finish_measure_window();
                        Console_Line("#ERR line_too_long");
                    } else {
                        Console_Line("#ERR line_too_long");
                    }
                } else {
                    size_t start = 0;
                    while (start < len && (line[start] == ' ' || line[start] == '\t')) {
                        start++;
                    }
                    while (len > start && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
                        line[--len] = '\0';
                    }

                    if (len > start) {
                        if (s_waiting_measure && Measure_IsBusy()) {
                            /* 采集窗口内：不执行，只记账 */
                            if (s_deferred_busy < 8) {
                                s_deferred_busy++;
                            }
                        } else {
                            /* 已经过了 #END：先把 #OK C 补上，再执行下一条命令。
                             * 不能只看 s_waiting_measure——上位机可能在 #END 后
                             * 立刻发下一条 C=，此时 busy 已经清零。 */
                            finish_measure_window();
                            handle_line(&line[start]);
                        }
                    }
                }

                len = 0;
                overflow = false;
                continue;
            }

            if (len + 1 < sizeof(line)) {
                line[len++] = c;
            } else {
                overflow = true;    /* 丢弃溢出部分，换行时统一报错 */
            }
        }
    }
}
