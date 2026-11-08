#include "display_backend.h"

#include "app_config.h"
#include "backlight.h"
#include "console.h"
#include "LCD.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define COL_BG       LCD_BLACK
#define COL_FG       LCD_WHITE
#define COL_DIM      LCD_GRAY
#define COL_LOSS     LCD_YELLOW
#define COL_BAR_BG   LCD_RGB565(32, 32, 32)

#define BAR_X        4
#define BAR_Y        112
#define BAR_W        200
#define BAR_H        14

#define BAR_RSSI_BEST   (-30)
#define BAR_RSSI_WORST  (-100)

/* 每行固定字符数：行内先清成背景再写字，短字符串不会留下上一帧的残影。 */
enum {
    L_TITLE = 0,
    L_SSID,
    L_CH,
    L_CASE,
    L_RSSI,
    L_MED,
    L_LOSS,
    L_MEAN,
    L_SAMPLE,
    L_BASE,
    L_COUNT
};

typedef struct {
    int16_t  x;
    int16_t  y;
    uint8_t  chars;
    uint8_t  font;
    uint16_t color;
    char     text[40];
    bool     valid;
} line_t;

/* 版面坐标来自提示词：240x240 竖排，最后一行结束于 y=222 */
static line_t s_lines[L_COUNT] = {
    {  4,   4, 16, LCD_8X16, COL_FG,   "", false },   /* [B] STA */
    {  4,  26, 20, LCD_6X8,  COL_DIM,  "", false },   /* SSID */
    {  4,  40,  8, LCD_8X16, COL_FG,   "", false },   /* CH */
    {  4,  64, 16, LCD_8X16, COL_FG,   "", false },   /* CASE */
    {  4,  88, 16, LCD_8X16, COL_FG,   "", false },   /* RSSI */
    {  4, 134, 16, LCD_8X16, COL_FG,   "", false },   /* MED */
    {  4, 156, 16, LCD_8X16, COL_LOSS, "", false },   /* LOSS */
    {  4, 178, 24, LCD_6X8,  COL_DIM,  "", false },   /* MEAN/SD */
    {  4, 192, 16, LCD_8X16, COL_FG,   "", false },   /* SAMPLE */
    {  4, 214, 24, LCD_6X8,  COL_DIM,  "", false },   /* BASE/T */
};

static int s_bar_len = -1;
static uint16_t s_bar_color = COL_BAR_BG;

static int line_width(const line_t *l)
{
    return (int)l->chars * (int)l->font;
}

static int line_height(const line_t *l)
{
    return (l->font == LCD_8X16) ? 16 : 8;
}

/* 只写显存，不上屏：开机静态框架用，最后统一 LCD_Update() 一次 */
static void line_draw_only(int idx, const char *text)
{
    line_t *l = &s_lines[idx];

    snprintf(l->text, sizeof(l->text), "%s", text);
    if (strlen(l->text) > (size_t)l->chars) {
        l->text[l->chars] = '\0';       /* 超出固定宽度的部分截掉，绝不串行 */
    }

    const int w = line_width(l);
    const int h = line_height(l);
    LCD_ClearArea(l->x, l->y, (uint16_t)w, (uint16_t)h, COL_BG);
    LCD_ShowString(l->x, l->y, l->text, l->font, l->color, COL_BG);
    l->valid = true;
}

/* 文本没变就一个像素都不发，这是 5 Hz 刷新不超时的关键 */
static void show_line(int idx, const char *text)
{
    line_t *l = &s_lines[idx];

    if (l->valid && strcmp(l->text, text) == 0) {
        return;
    }

    line_draw_only(idx, text);
    LCD_UpdateArea(l->x, l->y, (uint16_t)line_width(l), (uint16_t)line_height(l));
}

static uint16_t rssi_color(const disp_state_t *s)
{
    if (!s->link_ok || !s->rssi_valid) {
        return LCD_GRAY;
    }
    if (s->rssi_dbm >= -50) {
        return LCD_GREEN;
    }
    if (s->rssi_dbm >= -70) {
        return LCD_YELLOW;
    }
    if (s->rssi_dbm >= -85) {
        return LCD_ORANGE;
    }
    return LCD_RED;
}

static int rssi_bar_len(const disp_state_t *s)
{
    if (!s->link_ok || !s->rssi_valid) {
        return 0;
    }
    int d = BAR_RSSI_BEST - (int)s->rssi_dbm;      /* -30 dBm -> 0 */
    if (d < 0) {
        d = 0;
    }
    int len = d * BAR_W / (BAR_RSSI_BEST - BAR_RSSI_WORST);
    if (len > BAR_W) {
        len = BAR_W;
    }
    return len;
}

/* 信号条：颜色变化整条重刷；只是长度变化时只刷变化的那一小段 */
static void render_bar(const disp_state_t *s)
{
    const int len = rssi_bar_len(s);
    const uint16_t color = rssi_color(s);

    const bool full = (s_bar_len < 0) || (color != s_bar_color);
    if (!full && len == s_bar_len) {
        return;
    }

    LCD_DrawRectangle(BAR_X, BAR_Y, BAR_W, BAR_H, COL_BAR_BG, LCD_FILLED);
    if (len > 0) {
        LCD_DrawRectangle(BAR_X, BAR_Y, (uint16_t)len, BAR_H, color, LCD_FILLED);
    }

    if (full) {
        LCD_UpdateArea(BAR_X, BAR_Y, BAR_W, BAR_H);
    } else {
        const int lo = (len < s_bar_len) ? len : s_bar_len;
        const int hi = (len > s_bar_len) ? len : s_bar_len;
        if (hi > lo) {
            LCD_UpdateArea((int16_t)(BAR_X + lo), BAR_Y,
                           (uint16_t)(hi - lo), BAR_H);
        }
    }

    s_bar_len = len;
    s_bar_color = color;
}

void DispBackend_Init(void)
{
    LCD_Init();

    /* LCD_Init 内部按 LCD_BL_BRIGHTNESS 设过一次，这里统一由背光模块接管 */
    Backlight_Init();

    char buf[40];

    snprintf(buf, sizeof(buf), "[%s] %s", APP_ROLE_TAG, APP_ROLE_NAME);
    line_draw_only(L_TITLE, buf);
    snprintf(buf, sizeof(buf), "SSID: %.14s", APP_WIFI_SSID);
    line_draw_only(L_SSID, buf);
    snprintf(buf, sizeof(buf), "CH:%u", (unsigned)APP_WIFI_CHANNEL);
    line_draw_only(L_CH, buf);
    line_draw_only(L_CASE, "CASE 0 NONE");
    line_draw_only(L_RSSI, "RSSI: --- dBm");
    line_draw_only(L_MED, "MED: --.-dBm");
    line_draw_only(L_LOSS, "LOSS: --.-dB");
    line_draw_only(L_MEAN, "MEAN: --.- SD: --.-");
    snprintf(buf, sizeof(buf), "SAMPLE: 0/%u", (unsigned)APP_MEAS_N_DEFAULT);
    line_draw_only(L_SAMPLE, buf);
    line_draw_only(L_BASE, "BASE:  -   T:0s");

    /* 信号条外框（静态）+ 背景 */
    LCD_DrawRectangle(BAR_X - 1, BAR_Y - 1, BAR_W + 2, BAR_H + 2, COL_DIM, LCD_UNFILLED);
    LCD_DrawRectangle(BAR_X, BAR_Y, BAR_W, BAR_H, COL_BAR_BG, LCD_FILLED);
    s_bar_len = 0;
    s_bar_color = COL_BAR_BG;

    /* 开机整屏刷一次静态框架 */
    LCD_Update();

    Console_Line("#LCD w=%d h=%d xoff=%d yoff=%d inv=%d madctl=0x%02X bl=%u",
                 LCD_WIDTH, LCD_HEIGHT, LCD_X_OFFSET, LCD_Y_OFFSET,
                 LCD_INVERSION_ON, LCD_MADCTL, (unsigned)Backlight_GetPercent());
}

void DispBackend_Render(const disp_state_t *s)
{
    char buf[40];

    const char *role = (s->role_tag[0] != '\0') ? s->role_tag : APP_ROLE_TAG;
    if (s->link_ok) {
        snprintf(buf, sizeof(buf), "[%s] %s", role, APP_ROLE_NAME);
    } else {
        snprintf(buf, sizeof(buf), "[%s] %s NO LINK", role, APP_ROLE_NAME);
    }
    show_line(L_TITLE, buf);

    snprintf(buf, sizeof(buf), "SSID: %.14s",
             (s->ssid[0] != '\0') ? s->ssid : APP_WIFI_SSID);
    show_line(L_SSID, buf);

    snprintf(buf, sizeof(buf), "CH:%u", (unsigned)s->channel);
    show_line(L_CH, buf);

    snprintf(buf, sizeof(buf), "CASE %u %.7s", (unsigned)s->case_id,
             (s->case_label[0] != '\0') ? s->case_label : "?");
    show_line(L_CASE, buf);

    if (s->link_ok && s->rssi_valid) {
        snprintf(buf, sizeof(buf), "RSSI: %d dBm", (int)s->rssi_dbm);
    } else {
        snprintf(buf, sizeof(buf), "RSSI: --- dBm");
    }
    show_line(L_RSSI, buf);

    if (s->has_result) {
        snprintf(buf, sizeof(buf), "MED: %.1fdBm", (double)s->median_dbm);
    } else {
        snprintf(buf, sizeof(buf), "MED: --.-dBm");
    }
    show_line(L_MED, buf);

    if (s->loss_valid) {
        snprintf(buf, sizeof(buf), "LOSS: %.1fdB", (double)s->loss_db);
    } else {
        snprintf(buf, sizeof(buf), "LOSS: --.-dB");
    }
    show_line(L_LOSS, buf);

    if (s->has_result) {
        snprintf(buf, sizeof(buf), "MEAN:%.1f SD:%.1f",
                 (double)s->mean_dbm, (double)s->stdev_db);
    } else {
        snprintf(buf, sizeof(buf), "MEAN: --.- SD:--.-");
    }
    show_line(L_MEAN, buf);

    const unsigned total = (s->sample_total != 0) ? s->sample_total : APP_MEAS_N_DEFAULT;
    snprintf(buf, sizeof(buf), "SAMPLE: %u/%u",
             (unsigned)s->sample_done, total);
    show_line(L_SAMPLE, buf);

    if (s->base_valid) {
        snprintf(buf, sizeof(buf), "BASE:%.1f T:%lus",
                 (double)s->base_dbm, (unsigned long)s->uptime_s);
    } else {
        snprintf(buf, sizeof(buf), "BASE:  -   T:%lus", (unsigned long)s->uptime_s);
    }
    show_line(L_BASE, buf);

    render_bar(s);
}

