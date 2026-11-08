#include "backlight.h"

#include "app_config.h"

#include "LCD.h"

static uint8_t s_manual = APP_LCD_BRIGHTNESS_DEFAULT;
static uint8_t s_capture = APP_LCD_BRIGHTNESS_CAPTURE;
static bool s_in_capture;

static uint8_t clamp_percent(int percent)
{
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return (uint8_t)percent;
}

void Backlight_Init(void)
{
    s_manual = clamp_percent(s_manual);
    s_capture = clamp_percent(s_capture);
    s_in_capture = false;
    LCD_SetBrightness(s_manual);
}

void Backlight_SetPercent(int percent)
{
    s_manual = clamp_percent(percent);
    if (!s_in_capture) {
        LCD_SetBrightness(s_manual);
    }
}

uint8_t Backlight_GetPercent(void)
{
    return s_manual;
}

void Backlight_SetCapturePercent(int percent)
{
    s_capture = clamp_percent(percent);
    if (s_in_capture) {
        LCD_SetBrightness(s_capture);
    }
}

uint8_t Backlight_GetCapturePercent(void)
{
    return s_capture;
}

void Backlight_BeginCapture(void)
{
    s_in_capture = true;
    LCD_SetBrightness(s_capture);
}

void Backlight_EndCapture(void)
{
    s_in_capture = false;
    LCD_SetBrightness(s_manual);
}
