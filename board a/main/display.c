#include "display.h"

#include "app_config.h"
#include "display_backend.h"
#include "ap_measure.h"
#include "udp_sink.h"
#include "wifi_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 与串口输出互斥的刷屏标志：L 命令输出数据块期间为 true */
volatile bool g_ui_paused;

/* 渲染临界区标志。用 portMUX 而不是互斥量：这里只保护几行标志位，
 * 不允许阻塞，且中断/任务上下文都能安全进出。 */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_rendering;
static volatile bool s_force_refresh;

void Disp_RequestRefresh(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_force_refresh = true;
    taskEXIT_CRITICAL(&s_state_lock);
}

bool Disp_WaitIdle(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    for (;;) {
        bool busy;
        taskENTER_CRITICAL(&s_state_lock);
        busy = s_rendering;
        taskEXIT_CRITICAL(&s_state_lock);

        if (!busy) {
            return true;
        }
        if ((TickType_t)(xTaskGetTickCount() - start) >= timeout) {
            return false;
        }
        vTaskDelay(1);
    }
}

void Disp_Init(void)
{
    DispBackend_Init();
}

void Disp_Render(const disp_state_t *s)
{
    if (s == NULL) {
        return;
    }

    bool paused;
    taskENTER_CRITICAL(&s_state_lock);
    if (s_rendering) {
        /* 已经在刷，本次丢弃并预约下一次 */
        s_force_refresh = true;
        taskEXIT_CRITICAL(&s_state_lock);
        return;
    }
    s_rendering = true;
    /* 在临界区里读 g_ui_paused：保证与 L 命令的「置位 -> 等 idle」不交叉，
     * 否则可能出现 L 已开始输出、显示任务却刚开始刷屏的窗口。 */
    paused = g_ui_paused;
    taskEXIT_CRITICAL(&s_state_lock);

    if (!paused) {
        DispBackend_Render(s);
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_rendering = false;
    taskEXIT_CRITICAL(&s_state_lock);
}

static void build_state(disp_state_t *s)
{
    memset(s, 0, sizeof(*s));

    snprintf(s->role_tag, sizeof(s->role_tag), "%s", APP_ROLE_TAG);
    snprintf(s->ssid, sizeof(s->ssid), "%s", WifiLink_Ssid());
    s->channel = WifiLink_Channel();

    ap_sta_snapshot_t snap;
    ApMeasure_GetSnapshot(&snap);
    s->sta_count = snap.count;
    s->rssi_dbm = snap.rssi;

    s->txp_dbm = ApMeasure_TxPowerDbm();
    s->rx_udp = UdpSink_Count();
    s->link_ok = UdpSink_Alive();
    s->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
}

void Disp_Task(void *arg)
{
    (void)arg;

    Disp_Init();

    TickType_t last = xTaskGetTickCount() - pdMS_TO_TICKS(APP_DISP_PERIOD_MS);

    for (;;) {
        TickType_t now = xTaskGetTickCount();

        bool force;
        taskENTER_CRITICAL(&s_state_lock);
        force = s_force_refresh;
        taskEXIT_CRITICAL(&s_state_lock);

        /* force 只把「补刷」提前到 200 ms 边界，不会突破 5 Hz 上限：
         * 否则 L 结束后最长要再等一个 20 ms 轮询粒度才刷。 */
        if (force && (TickType_t)(now - last) < pdMS_TO_TICKS(APP_DISP_PERIOD_MS)) {
            TickType_t remain = pdMS_TO_TICKS(APP_DISP_PERIOD_MS) - (TickType_t)(now - last);
            vTaskDelay(remain);
            continue;
        }

        /* 刷新周期上限 5 Hz */
        if ((TickType_t)(now - last) >= pdMS_TO_TICKS(APP_DISP_PERIOD_MS)) {
            if (g_ui_paused) {
                /* L 正在输出数据块：这一拍跳过，last 不更新，解除暂停后立即补刷 */
                vTaskDelay(pdMS_TO_TICKS(APP_DISP_POLL_MS));
                continue;
            }

            disp_state_t st;
            build_state(&st);
            last = now;

            if (force) {
                taskENTER_CRITICAL(&s_state_lock);
                s_force_refresh = false;
                taskEXIT_CRITICAL(&s_state_lock);
            }

            Disp_Render(&st);
        }

        vTaskDelay(pdMS_TO_TICKS(APP_DISP_POLL_MS));
    }
}
