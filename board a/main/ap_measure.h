#ifndef AP_MEASURE_H
#define AP_MEASURE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 已连 STA 的快照，供显示层使用 */
typedef struct {
    uint8_t count;     /* 已连接 STA 数 */
    int8_t  rssi;      /* 第一个 STA 的接收电平（上行 RSSI），count==0 时无意义 */
    uint8_t mac[6];    /* 第一个 STA 的 MAC */
} ap_sta_snapshot_t;

/** 读一次 STA 列表（调用 esp_wifi_ap_get_sta_list）。 */
esp_err_t ApMeasure_GetStaList(wifi_sta_list_t *list);

/**
 * 设置射频最大发射功率。
 * dbm 必须在 [APP_TXP_MIN_DBM, APP_TXP_MAX_DBM]，内部换算成 0.25 dBm 单位。
 * 成功时把实际下发的 raw 值写入 *raw_applied。
 */
esp_err_t ApMeasure_SetTxPowerDbm(float dbm, int8_t *raw_applied);

/** 读取当前最大发射功率（单位换算后） */
esp_err_t ApMeasure_GetTxPower(float *dbm, int8_t *raw);

/** 便捷接口：读失败返回 0 dBm；只在显示层用，失败时上层看 link_ok/命令输出。 */
float ApMeasure_TxPowerDbm(void);

/** 取 STA 快照；AP 未启动或没有 STA 时 count=0。 */
void ApMeasure_GetSnapshot(ap_sta_snapshot_t *snap);

/** "aa:bb:cc:dd:ee:ff" 格式化，out 至少 18 字节。 */
void ApMeasure_FormatMac(const uint8_t mac[6], char out[18]);

#ifdef __cplusplus
}
#endif

#endif /* AP_MEASURE_H */
