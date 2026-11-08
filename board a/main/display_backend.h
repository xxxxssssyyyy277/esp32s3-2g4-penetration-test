#ifndef DISPLAY_BACKEND_H
#define DISPLAY_BACKEND_H

/* 屏后端私有接口：只有 display.c 使用。
 * 板 A: display_oled.c   板 B: display_lcd.c
 * main.c 不允许出现任何 OLED_ / ST7789_ 前缀。 */

#include "display.h"

#ifdef __cplusplus
extern "C" {
#endif

void DispBackend_Init(void);
void DispBackend_Render(const disp_state_t *s);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_BACKEND_H */
