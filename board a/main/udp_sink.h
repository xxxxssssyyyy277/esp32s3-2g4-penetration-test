#ifndef UDP_SINK_H
#define UDP_SINK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建并绑定 UDP 3333 端口（板 A 只收不发）。 */
esp_err_t UdpSink_Init(void);

/**
 * 接收任务主体：循环 recvfrom 把包读走并丢弃，只累加计数。
 * main.c 创建：xTaskCreatePinnedToCore(UdpSink_Task, "udp", 4096, NULL, 3, NULL, 1)
 */
void UdpSink_Task(void *arg);

/** 累计收到的保活包数量。 */
uint32_t UdpSink_Count(void);

/** 最近 2 s 内是否收到过包（屏上 RX / NO 的判据）。 */
bool UdpSink_Alive(void);

#ifdef __cplusplus
}
#endif

#endif /* UDP_SINK_H */
