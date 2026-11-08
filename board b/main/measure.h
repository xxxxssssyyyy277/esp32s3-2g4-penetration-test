#ifndef MEASURE_H
#define MEASURE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 采样任务对外发布的快照（显示任务只读） */
typedef struct {
    bool     busy;
    uint8_t  case_id;
    char     label[16];
    int8_t   rssi;            /* 最近一个采样点的 RSSI */
    bool     rssi_valid;      /* 最近一个采样点是否成功 */
    uint16_t sample_done;
    uint16_t sample_total;
    bool     has_result;      /* 本轮已算完统计量 */
    float    median_dbm;
    float    mean_dbm;
    float    stdev_db;
    bool     base_valid;      /* 已记住基准（最近一次 case 0 的 median） */
    float    base_dbm;
    bool     loss_valid;      /* 当前 case 的损耗可用 */
    float    loss_db;
} measure_snapshot_t;

/** 采样任务主体：app_main 用 xTaskCreatePinnedToCore 创建（core 1，prio 3） */
void Measure_Task(void *arg);

/** 启动一轮采集。忙时返回 false（调用方回 #ERR busy）。 */
bool Measure_Start(uint8_t case_id);

bool Measure_IsBusy(void);

/** 取快照（线程安全） */
void Measure_GetSnapshot(measure_snapshot_t *out);

uint16_t Measure_GetN(void);
bool     Measure_SetN(int n);              /* 10 ~ 500 */

uint16_t Measure_GetIntervalMs(void);
bool     Measure_SetIntervalMs(int ms);    /* 20 ~ 1000 */

uint8_t     Measure_CaseCount(void);
bool        Measure_CaseExists(uint8_t case_id);
const char *Measure_CaseLabel(uint8_t case_id);

#ifdef __cplusplus
}
#endif

#endif /* MEASURE_H */
