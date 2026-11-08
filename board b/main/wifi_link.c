#include "wifi_link.h"

#include "app_config.h"
#include "console.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

static esp_netif_t *s_netif;
static volatile bool s_connected;
static int64_t s_last_connect_us;

/* 事件回调（Wi-Fi 任务）与显示/命令任务共享这份快照，用临界区保护 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_link_info_t s_info;
static bool s_prev_bssid_valid;
static uint8_t s_prev_bssid[6];

static void format_mac(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void snapshot_set_connected(bool connected, const wifi_ap_record_t *ap)
{
    /* snprintf 放在临界区外，临界区里只做定长拷贝 */
    char mac[18];
    char ssid[33] = {0};
    if (ap != NULL) {
        format_mac(ap->bssid, mac);
        snprintf(ssid, sizeof(ssid), "%s", (const char *)ap->ssid);
    }

    taskENTER_CRITICAL(&s_lock);
    s_connected = connected;
    s_info.connected = connected;
    if (ap != NULL) {
        memcpy(s_info.bssid, ap->bssid, sizeof(s_info.bssid));
        memcpy(s_info.bssid_str, mac, 18);
        memcpy(s_info.ssid, ssid, sizeof(s_info.ssid));
        s_info.channel = ap->primary;
        s_info.rssi = ap->rssi;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void sta_got_ip_handler(void)
{
    /* 关键：关省电。开启 modem sleep 时只在 beacon 时刻醒来，RSSI 会呈阶梯状。 */
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_ps err=%d", (int)err);
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        /* 极端情况：刚拿到 IP 但 AP 记录还没就绪。仍要标记已连接，
         * 否则重连轮询会误判、显示一直 NO LINK。 */
        snapshot_set_connected(true, NULL);
        wifi_link_info_t info;
        WifiLink_GetInfo(&info);
        Console_Line("#APINFO ssid=%s bssid=%s ch=%u rssi=0",
                     info.ssid, info.bssid_str, (unsigned)info.channel);
        return;
    }

    bool bssid_changed = s_prev_bssid_valid &&
                         (memcmp(s_prev_bssid, ap.bssid, sizeof(s_prev_bssid)) != 0);

    snapshot_set_connected(true, &ap);

    if (!s_prev_bssid_valid) {
        memcpy(s_prev_bssid, ap.bssid, sizeof(s_prev_bssid));
        s_prev_bssid_valid = true;
    } else if (bssid_changed) {
        memcpy(s_prev_bssid, ap.bssid, sizeof(s_prev_bssid));
    }

    char bssid[18];
    format_mac(ap.bssid, bssid);
    Console_Line("#APINFO ssid=%s bssid=%s ch=%u rssi=%d",
                 (const char *)ap.ssid, bssid, (unsigned)ap.primary, (int)ap.rssi);

    if (bssid_changed) {
        /* 换了 AP 或换了信道：之前的数据与之后不可比 */
        Console_Line("#WARN bssid_changed");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        snapshot_set_connected(false, NULL);
        /* 断线时不打印任何行：采集窗口里多一行非 CSV 输出就难排查。
         * 重连由 WifiLink_PollConnect() 退避 1 s 发起，屏上会显示 NO LINK。 */
        ESP_LOGD(TAG, "sta disconnected");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        sta_got_ip_handler();
    }
}

esp_err_t WifiLink_StartSta(void)
{
    esp_err_t err;

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_netif = esp_netif_create_default_wifi_sta();
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

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* 注册回调之后再初始化快照，避免回调抢先写入半成品 */
    memset(&s_info, 0, sizeof(s_info));
    snprintf(s_info.ssid, sizeof(s_info.ssid), "%s", APP_WIFI_SSID);
    snprintf(s_info.bssid_str, sizeof(s_info.bssid_str), "%s", "00:00:00:00:00:00");
    s_info.channel = APP_WIFI_CHANNEL;
    s_info.connected = false;
    s_connected = false;

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.sta.ssid, APP_WIFI_SSID, strlen(APP_WIFI_SSID));
    memcpy(cfg.sta.password, APP_WIFI_PASSWORD, strlen(APP_WIFI_PASSWORD));
    cfg.sta.channel = APP_WIFI_CHANNEL;          /* 板 A 锁信道 6，直连更快 */
    cfg.sta.scan_method = WIFI_FAST_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    /* 固定 HT20：与板 A 一致，避免 HT40 次级信道漂移 */
    err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (err != ESP_OK) {
        return err;
    }

    s_last_connect_us = esp_timer_get_time();
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

void WifiLink_PollConnect(void)
{
    if (s_connected) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if ((now - s_last_connect_us) < (int64_t)APP_WIFI_RECONNECT_MS * 1000) {
        return;
    }
    s_last_connect_us = now;
    /* 失败也无所谓：下一个 1 s 再来一次，不限次数 */
    (void)esp_wifi_connect();
}

void WifiLink_GetInfo(wifi_link_info_t *info)
{
    if (info == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *info = s_info;
    info->connected = s_connected;
    taskEXIT_CRITICAL(&s_lock);
}

bool WifiLink_IsConnected(void)
{
    return s_connected;
}

bool WifiLink_GetLiveRssi(int8_t *rssi)
{
    if (rssi == NULL || !s_connected) {
        return false;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    *rssi = ap.rssi;

    taskENTER_CRITICAL(&s_lock);
    s_info.rssi = ap.rssi;
    s_info.channel = ap.primary;
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

bool WifiLink_GetGateway(esp_ip4_addr_t *gw)
{
    if (gw == NULL || s_netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) {
        return false;
    }
    if (ip.gw.addr == 0) {
        return false;
    }
    *gw = ip.gw;
    return true;
}

void WifiLink_PrintApInfo(void)
{
    wifi_link_info_t info;
    WifiLink_GetInfo(&info);

    int8_t rssi = info.rssi;
    (void)WifiLink_GetLiveRssi(&rssi);

    uint8_t channel = info.channel;
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    /* 只有真正连上 AP 时才用实时信道；未连接时 esp_wifi_get_channel()
     * 返回的是扫描后残留的信道（可能 1..13 任意值），会误导上位机。 */
    if (info.connected &&
        esp_wifi_get_channel(&primary, &second) == ESP_OK && primary != 0) {
        channel = primary;
    }
    if (channel == 0) {
        channel = APP_WIFI_CHANNEL;
    }

    Console_Line("#APINFO ssid=%s bssid=%s ch=%u rssi=%d",
                 info.ssid, info.bssid_str, (unsigned)channel, (int)rssi);
}

const char *WifiLink_Ssid(void)
{
    return APP_WIFI_SSID;
}

const char *WifiLink_BssidStr(void)
{
    return s_info.bssid_str;
}

uint8_t WifiLink_Channel(void)
{
    return s_info.channel;
}

/* TODO: 由板 A 提示词补全（AP 模式、#APINFO with txp、STA 列表、发射功率） */
esp_err_t WifiLink_StartSoftAp(void)
{
    Console_Line("#ERR not_supported role_ap");
    return ESP_ERR_NOT_SUPPORTED;
}
