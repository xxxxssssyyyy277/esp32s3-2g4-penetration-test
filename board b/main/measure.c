#include "measure.h"

#include "app_config.h"
#include "console.h"
#include "display.h"
#include "wifi_link.h"

#if defined(DISP_KIND_LCD)
#include "backlight.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CASE_ENTRY(id, name) [id] = #name,
#define CASE_LABEL_CHECK(id, name) \
    _Static_assert((sizeof(#name) - 1) < APP_CASE_LABEL_MAX, "case label too long");

APP_CASE_LIST(CASE_LABEL_CHECK)

static const char *const s_case_labels[APP_CASE_COUNT] = {
    APP_CASE_LIST(CASE_ENTRY)
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static measure_snapshot_t s_snap = {
    .case_id = APP_CASE_BASELINE,
    .label = "NONE",
    .sample_total = APP_MEAS_N_DEFAULT,
};
static volatile bool s_busy;
static uint8_t s_pending_case;
static TaskHandle_t s_task;

/* 命令行可改、下一轮生效 */
static uint16_t s_n = APP_MEAS_N_DEFAULT;
static uint16_t s_interval_ms = APP_MEAS_I_DEFAULT_MS;

/* 统计用：静态数组，避免 500 个样本压栈，也避免动态分配 */
static int16_t s_samples[APP_MEAS_N_MAX];

/* 最近一次 case 0 的中位数，跨轮次保留 */
static bool s_base_valid;
static float s_base_dbm;

static void snap_begin_case(uint8_t case_id, uint16_t n)
{
    /* 先在临界区外格式化，临界区里只做定长拷贝 */
    char label[16] = {0};
    snprintf(label, sizeof(label), "%s", Measure_CaseLabel(case_id));

    taskENTER_CRITICAL(&s_lock);
    s_snap.case_id = case_id;
    memcpy(s_snap.label, label, sizeof(s_snap.label));
    s_snap.rssi = 0;
    s_snap.rssi_valid = false;
    s_snap.sample_done = 0;
    s_snap.sample_total = n;
    s_snap.has_result = false;
    s_snap.median_dbm = 0.0f;
    s_snap.mean_dbm = 0.0f;
    s_snap.stdev_db = 0.0f;
    s_snap.base_valid = s_base_valid;
    s_snap.base_dbm = s_base_dbm;
    s_snap.loss_valid = false;
    s_snap.loss_db = 0.0f;
    taskEXIT_CRITICAL(&s_lock);
}

static void snap_progress(uint16_t done, bool valid, int16_t rssi)
{
    taskENTER_CRITICAL(&s_lock);
    s_snap.sample_done = done;
    s_snap.rssi_valid = valid;
    if (valid) {
        s_snap.rssi = (int8_t)rssi;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static int cmp_int16(const void *a, const void *b)
{
    const int16_t va = *(const int16_t *)a;
    const int16_t vb = *(const int16_t *)b;
    return (va > vb) - (va < vb);
}

static void run_one_case(void)
{
    uint8_t case_id;
    uint16_t n;
    uint16_t interval_ms;

    taskENTER_CRITICAL(&s_lock);
    case_id = s_pending_case;
    n = s_n;
    interval_ms = s_interval_ms;
    taskEXIT_CRITICAL(&s_lock);

    if (n > APP_MEAS_N_MAX) {
        n = APP_MEAS_N_MAX;
    }

    snap_begin_case(case_id, n);

    /* 1. 暂停显示（每轮只刷一次）并等显示任务退出临界区，避免 [DISP] 混进 CSV */
    g_ui_paused = true;
    Disp_WaitIdle(APP_UI_SETTLE_MS);

    /* 2. 背光降到 20%：减小背光升压电路对 RSSI 的干扰 */
#if defined(DISP_KIND_LCD)
    Backlight_BeginCapture();
#endif

    /* 3. 开采集窗口 */
    const int64_t t0_us = esp_timer_get_time();
    Console_Line("#BEGIN case_id=%u label=%s n=%u interval_ms=%u t0_ms=%lld",
                 (unsigned)case_id, Measure_CaseLabel(case_id), (unsigned)n,
                 (unsigned)interval_ms, (long long)(t0_us / 1000));
    Console_Line("case_id,seq,t_ms,rssi_dbm");

    uint16_t valid = 0;
    int32_t sum = 0;
    TickType_t last_wake = xTaskGetTickCount();

    for (uint16_t seq = 1; seq <= n; seq++) {
        wifi_ap_record_t ap;
        const bool ok = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        const long long t_ms = (long long)((esp_timer_get_time() - t0_us) / 1000);

        if (ok) {
            const int16_t rssi = ap.rssi;
            if (valid < APP_MEAS_N_MAX) {
                s_samples[valid] = rssi;
            }
            valid++;
            sum += rssi;
            Console_Line("%u,%u,%lld,%d", (unsigned)case_id, (unsigned)seq, t_ms, (int)rssi);
            snap_progress(seq, true, rssi);
        } else {
            /* 采集失败也必须占一行，rssi 列留空，上位机按空值处理 */
            Console_Line("%u,%u,%lld,", (unsigned)case_id, (unsigned)seq, t_ms);
            snap_progress(seq, false, 0);
        }

        if (seq < n) {
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(interval_ms));
        }
    }

    /* 4. 统计：中位数用 qsort，标准差用总体标准差 */
    float median = 0.0f;
    float mean = 0.0f;
    float stdev = 0.0f;
    int16_t min_v = 0;
    int16_t max_v = 0;

    if (valid > 0) {
        qsort(s_samples, valid, sizeof(s_samples[0]), cmp_int16);
        min_v = s_samples[0];
        max_v = s_samples[valid - 1];
        if ((valid & 1U) != 0U) {
            median = (float)s_samples[valid / 2];
        } else {
            median = ((float)s_samples[valid / 2 - 1] + (float)s_samples[valid / 2]) / 2.0f;
        }
        mean = (float)sum / (float)valid;
        double acc = 0.0;
        for (uint16_t i = 0; i < valid; i++) {
            const double d = (double)s_samples[i] - (double)mean;
            acc += d * d;
        }
        stdev = (float)sqrt(acc / (double)valid);
    }

    /* 5. 基准与穿透损耗 = median(case 0) - median(case n) */
    const bool has_result = (valid > 0);
    bool loss_valid = false;
    float loss_db = 0.0f;

    if (has_result) {
        if (case_id == APP_CASE_BASELINE) {
            s_base_dbm = median;
            s_base_valid = true;
        } else if (s_base_valid) {
            loss_db = s_base_dbm - median;
            loss_valid = true;
        }
    }

    taskENTER_CRITICAL(&s_lock);
    s_snap.has_result = has_result;
    s_snap.median_dbm = median;
    s_snap.mean_dbm = mean;
    s_snap.stdev_db = stdev;
    s_snap.base_valid = s_base_valid;
    s_snap.base_dbm = s_base_dbm;
    s_snap.loss_valid = loss_valid;
    s_snap.loss_db = loss_db;
    taskEXIT_CRITICAL(&s_lock);

    /* 6. 先 #SUMMARY 再 #END，最后一行一定是 #END */
    Console_Line("#SUMMARY case_id=%u label=%s n=%u median=%.2f mean=%.2f stdev=%.2f min=%d max=%d valid=%u",
                 (unsigned)case_id, Measure_CaseLabel(case_id), (unsigned)n,
                 (double)median, (double)mean, (double)stdev,
                 (int)min_v, (int)max_v, (unsigned)valid);
    Console_Line("#END");

    /* 7. 恢复背光与显示，最后才清 busy（命令任务此时才回 #OK C） */
#if defined(DISP_KIND_LCD)
    Backlight_EndCapture();
#endif
    g_ui_paused = false;
    Disp_RequestRefresh();

    taskENTER_CRITICAL(&s_lock);
    s_busy = false;
    taskEXIT_CRITICAL(&s_lock);
}

void Measure_Task(void *arg)
{
    (void)arg;

    taskENTER_CRITICAL(&s_lock);
    s_task = xTaskGetCurrentTaskHandle();
    taskEXIT_CRITICAL(&s_lock);

    for (;;) {
        /* 阻塞等命令任务唤醒；不用动态内存，也不轮询 */
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        run_one_case();
    }
}

bool Measure_Start(uint8_t case_id)
{
    if (!Measure_CaseExists(case_id)) {
        return false;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_busy || s_task == NULL) {
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    s_busy = true;
    s_pending_case = case_id;
    const TaskHandle_t task = s_task;
    taskEXIT_CRITICAL(&s_lock);

    xTaskNotifyGive(task);
    return true;
}

bool Measure_IsBusy(void)
{
    return s_busy;
}

void Measure_GetSnapshot(measure_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_snap;
    taskEXIT_CRITICAL(&s_lock);
}

uint16_t Measure_GetN(void)
{
    return s_n;
}

bool Measure_SetN(int n)
{
    if (n < APP_MEAS_N_MIN || n > APP_MEAS_N_MAX) {
        return false;
    }
    s_n = (uint16_t)n;
    return true;
}

uint16_t Measure_GetIntervalMs(void)
{
    return s_interval_ms;
}

bool Measure_SetIntervalMs(int ms)
{
    if (ms < APP_MEAS_I_MIN_MS || ms > APP_MEAS_I_MAX_MS) {
        return false;
    }
    s_interval_ms = (uint16_t)ms;
    return true;
}

uint8_t Measure_CaseCount(void)
{
    return APP_CASE_COUNT;
}

bool Measure_CaseExists(uint8_t case_id)
{
    return case_id < APP_CASE_COUNT;
}

const char *Measure_CaseLabel(uint8_t case_id)
{
    if (case_id >= APP_CASE_COUNT) {
        return "?";
    }
    return s_case_labels[case_id];
}
