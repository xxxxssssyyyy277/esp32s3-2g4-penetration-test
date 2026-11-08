#include "app_config.h"
#include "console.h"
#include "display.h"
#include "measure.h"
#include "rgb_led.h"
#include "uart_cmd.h"
#include "udp_keepalive.h"
#include "wifi_link.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static void create_task(TaskFunction_t fn, const char *name, uint32_t stack,
                        UBaseType_t prio, BaseType_t core)
{
    if (xTaskCreatePinnedToCore(fn, name, stack, NULL, prio, NULL, core) != pdPASS) {
        Console_Line("#ERR task_create %s", name);
    }
}

void app_main(void)
{
    /* 1. console 先就绪，后面的 #RGB / #BOOT 才能可靠打出来。
     *    紧接着立刻把板载 RGB 彩灯拉低（见 rgb_led.c），避免上电闪亮。 */
    Console_Init();
    /* 全部 IDF 日志关闭：串口只允许 # 协议行、[DISP] 与 CSV。
     * 运行期真正的问题用 #ERR / #WARN 协议行报告。 */
    esp_log_level_set("*", ESP_LOG_NONE);

    RgbLed_Off();

    Console_Line("#BOOT chip=ESP32-S3 role=%s disp=%s fw=%s", APP_ROLE_NAME, APP_DISP_NAME, APP_FW_VERSION);

    /* 2. NVS（Wi-Fi 驱动需要） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        Console_Line("#ERR nvs_init %d", (int)err);
    }

    /* 3. 联网。不阻塞等待：拿不到 IP 时 UdpKa_Task 每 1 s 重试。 */
#if defined(APP_ROLE_AP)
    /* TODO: 由板 A 提示词补全（SoftAP + STA 列表） */
    err = WifiLink_StartSoftAp();
#else
    err = WifiLink_StartSta();
#endif
    if (err != ESP_OK) {
        Console_Line("#ERR wifi_start %d", (int)err);
    }

    /* 4. 只创建任务，不在这里做任何业务逻辑 */
    create_task(Measure_Task, "meas", APP_MEAS_TASK_STACK, APP_MEAS_TASK_PRIO, APP_MEAS_TASK_CORE);
    create_task(UartCmd_Task, "cmd", APP_CMD_TASK_STACK, APP_CMD_TASK_PRIO, APP_CMD_TASK_CORE);
    create_task(UdpKa_Task, "udpka", APP_UDP_TASK_STACK, APP_UDP_TASK_PRIO, APP_UDP_TASK_CORE);
    create_task(Disp_Task, "disp", APP_DISP_TASK_STACK, APP_DISP_TASK_PRIO, APP_DISP_TASK_CORE);

    Console_Line("#READY");
}
