#include "display_backend.h"

#include "app_config.h"
#include "console.h"

/* TODO: 由板 A 提示词补全（SSD1306 软件 I2C OLED 后端）。
 * 本工程只实现板 B（LCD）；这里保留可编译占位，保证
 *   idf.py -DAPP_ROLE=AP -DDISP_KIND=OLED build
 * 也能通过，方便两板共用一个源码树。 */

void DispBackend_Init(void)
{
    Console_Line("#ERR not_supported disp_oled");
}

void DispBackend_Render(const disp_state_t *s)
{
    (void)s;
}
