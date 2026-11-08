#ifndef UART_CMD_H
#define UART_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 命令解析任务主体。main.c 创建：
 *   xTaskCreatePinnedToCore(UartCmd_Task, "cmd", 4096, NULL, 2, NULL, 1)
 *
 * 命令（每行一条，\n 结尾，不区分大小写）：
 *   P           打印 #APINFO
 *   L           输出 #STALIST_BEGIN / #STALIST / mac,rssi / 数据行
 *   T=<float>   设置最大发射功率 2..20 dBm
 *   G           读回最大发射功率
 *   BL=<0..100> 本板无背光 -> #ERR not_supported
 *   H           帮助
 */
void UartCmd_Task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* UART_CMD_H */
