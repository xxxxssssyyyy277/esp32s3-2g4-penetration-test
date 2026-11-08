#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* =====================================================================
 * 本工程所有可调参数集中在这里（OLED 组件内部的引脚宏除外，见文件末尾说明）
 * ===================================================================== */

/* ---------------- 编译期身份 ---------------- */
/* 由 main/CMakeLists.txt 注入：
 *   APP_ROLE_AP  或 APP_ROLE_STA
 *   DISP_KIND_OLED 或 DISP_KIND_LCD
 * 这里给出缺省值，直接用 idf.py build 时按板 A 构建。 */
#if !defined(APP_ROLE_AP) && !defined(APP_ROLE_STA)
#define APP_ROLE_AP 1
#endif
#if defined(APP_ROLE_AP) && defined(APP_ROLE_STA)
#error "APP_ROLE_AP 与 APP_ROLE_STA 不能同时定义"
#endif

#if !defined(DISP_KIND_OLED) && !defined(DISP_KIND_LCD)
#define DISP_KIND_OLED 1
#endif
#if defined(DISP_KIND_OLED) && defined(DISP_KIND_LCD)
#error "DISP_KIND_OLED 与 DISP_KIND_LCD 不能同时定义"
#endif

#define APP_FW_VERSION "A-1.0.0"

/* 屏幕上第 1 行的角色标记，板 A 显示 [A]AP，板 B 显示 [B]STA */
#if defined(APP_ROLE_AP)
#define APP_ROLE_TAG               "A"
#define APP_ROLE_NAME              "AP"
#else
#define APP_ROLE_TAG               "B"
#define APP_ROLE_NAME              "STA"
#endif

/* ---------------- Wi-Fi（板 A：SoftAP） ---------------- */
#define APP_WIFI_SSID              "ESP32S3-LINK"
#define APP_WIFI_PASSWORD          "12345678"
#define APP_WIFI_CHANNEL           6          /* 锁定信道 6，前后数据才可比 */
#define APP_WIFI_MAX_STA           4          /* 本项目只用 1 个 */
#define APP_WIFI_BEACON_INTERVAL   100        /* TU，100 TU = 102.4 ms */

/* ---------------- 射频最大发射功率 ---------------- */
#define APP_TXP_MIN_DBM            2.0f
#define APP_TXP_MAX_DBM            20.0f
#define APP_TXP_DEFAULT_DBM        20.0f
#define APP_TXP_STEP_DBM           0.25f      /* esp_wifi_set_max_tx_power 的单位 */
#define APP_TXP_RAW_MIN            8          /*  2.0 dBm / 0.25 = 8   */
#define APP_TXP_RAW_MAX            80         /* 20.0 dBm / 0.25 = 80  */
#define APP_TXP_SETTLE_MS          500        /* 设完等 beacon 用新功率发出后再回 #OK */

/* ---------------- UDP 保活接收 ---------------- */
#define APP_UDP_PORT               3333
#define APP_UDP_PAYLOAD_MAX        64
#define APP_UDP_ALIVE_TIMEOUT_MS   2000       /* 2 s 收不到包 -> 屏上显示 NO */
#define APP_UDP_RECV_TIMEOUT_MS    100

/* ---------------- 串口与命令 ---------------- */
#define APP_UART_PORT              0          /* console 走 UART0 */
#define APP_UART_BAUD              115200
#define APP_UART_RX_BUF            1024
#define APP_CMD_LINE_MAX           64         /* 行缓冲固定 64 字节 */
#define APP_CMD_READ_TIMEOUT_MS    50
#define APP_L_UI_SETTLE_MS         100        /* 等显示任务退出渲染临界区的最长时间 */

/* ---------------- 任务与核绑定 ---------------- */
#define APP_TASK_STACK             4096
#define APP_UDP_TASK_PRIO          3
#define APP_UDP_TASK_CORE          1
#define APP_CMD_TASK_PRIO          2
#define APP_CMD_TASK_CORE          1
#define APP_DISP_TASK_PRIO         1
#define APP_DISP_TASK_CORE         0
#define APP_DISP_PERIOD_MS         200        /* 刷新不快于 200 ms（≤5 Hz） */
#define APP_DISP_POLL_MS           20         /* 显示任务的唤醒粒度 */

/* ---------------- OLED 版面 ---------------- */
#define APP_OLED_COLS              16         /* 8x16 字号，一行恰好 16 列 */
#define APP_OLED_ROWS              4

/* OLED 引脚与软件 I2C 延时定义在 components/oled/OLED.h：
 *   OLED_SCL_PIN      = GPIO_NUM_1   （模块丝印 SCK，同一根时钟线）
 *   OLED_SDA_PIN      = GPIO_NUM_2
 *   OLED_I2C_DELAY_US = 1
 * 组件不能包含 main/ 下的头文件，因此这三个宏留在驱动组件内，
 * 均可用 -D 覆盖，README「板 A 说明」有完整引脚表。 */

#endif /* APP_CONFIG_H */
