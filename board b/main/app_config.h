#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* =====================================================================
 * 板 B（STA + LCD）全部可调参数集中在这里。
 * 一套代码、两个编译期开关：APP_ROLE / DISP_KIND（见 main/CMakeLists.txt）
 * ===================================================================== */

/* ---------------- 编译期身份 ---------------- */
#if !defined(APP_ROLE_AP) && !defined(APP_ROLE_STA)
#define APP_ROLE_STA 1              /* 本工程缺省按板 B 构建 */
#endif
#if defined(APP_ROLE_AP) && defined(APP_ROLE_STA)
#error "APP_ROLE_AP 与 APP_ROLE_STA 不能同时定义"
#endif

#if !defined(DISP_KIND_OLED) && !defined(DISP_KIND_LCD)
#define DISP_KIND_LCD 1             /* 本工程缺省按 LCD 后端构建 */
#endif
#if defined(DISP_KIND_OLED) && defined(DISP_KIND_LCD)
#error "DISP_KIND_OLED 与 DISP_KIND_LCD 不能同时定义"
#endif

#define APP_FW_VERSION "B-1.0.0"

#if defined(DISP_KIND_LCD)
#define APP_DISP_NAME "LCD"
#else
#define APP_DISP_NAME "OLED"
#endif

#if defined(APP_ROLE_AP)
#define APP_ROLE_TAG  "A"
#define APP_ROLE_NAME "AP"
#else
#define APP_ROLE_TAG  "B"
#define APP_ROLE_NAME "STA"
#endif

/* ---------------- Wi-Fi（板 B：STA） ---------------- */
#define APP_WIFI_SSID             "ESP32S3-LINK"
#define APP_WIFI_PASSWORD         "12345678"
#define APP_WIFI_CHANNEL          6          /* 板 A 锁定信道 6；这里只作显示/校验 */
#define APP_WIFI_RECONNECT_MS     1000       /* 断线后退避 1 s，不限次数 */

/* ---------------- 测点表 ----------------
 * 必须与板 A、上位机 materials.csv 完全一致。
 * 标签纯 ASCII、最长 7 字符（要显示在 LCD 上）。
 * X(id, LABEL) 由 measure.c 展开成字符串表，改这里就够了。 */
#define APP_CASE_LIST(X)                                                     \
    X(0, NONE)      /* 无遮挡，基准 */                                      \
    X(1, PAPER)     /* 硬纸板 1 层 约 3 mm */                               \
    X(2, BOOK1)     /* 1 本书 约 25 mm */                                   \
    X(3, BOOK5)     /* 5 本书 约 125 mm，与 2 号构成厚度序列 */             \
    X(4, WATER)     /* 水墙：6 个水瓶并排 约 100 mm */                      \
    X(5, FOIL)      /* 铝箔 2 层，整片无缝隙 */                             \
    X(6, FOILGAP)   /* 铝箔 2 层，中间留 5 cm 直缝 */                        \
    X(7, BODY)      /* 人体站在链路中点 */                                   \
    X(8, HAND_A1)   /* 手捂板 A（被试 1）：近场吸收 + 失谐，反应最明显 */     \
    X(9, HAND_A2)   /* 手捂板 A（被试 2）：换人重复，手掌大小/干湿不同 */      \
    X(10, HAND_A3)  /* 手捂板 A（被试 3）：换人重复，用于评估人际差异 */
#define APP_CASE_COUNT            11
#define APP_CASE_LABEL_MAX        8          /* 含结尾 '\0'，最长 7 个字符 */
#define APP_CASE_BASELINE         0          /* 基准测点编号，0 号 */

/* ---------------- 采样参数 ---------------- */
#define APP_MEAS_N_DEFAULT        50
#define APP_MEAS_N_MIN            10
#define APP_MEAS_N_MAX            500
#define APP_MEAS_I_DEFAULT_MS     100
#define APP_MEAS_I_MIN_MS         20
#define APP_MEAS_I_MAX_MS         1000

/* ---------------- UDP 保活 ---------------- */
#define APP_UDP_PORT              3333
#define APP_UDP_PAYLOAD_LEN       32
#define APP_UDP_PERIOD_MS         50         /* 板 A 侧 RSSI 依赖这个包刷新 */
#define APP_UDP_FAIL_WARN_EVERY   20         /* 连续失败 20 次（约 1 s）才警告一次 */

/* ---------------- 串口与命令行 ---------------- */
#define APP_UART_PORT             0          /* console 走 UART0 */
#define APP_UART_BAUD             115200
#define APP_UART_RX_BUF           1024
#define APP_CMD_LINE_MAX          64         /* 行缓冲固定 64 字节 */
#define APP_CMD_READ_TIMEOUT_MS   50
#define APP_UI_SETTLE_MS          5000       /* 等显示任务/开机 LCD 初始化退出渲染临界区（实测约 1.8 s，留足余量） */

/* ---------------- 任务与核绑定 ---------------- */
#define APP_MEAS_TASK_PRIO        3
#define APP_MEAS_TASK_CORE        1
#define APP_MEAS_TASK_STACK       4096

#define APP_CMD_TASK_PRIO         2
#define APP_CMD_TASK_CORE         1
#define APP_CMD_TASK_STACK        4096

#define APP_UDP_TASK_PRIO         2
#define APP_UDP_TASK_CORE         1
#define APP_UDP_TASK_STACK        3072

#define APP_DISP_TASK_PRIO        1
#define APP_DISP_TASK_CORE        0          /* 显示独占 core 0 */
#define APP_DISP_TASK_STACK       4096
#define APP_DISP_PERIOD_MS        200        /* 刷新不快于 200 ms（≤5 Hz） */
#define APP_DISP_POLL_MS          20
#define APP_DISP_SLOW_DIV         5          /* MEAN/BASE 等慢字段 1 Hz 更新 */

/* ---------------- LCD 版面与背光 ---------------- */
#define APP_LCD_WIDTH             240
#define APP_LCD_HEIGHT            240
#define APP_LCD_BRIGHTNESS_DEFAULT 60        /* 开机亮度 60% */
#define APP_LCD_BRIGHTNESS_CAPTURE 20        /* 采集期间降到 20%，减小背光升压噪声 */

/* ---------------- 板载 RGB 彩灯：保持熄灭 ----------------
 * 上电后立刻把这些脚配成推挽输出并拉低，WS2812 收不到数据帧 / 普通 RGB
 * 彩灯三极管不导通，彩灯不会亮，也不会在采集期间闪烁干扰射频。
 * ESP32-S3-DevKitC-1 v1.0 = GPIO48，v1.1 = GPIO38，ESP32-S3-Zero = GPIO21。
 * 本工程其余功能不用这三个脚；确认板子型号后可删掉多余项。 */
#define APP_RGB_LED_PINS          48, 38, 21
#define APP_RGB_LED_PIN_COUNT     3

#endif /* APP_CONFIG_H */
