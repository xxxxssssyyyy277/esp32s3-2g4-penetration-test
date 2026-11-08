#include "display.h"

#include "app_config.h"
#include "console.h"
#include "display_backend.h"
#include "measure.h"
#include "wifi_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 与串口数据块互斥的刷屏标志：C= 输出 #BEGIN...#END 期间为 true */
volatile bool g_ui_paused;

/* 渲染临界区标志。用 portMUX 而不是互斥量：只保护几行标志位，不允许阻塞。 */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
/* 上电即置 true：显示任务还没跑到 Disp_Init 时，C= 也必须能等到
 * 开机 LCD 初始化/首屏刷新结束，否则 #LCD 可能落进 #BEGIN...#END。 */
static volatile bool s_rendering = true;
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
    /* 开机 LCD 初始化/全屏刷也要算进“渲染临界区”：
     * 否则上电后立刻发 C= 时，采样可能和 115 KB 显存的软件 SPI 全屏刷新重叠。 */
    taskENTER_CRITICAL(&s_state_lock);
    s_rendering = true;
    taskEXIT_CRITICAL(&s_state_lock);

    DispBackend_Init();

    taskENTER_CRITICAL(&s_state_lock);
    s_rendering = false;
    taskEXIT_CRITICAL(&s_state_lock);
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
    /* 在临界区里读 g_ui_paused：保证与 C= 的「置位 -> 等 idle」不交叉，
     * 否则可能出现命令已开始输出、显示任务却刚开始刷屏的窗口。 */
    paused = g_ui_paused;
    taskEXIT_CRITICAL(&s_state_lock);

    if (!paused) {
        DispBackend_Render(s);

        /* 每次真正刷新都打一行同步日志。必须位于行首、用 printf 输出，
         * 不能走 ESP_LOGI（会带 "I (123) tag:" 前缀，上位机过滤不掉）。 */
        Console_Line("[DISP] [%s]C%u %s rssi=%d med=%.1f base=%.1f smp=%u/%u",
                     s->role_tag, (unsigned)s->case_id, s->case_label,
                     (int)s->rssi_dbm, (double)s->median_dbm, (double)s->base_dbm,
                     (unsigned)s->sample_done, (unsigned)s->sample_total);
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

    measure_snapshot_t m;
    Measure_GetSnapshot(&m);

    s->channel = WifiLink_Channel();
    s->link_ok = WifiLink_IsConnected();
    s->case_id = m.case_id;
    snprintf(s->case_label, sizeof(s->case_label), "%.7s", m.label);
    s->sample_done = m.sample_done;
    s->sample_total = m.sample_total;
    s->median_dbm = m.median_dbm;
    s->mean_dbm = m.mean_dbm;
    s->stdev_db = m.stdev_db;
    s->has_result = m.has_result;
    s->base_dbm = m.base_dbm;
    s->base_valid = m.base_valid;
    s->loss_db = m.loss_db;
    s->loss_valid = m.loss_valid;

    int8_t live_rssi = 0;
    if (m.busy) {
        /* 采集中用采样任务发布的最新值，避免与采样读数互相打架 */
        s->rssi_dbm = m.rssi;
        s->rssi_valid = m.rssi_valid;
    } else if (s->link_ok && WifiLink_GetLiveRssi(&live_rssi)) {
        s->rssi_dbm = live_rssi;
        s->rssi_valid = true;
    } else {
        s->rssi_dbm = 0;
        s->rssi_valid = false;
    }

    s->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    /* rx_udp / txp_dbm / sta_count 属于板 A，板 B 填 0 且不占版面 */
    s->rx_udp = 0;
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

        /* force 只把「补刷」提前到 200 ms 边界，不会突破 5 Hz 上限 */
        if (force && (TickType_t)(now - last) < pdMS_TO_TICKS(APP_DISP_PERIOD_MS)) {
            TickType_t remain = pdMS_TO_TICKS(APP_DISP_PERIOD_MS) - (TickType_t)(now - last);
            vTaskDelay(remain);
            continue;
        }

        if ((TickType_t)(now - last) >= pdMS_TO_TICKS(APP_DISP_PERIOD_MS)) {
            if (g_ui_paused) {
                /* C= 正在输出数据块：这一拍跳过，last 不更新，解除暂停后立即补刷 */
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
