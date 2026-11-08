#include "display_backend.h"

#include "app_config.h"
#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "OLED.h"

#include "esp_timer.h"

#define DISP_ROWS APP_OLED_ROWS
#define DISP_COLS APP_OLED_COLS

/* 屏没应答时每隔这么久重新探测一次：
 * 无论 OLED 比 ESP32 后上电、还是中途热插拔，接上就能自动点亮，不用复位。 */
#define OLED_REPROBE_MS 2000

/* 上一帧每行的内容：相同就不刷这一行，避免整屏重绘拖慢 I2C */
static char s_last[DISP_ROWS][DISP_COLS + 1];
static bool s_last_valid[DISP_ROWS];

static bool s_oled_ready;          /* 初始化序列已发出（可能没读到 ACK，见下） */
static bool s_bus_acked;           /* 最近一次初始化/探测确实读到 ACK */
static bool s_nack_logged;         /* 失败诊断只打一次，免得串口被周期性错误淹没 */
static int64_t s_last_probe_us;    /* 上次探测时刻 */
static uint64_t s_pu_mask;         /* 上次「上拉指纹」结果（只有诊断版会用到） */
static bool s_pu_reported;         /* 是否已经报过一次「上拉指纹」 */
static uint8_t s_lv_last = 0xFF;   /* 上次读到的 (SCL<<1)|SDA 空闲电平 */
static bool s_lv_reported;         /* 是否已经报过一次电平 */

/* 每行先拼成定宽 16 列字符串：不足补空格，超限截断。
 * 定宽 + 8x16 点阵正好 128 像素，长数字不会残留、不会越界。 */
static void row_printf(char *dst, const char *fmt, ...)
{
    char tmp[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    size_t len = strlen(tmp);
    if (len > DISP_COLS) {
        len = DISP_COLS;
    }
    memset(dst, ' ', DISP_COLS);
    memcpy(dst, tmp, len);
    dst[DISP_COLS] = '\0';
}

static void build_rows(const disp_state_t *s, char rows[DISP_ROWS][DISP_COLS + 1])
{
    /* 行1：[A]AP CH6 */
    row_printf(rows[0], "[%s]%s CH%u", s->role_tag, APP_ROLE_NAME, (unsigned)s->channel);

    /* 行2：TXP:20.0dBm */
    row_printf(rows[1], "TXP:%.1fdBm", (double)s->txp_dbm);

    /* 行3：STA1 -45dBm / STA0 - */
    if (s->sta_count == 0) {
        row_printf(rows[2], "STA0 -");
    } else {
        row_printf(rows[2], "STA%u %ddBm", (unsigned)s->sta_count, (int)s->rssi_dbm);
    }

    /* 行4：RX1234 T:12s ；2 s 没有 UDP 包时前缀变 NO。
     * 计数超过 9999 按规格缩位：12345 -> RX12k3（12.3k），保证一行不超过 16 列 */
    if (s->rx_udp > 9999UL) {
        row_printf(rows[3], "%s%luk%lu T:%lus",
                   s->link_ok ? "RX" : "NO",
                   (unsigned long)(s->rx_udp / 1000UL),
                   (unsigned long)((s->rx_udp / 100UL) % 10UL),
                   (unsigned long)s->uptime_s);
    } else {
        row_printf(rows[3], "%s%lu T:%lus",
                   s->link_ok ? "RX" : "NO",
                   (unsigned long)s->rx_udp,
                   (unsigned long)s->uptime_s);
    }
}

/* 串口同步：前缀 [DISP]，去掉行尾补的空格后拼成一行 */
static void log_rows(const char rows[DISP_ROWS][DISP_COLS + 1])
{
    char parts[DISP_ROWS][DISP_COLS + 1];

    for (int r = 0; r < DISP_ROWS; r++) {
        memcpy(parts[r], rows[r], DISP_COLS + 1);
        int end = DISP_COLS - 1;
        while (end >= 0 && parts[r][end] == ' ') {
            parts[r][end] = '\0';
            end--;
        }
    }

    Console_Line("[DISP] %s %s %s %s", parts[0], parts[1], parts[2], parts[3]);
}

/* 诊断版（-DOLED_SCAN_ALL=1）专用：看一遍「哪几根脚上有外部上拉」。
 * 只把引脚设成输入 + 内部下拉去读电平，不发任何波形，所以屏不应答也能用：
 * 屏有电且与 ESP32 共地时，它的 4.7 kΩ 上拉会让 SCL/SDA 那两根脚读成高。
 * 结果变了才打印，方便热插拔杜邦线时实时看屏到底接在哪两根脚上。
 * 一条都不打 ⇒ 模块没供电 / 没共地 / 根本没接。 */
static void report_pullups_if_changed(void)
{
    uint8_t   pins[24];     /* 扫描表最多 21 颗脚，留点余量 */
    size_t    n    = OLED_ScanPullups(pins, sizeof(pins) / sizeof(pins[0]));
    uint64_t  mask = 0;

    for (size_t i = 0; i < n; i++) {
        mask |= (1ULL << pins[i]);
    }
    if (s_pu_reported && mask == s_pu_mask) {
        return;
    }
    s_pu_mask     = mask;
    s_pu_reported = true;

    if (n == 0) {
        Console_Line("#OLED pullup none (no powered module on any scanned pin)");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        Console_Line("#OLED pullup pin=%u", (unsigned)pins[i]);
    }
}

/* 诊断版（-DOLED_SCAN_ALL=1）专用：SCL/SDA 空闲电平变了就打印一行。
 * 用法（不用万用表）：把屏那头的某根线头用手碰一下模块的 GND 焊盘 ——
 * 对应位从 1 变 0 就说明「这根线 + 这颗 GPIO」是通的；
 * 碰了没反应 ⇒ 杜邦线断 / 插错孔 / 这颗脚不是你以为的那颗。 */
static void report_levels_if_changed(void)
{
    uint8_t lv = OLED_GetLineLevels();

    if (s_lv_reported && lv == s_lv_last) {
        return;
    }
    s_lv_last     = lv;
    s_lv_reported = true;
    Console_Line("#OLED lv scl=%u sda=%u", (unsigned)((lv >> 1) & 1U), (unsigned)(lv & 1U));
}

void DispBackend_Init(void)
{
    /* OLED_Init() 内部流程：探测地址（并自动试一次 SCL/SDA 互换）
     *   → 照发整段初始化序列 → 清屏 + 整屏刷新。
     *
     * 关键行为（对照江协 V2.0 参考实现改的）：参考实现的软件 I2C 根本不读 ACK，
     * 照样能把屏点亮。所以本驱动也不再把"读到 ACK"当作初始化的前提，
     * 只要线接对了，命令就一定被屏收到、屏就会亮。
     * 返回 0 只是说明"没读到 ACK"，用于串口诊断，不影响显示。 */
    uint8_t addr = OLED_Init();
    uint32_t acks = OLED_GetAckCount();
    s_last_probe_us = esp_timer_get_time();

    s_oled_ready = true;                    /* 初始化序列已发出，可以渲染 */
    s_bus_acked = (addr != 0);              /* 读到 ACK 才算总线确认 */

    if (s_bus_acked) {
        s_nack_logged = false;
        Console_Line("#OLED addr=0x%02X spd=%luus scl=%u sda=%u ack=%lu",
                     (unsigned)addr, (unsigned long)OLED_GetDelayUs(),
                     (unsigned)OLED_GetSclPin(), (unsigned)OLED_GetSdaPin(),
                     (unsigned long)acks);
    } else if (!s_nack_logged) {
        uint8_t lv = OLED_GetLineLevels();
        uint8_t pu = OLED_CheckPullups();
        Console_Line("#OLED err=nack scl=%u sda=%u pu=%u%u scl_pin=%u sda_pin=%u",
                     (unsigned)((lv >> 1) & 1U), (unsigned)(lv & 1U),
                     (unsigned)((pu >> 1) & 1U), (unsigned)(pu & 1U),
                     (unsigned)OLED_GetSclPin(), (unsigned)OLED_GetSdaPin());

        /* 速度扫描（诊断版）：在 scl=1/sda=2 上把总线从 200 kHz 一路放到 250 Hz，
         * 看哪一档能读到 ACK。全用 noack ⇒ 屏不在这两根线上；
         * 慢速档才 ack ⇒ 线接对了，只是没板载上拉/线太长，需要加 4.7 kΩ 上拉。 */
        if (OLED_DiagScanEnabled()) {
            oled_speed_result_t spd[12];
            size_t ns = OLED_SpeedSweep(spd, sizeof(spd) / sizeof(spd[0]));
            for (size_t i = 0; i < ns; i++) {
                if (spd[i].addr != 0) {
                    Console_Line("#OLED sweep %luus ack addr=0x%02X",
                                 (unsigned long)spd[i].delay_us, (unsigned)spd[i].addr);
                } else {
                    Console_Line("#OLED sweep %luus noack", (unsigned long)spd[i].delay_us);
                }
            }
        }

        /* 诊断版：先打 SCL/SDA 空闲电平，再做「上拉指纹」扫描 */
        if (OLED_DiagScanEnabled()) {
            report_levels_if_changed();
            report_pullups_if_changed();
        }

        /* 初始化序列已盲发；这里再试"另一组地址 + 换一根脚"之外的组合没有意义，
         * 除非是诊断版固件（-DOLED_SCAN_ALL=1）才会真的去扫全片引脚 */
        oled_scan_hit_t hits[16];
        size_t n = OLED_ScanPins(hits, sizeof(hits) / sizeof(hits[0]));
        for (size_t i = 0; i < n; i++) {
            Console_Line("#OLED scan scl=%u sda=%u addr=0x%02X",
                         (unsigned)hits[i].scl, (unsigned)hits[i].sda, (unsigned)hits[i].addr);
        }
        Console_Line("#OLED blind retry=%dms", OLED_REPROBE_MS);
        s_nack_logged = true;
    }

    char rows[DISP_ROWS][DISP_COLS + 1];
    row_printf(rows[0], "[%s]%s CH-", APP_ROLE_TAG, APP_ROLE_NAME);
    row_printf(rows[1], "TXP:--.-dBm");
    row_printf(rows[2], "STA0 -");
    row_printf(rows[3], "NO0 T:0s");

    for (int r = 0; r < DISP_ROWS; r++) {
        OLED_ShowString(0, (int16_t)(r * 16), rows[r], OLED_8X16);
        memcpy(s_last[r], rows[r], DISP_COLS + 1);
        s_last_valid[r] = false;   /* 第一帧真实数据仍然重绘一次 */
    }

    OLED_Update();                 /* 开机整屏刷一次静态框架 */
}

void DispBackend_Render(const disp_state_t *s)
{
    char rows[DISP_ROWS][DISP_COLS + 1];
    build_rows(s, rows);

    /* 没读到 ACK：用轻量探测每 OLED_REPROBE_MS 查一次（只发地址字节，
     * 不动显存、不重发初始化序列，所以屏不会闪）。
     * 一旦探到 ACK，说明屏上线/接线恢复，补一次完整初始化并重画整屏。 */
    if (!s_bus_acked &&
        (esp_timer_get_time() - s_last_probe_us) >= (int64_t)OLED_REPROBE_MS * 1000) {
        s_last_probe_us = esp_timer_get_time();
        if (OLED_Probe()) {
            DispBackend_Init();     /* 会打印 #OLED addr=... 并把 s_bus_acked 置真 */
        } else if (OLED_DiagScanEnabled()) {
            /* 诊断版：热插拔杜邦线时每 2 s 复查一次空闲电平 +「上拉指纹」，
             * 屏一接上（有电）就能看到它落在哪两根脚上，不用重新烧录。 */
            report_levels_if_changed();
            report_pullups_if_changed();
        }
    }

    bool changed = false;
    for (int r = 0; r < DISP_ROWS; r++) {
        if (s_last_valid[r] && strcmp(rows[r], s_last[r]) == 0) {
            continue;
        }
        if (s_oled_ready) {
            OLED_ShowString(0, (int16_t)(r * 16), rows[r], OLED_8X16);
            /* 只把变化的这一行（128x16）推给屏，不动其他页 */
            OLED_UpdateArea(0, (int16_t)(r * 16), 128, 16);
        }

        memcpy(s_last[r], rows[r], DISP_COLS + 1);
        s_last_valid[r] = true;
        changed = true;
    }

    if (changed) {
        log_rows(rows);
    }
}
