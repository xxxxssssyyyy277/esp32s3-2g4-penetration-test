#ifndef UART_CMD_H
#define UART_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 串口命令行任务主体（core 1，prio 2，栈 4096）。
 *
 * 支持的命令（均为 ASCII，大小写不敏感）：
 *   C=<0..10>     采集一个测点，输出 #BEGIN ... CSV ... #SUMMARY #END
 *   CL            打印测点表 #CASELIST
 *   N=<10..500>   每测点采样数，默认 50
 *   I=<20..1000>  采样间隔 ms，默认 100
 *   U=<0|1>       UDP 保活开关，默认 0
 *   P             打印当前 #APINFO
 *   H             帮助
 *   BL=<0..100>   背光亮度（仅 LCD 构建）
 *   BLDIM=<0..100> 采集期间背光亮度（仅 LCD 构建）
 *
 * 采集进行中收到的命令不执行，统一在 #END 之后回 #ERR busy。
 */
void UartCmd_Task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* UART_CMD_H */
