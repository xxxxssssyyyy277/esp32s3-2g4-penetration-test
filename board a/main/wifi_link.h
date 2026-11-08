#ifndef WIFI_LINK_H
#define WIFI_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 板 A 热点信息快照 */
typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    char    bssid_str[18];   /* "aa:bb:cc:dd:ee:ff" */
    uint8_t channel;
    bool    started;
} wifi_link_info_t;

/**
 * 启动 SoftAP：SSID/密码/信道 6/HT20/最大 4 连接，禁用自动信道选择。
 * 成功后把默认发射功率设为 APP_TXP_DEFAULT_DBM 并打印一行 #APINFO。
 */
esp_err_t WifiLink_StartSoftAp(void);

/** 取热点信息（ssid / bssid / channel），线程安全（只读缓存）。 */
void WifiLink_GetInfo(wifi_link_info_t *info);

const char *WifiLink_Ssid(void);
const char *WifiLink_BssidStr(void);
uint8_t WifiLink_Channel(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_LINK_H */
