#ifndef UDP_KEEPALIVE_H
#define UDP_KEEPALIVE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * UDP 保活任务主体（core 1，prio 2，栈 3072）。
 * U=1 时每 50 ms 向网关 3333 端口发 1 个 32 字节包，同时负责断线重连轮询。
 */
void UdpKa_Task(void *arg);

void UdpKa_SetEnabled(bool enabled);
bool UdpKa_IsEnabled(void);

/** 成功发出的包数（U=0 期间不增长）。 */
uint32_t UdpKa_Count(void);

#ifdef __cplusplus
}
#endif

#endif /* UDP_KEEPALIVE_H */
