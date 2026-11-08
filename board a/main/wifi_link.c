#include "wifi_link.h"

#include "app_config.h"
#include "console.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

static wifi_link_info_t s_info;
static esp_netif_t *s_netif;

static void format_mac(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *ev = (const wifi_event_ap_staconnected_t *)event_data;
        char mac[18];
        format_mac(ev->mac, mac);
        /* 只在 DEBUG 级别提示：协议行必须保持 [DISP] 与 # 两类，不额外插话 */
        ESP_LOGD("wifi", "sta connected mac=%s aid=%u", mac, ev->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *ev = (const wifi_event_ap_stadisconnected_t *)event_data;
        char mac[18];
        format_mac(ev->mac, mac);
        ESP_LOGD("wifi", "sta disconnected mac=%s aid=%u reason=%u", mac, ev->aid, ev->reason);
        break;
    }
    default:
        break;
    }
}

esp_err_t WifiLink_StartSoftAp(void)
{
    esp_err_t err;

    err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        return err;
    }

    s_netif = esp_netif_create_default_wifi_ap();
    if (s_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* 配置只放 RAM，不写 flash：每次上电都是同一套参数 */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.ap.ssid, APP_WIFI_SSID, strlen(APP_WIFI_SSID));
    memcpy(cfg.ap.password, APP_WIFI_PASSWORD, strlen(APP_WIFI_PASSWORD));
    cfg.ap.ssid_len = (uint8_t)strlen(APP_WIFI_SSID);
    cfg.ap.channel = APP_WIFI_CHANNEL;          /* 锁定信道 6 */
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = APP_WIFI_MAX_STA;
    cfg.ap.ssid_hidden = 0;
    cfg.ap.beacon_interval = APP_WIFI_BEACON_INTERVAL;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    /* 带宽与信道都必须在 start 之后落地：HT20 固定，避免 HT40 次级信道漂移 */
    err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_channel(APP_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        return err;
    }

    /* 默认最大发射功率 20 dBm（0.25 dBm 单位 -> 80） */
    int8_t raw = (int8_t)(APP_TXP_DEFAULT_DBM / APP_TXP_STEP_DBM + 0.5f);
    err = esp_wifi_set_max_tx_power(raw);
    if (err != ESP_OK) {
        return err;
    }

    memset(&s_info, 0, sizeof(s_info));
    snprintf(s_info.ssid, sizeof(s_info.ssid), "%s", APP_WIFI_SSID);
    s_info.channel = APP_WIFI_CHANNEL;
    s_info.started = true;

    err = esp_wifi_get_mac(WIFI_IF_AP, s_info.bssid);
    if (err != ESP_OK) {
        return err;
    }
    format_mac(s_info.bssid, s_info.bssid_str);

    Console_Line("#APINFO ssid=%s bssid=%s ch=%u txp=%.1f",
                 s_info.ssid, s_info.bssid_str, s_info.channel,
                 (float)raw * APP_TXP_STEP_DBM);

    return ESP_OK;
}

void WifiLink_GetInfo(wifi_link_info_t *info)
{
    if (info != NULL) {
        *info = s_info;
    }
}

const char *WifiLink_Ssid(void)
{
    return s_info.ssid;
}

const char *WifiLink_BssidStr(void)
{
    return s_info.bssid_str;
}

uint8_t WifiLink_Channel(void)
{
    /* 以驱动实际信道为准：启动后再读一次，避免 #APINFO 与实际不符 */
    uint8_t primary = s_info.channel;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK && primary != 0) {
        s_info.channel = primary;
    }
    return s_info.channel;
}
