#include "ap_measure.h"

#include "app_config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_wifi.h"

esp_err_t ApMeasure_GetStaList(wifi_sta_list_t *list)
{
    if (list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(list, 0, sizeof(*list));
    return esp_wifi_ap_get_sta_list(list);
}

esp_err_t ApMeasure_SetTxPowerDbm(float dbm, int8_t *raw_applied)
{
    /* NaN 的比较结果恒为 false，因此这一句同时挡掉 NaN */
    if (!(dbm >= APP_TXP_MIN_DBM && dbm <= APP_TXP_MAX_DBM)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 用加 0.5 取整，避免依赖 libm 的 lroundf */
    long raw = (long)(dbm / APP_TXP_STEP_DBM + 0.5f);
    if (raw < APP_TXP_RAW_MIN) {
        raw = APP_TXP_RAW_MIN;
    } else if (raw > APP_TXP_RAW_MAX) {
        raw = APP_TXP_RAW_MAX;
    }

    esp_err_t err = esp_wifi_set_max_tx_power((int8_t)raw);
    if (err == ESP_OK && raw_applied != NULL) {
        *raw_applied = (int8_t)raw;
    }
    return err;
}

esp_err_t ApMeasure_GetTxPower(float *dbm, int8_t *raw)
{
    int8_t value = 0;
    esp_err_t err = esp_wifi_get_max_tx_power(&value);
    if (err != ESP_OK) {
        return err;
    }
    if (raw != NULL) {
        *raw = value;
    }
    if (dbm != NULL) {
        *dbm = (float)value * APP_TXP_STEP_DBM;
    }
    return ESP_OK;
}

float ApMeasure_TxPowerDbm(void)
{
    float dbm = 0.0f;
    if (ApMeasure_GetTxPower(&dbm, NULL) != ESP_OK) {
        return 0.0f;
    }
    return dbm;
}

void ApMeasure_GetSnapshot(ap_sta_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }

    memset(snap, 0, sizeof(*snap));

    wifi_sta_list_t list;
    if (ApMeasure_GetStaList(&list) != ESP_OK || list.num <= 0) {
        return;
    }

    snap->count = (uint8_t)list.num;
    snap->rssi = list.sta[0].rssi;
    memcpy(snap->mac, list.sta[0].mac, sizeof(snap->mac));
}

void ApMeasure_FormatMac(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
