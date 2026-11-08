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
 * 板 A 实际使用：role_tag、channel、rssi_dbm、uptime_s、link_ok、
 *                txp_dbm、sta_count、rx_udp
 * 其余字段由板 A 填 0。
 */
typedef struct {
    char     role_tag[8];      /* "A" / "B"，屏幕第 1 行用 */
    char     ssid[33];         /* 热点名，板 A 不显示在屏上 */
    uint8_t  channel;          /* 信道 */
    int8_t   rssi_dbm;         /* 目标 STA 的接收电平；无 STA 时无效 */
    uint8_t  case_id;          /* 测点编号（板 B 用） */
    char     case_label[8];    /* 测点标签，最长 7 ASCII 字符（板 B 用） */
    float    median_dbm;       /* 中位数（板 B 用） */
    float    mean_dbm;         /* 均值（板 B 用） */
    float    stdev_db;         /* 标准差（板 B 用） */
    uint16_t sample_done;      /* 已采样数（板 B 用） */
    uint16_t sample_total;     /* 采样目标数（板 B 用） */
    uint32_t uptime_s;         /* 开机秒数 */
    bool     link_ok;          /* UDP 保活 2 s 内有包 */
    float    txp_dbm;          /* 当前最大发射功率，单位 dBm */
    uint8_t  sta_count;        /* 已连接 STA 数 */
    uint32_t rx_udp;           /* 收到的 UDP 保活包计数 */
} disp_state_t;

/** 初始化屏（板 A：OLED 上电、清屏、画静态框架）。 */
void Disp_Init(void);

/** 按 DISP_KIND 分派到具体后端，只刷新发生变化的区域。 */
void Disp_Render(const disp_state_t *s);

/**
 * 显示任务主体。main.c 负责创建：
 *   xTaskCreatePinnedToCore(Disp_Task, "disp", 4096, NULL, 1, NULL, 0)
 */
void Disp_Task(void *arg);

/**
 * 刷屏/数据输出互斥标志：
 *  - `L` 命令输出 #STALIST 块前置 true，输出完复位 false
 *  - 显示任务只在它为 false 时渲染，保证 [DISP] 不会插进数据块
 */
extern volatile bool g_ui_paused;

/** 请求显示任务强制重绘一次（`L` 结束后调用；不能在命令执行路径里直接刷屏）。 */
void Disp_RequestRefresh(void);

/** 等待显示任务退出渲染临界区，最多 timeout_ms 毫秒；退出返回 true。 */
bool Disp_WaitIdle(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */
