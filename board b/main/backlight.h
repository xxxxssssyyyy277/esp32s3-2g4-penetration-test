#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 背光控制（只编进 LCD 构建）。
 * 正常亮度默认 60%，采集期间临时降到 20%，结束后恢复用户设置的亮度。
 */
void Backlight_Init(void);

/** 设置正常亮度 0~100（越界自动夹紧）。采集期间只记录，不立即生效。 */
void Backlight_SetPercent(int percent);
uint8_t Backlight_GetPercent(void);

/** 设置采集期间的亮度 0~100（BLDIM= 命令）。 */
void Backlight_SetCapturePercent(int percent);
uint8_t Backlight_GetCapturePercent(void);

/** 采集开始/结束：切换实际 PWM 占空比。 */
void Backlight_BeginCapture(void);
void Backlight_EndCapture(void);

#ifdef __cplusplus
}
#endif

#endif /* BACKLIGHT_H */
