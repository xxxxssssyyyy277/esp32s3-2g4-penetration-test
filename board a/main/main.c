#include "app_config.h"
#include "console.h"
#include "display.h"
#include "uart_cmd.h"
#include "udp_sink.h"
#include "wifi_link.h"

#include <stdio.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static void create_task(TaskFunction_t fn, const char *name,
                        UBaseType_t prio, BaseType_t core)
{
    if (xTaskCreatePinnedToCore(fn, name, APP_TASK_STACK, NULL, prio, NULL, core) != pdPASS) {
        Console_Line("#ERR task_create %s", name);
    }
}

void app_main(void)
{
    /* 先装 UART 驱动，后面所有输出都走 Console_Line（不带日志前缀，协议行必须是行首） */
    Console_Init();
    Console_Line("#BOOT chip=ESP32-S3 fw=%s", APP_FW_VERSION);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        Console_Line("#ERR nvs_init err=%d", (int)err);
    }

#if defined(APP_ROLE_AP)
    /* 板 A：固定信道 6 的 SoftAP，成功后自己打印 #APINFO */
    err = WifiLink_StartSoftAp();
    if (err != ESP_OK) {
        /* 起热点失败也让后面的任务跑起来，串口和屏上能看出错在哪 */
        Console_Line("#ERR wifi_ap_start err=%d", (int)err);
    }
#else
    /* TODO: 由板 B 提示词补全（STA 连接、C= 采样、UDP 发送） */
    Console_Line("#ERR not_supported role_sta");
#endif

    /* UDP 3333：板 A 只收不发，读走即丢弃 */
    if (UdpSink_Init() != ESP_OK) {
        /* 任务里会每秒重试，这里不阻塞启动 */
        Console_Line("#ERR udp_sink_init");
    }

    create_task(Disp_Task, "disp", APP_DISP_TASK_PRIO, APP_DISP_TASK_CORE);
    create_task(UdpSink_Task, "udp", APP_UDP_TASK_PRIO, APP_UDP_TASK_CORE);
    create_task(UartCmd_Task, "cmd", APP_CMD_TASK_PRIO, APP_CMD_TASK_CORE);
}
