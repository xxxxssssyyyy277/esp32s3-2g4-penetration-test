#include "rgb_led.h"

#include "app_config.h"
#include "console.h"

#include <stdio.h>

#include "driver/gpio.h"

/* 板载 RGB 彩灯保持熄灭：
 *  - WS2812 只有收到 24 bit 数据帧才亮；把数据脚静态拉低，上电后一直灭。
 *  - 若板子是普通三色 LED（共阴/共阳经三极管驱动），拉低同样不导通。
 * 这一步必须放在 app_main 最前面，避免上电到 LCD 初始化之间出现闪亮。 */
void RgbLed_Off(void)
{
    static const int pins[APP_RGB_LED_PIN_COUNT] = { APP_RGB_LED_PINS };
    uint64_t mask = 0;

    for (int i = 0; i < APP_RGB_LED_PIN_COUNT; i++) {
        if (pins[i] >= 0 && pins[i] < 49) {
            mask |= (1ULL << pins[i]);
        }
    }
    if (mask == 0) {
        return;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    for (int i = 0; i < APP_RGB_LED_PIN_COUNT; i++) {
        if (pins[i] >= 0 && pins[i] < 49) {
            gpio_set_level((gpio_num_t)pins[i], 0);
        }
    }

    /* 打印实际拉低的引脚列表（不假设宏里一定是 3 个） */
    char list[32] = {0};
    size_t used = 0;
    for (int i = 0; i < APP_RGB_LED_PIN_COUNT; i++) {
        const int n = snprintf(list + used, sizeof(list) - used,
                               (used == 0) ? "%d" : ",%d", pins[i]);
        if (n < 0 || (size_t)n >= sizeof(list) - used) {
            break;
        }
        used += (size_t)n;
    }
    Console_Line("#RGB off pins=%s", list);
}
