#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 屏无关的显示状态。一次定义完整，板 A（OLED）与板 B（LCD）共用。
 *
 * 板 B 使用：role_tag、ssid、channel、rssi_dbm、case_id、case_label、
 *            median_dbm、mean_dbm、stdev_db、sample_done、sample_total、
 *            uptime_s、link_ok，以及末尾追加的 base/loss 字段。
 * 其余字段由板 B 填 0。
 *
 * 追加字段说明（README「显示状态结构体追加字段」一节已记录）：
 *   rssi_valid  RSSI 是否可用（未连接 / 采样失败时为 false）
 *   has_result  本轮采集是否已算出统计量
 *   base_dbm    最近一次 case 0 的中位数
 *   base_valid  是否已经采过基准
 *   loss_db     穿透损耗 = base_dbm - median_dbm
 *   loss_valid  当前 case 的损耗是否可计算（有结果且已有基准，且不是 case 0）
 */
typedef struct {
    char     role_tag[8];      /* "A" / "B"，屏幕第 1 行用 */
    char     ssid[33];         /* 热点名，板 B 显示在屏上 */
    uint8_t  channel;          /* 信道 */
    int8_t   rssi_dbm;         /* 实时/本轮 RSSI */
    uint8_t  case_id;          /* 测点编号（板 B 用） */
    char     case_label[8];    /* 测点标签，最长 7 ASCII 字符（板 B 用） */
    float    median_dbm;       /* 中位数（板 B 用） */
    float    mean_dbm;         /* 均值（板 B 用） */
    float    stdev_db;         /* 标准差（板 B 用） */
    uint16_t sample_done;      /* 已采样数（板 B 用） */
    uint16_t sample_total;     /* 采样目标数（板 B 用） */
    uint32_t uptime_s;         /* 开机秒数 */
    bool     link_ok;          /* 板 B：Wi-Fi 已连接并拿到 IP */
    float    txp_dbm;          /* 板 A 用，板 B 恒 0 */
    uint8_t  sta_count;        /* 板 A 用，板 B 恒 0 */
    uint32_t rx_udp;           /* 板 A 用，板 B 恒 0 */

    /* ---- 以下字段为板 B 追加（追加在结构体末尾，不影响共享布局） ---- */
    bool     rssi_valid;
    bool     has_result;
    float    base_dbm;
    bool     base_valid;
    float    loss_db;
    bool     loss_valid;
} disp_state_t;

/** 初始化屏（板 B：LCD 上电、清屏、画静态框架）。 */
void Disp_Init(void);

/** 按 DISP_KIND 分派到具体后端，只刷新发生变化的区域。 */
void Disp_Render(const disp_state_t *s);

/**
 * 显示任务主体。main.c 负责创建：
 *   xTaskCreatePinnedToCore(Disp_Task, "disp", APP_DISP_TASK_STACK, NULL,
 *                           APP_DISP_TASK_PRIO, NULL, APP_DISP_TASK_CORE);
 */
void Disp_Task(void *arg);

/**
 * 刷屏/数据输出互斥标志：
 *  - `C=` 命令输出 #BEGIN...#END 块前置 true，输出完复位 false
 *  - 显示任务只在它为 false 时渲染，保证 [DISP] 不会插进 CSV
 */
extern volatile bool g_ui_paused;

/** 请求显示任务强制重绘一次（采集结束后调用；不在命令路径里直接刷屏）。 */
void Disp_RequestRefresh(void);

/** 等待显示任务退出渲染临界区，最多 timeout_ms 毫秒；退出返回 true。 */
bool Disp_WaitIdle(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */
