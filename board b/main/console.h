#ifndef APP_CONSOLE_H
#define APP_CONSOLE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 console 串口（UART0，115200 8N1）并安装 UART 驱动。
 * 必须在创建命令解析任务之前调用。
 */
void Console_Init(void);

/**
 * 输出一行协议文本（自动补 '\n'，不做任何日志前缀）。
 *
 * 为什么不用 ESP_LOGI：ESP-IDF 的日志会加上 "I (123) TAG: " 前缀，
 * 上位机要求 [DISP] 与 # 开头的行必须位于行首，否则无法按协议解析。
 */
void Console_Line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* APP_CONSOLE_H */
