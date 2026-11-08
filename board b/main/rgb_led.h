#ifndef RGB_LED_H
#define RGB_LED_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 把板载 RGB 彩灯的数据脚全部配成推挽输出并拉低，保证彩灯常灭。
 *
 * 覆盖三种常见接法：ESP32-S3-DevKitC-1 v1.0 (GPIO48)、v1.1 (GPIO38)、
 * ESP32-S3-Zero (GPIO21)。引脚列表在 app_config.h 的 APP_RGB_LED_PINS。
 * 本工程不使用外部 JTAG / 这三个脚，拉低不会影响 LCD、Wi-Fi、串口。
 */
void RgbLed_Off(void);

#ifdef __cplusplus
}
#endif

#endif /* RGB_LED_H */
