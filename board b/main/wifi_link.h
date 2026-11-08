#ifndef WIFI_LINK_H
#define WIFI_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 当前连接（或最近一次连接）的 AP 信息快照 */
typedef struct {
    char     ssid[33];
    uint8_t  bssid[6];
    char     bssid_str[18];   /* "aa:bb:cc:dd:ee:ff" */
    uint8_t  channel;
    int8_t   rssi;            /* 最近一次读到的 AP 信标 RSSI */
    bool     connected;
} wifi_link_info_t;

/**
 * 启动 STA：连接 app_config.h 里的 SSID/密码，启用事件回调。
 * 连接成功后自动关闭 Wi-Fi 省电（WIFI_PS_NONE）并打印一行 #APINFO。
 * 本函数不阻塞等待 IP；断线重连由 WifiLink_PollConnect() 每 1 s 发起。
 */
esp_err_t WifiLink_StartSta(void);

/** 断线重连轮询：未连接且距上次尝试 ≥ 1 s 时调用 esp_wifi_connect()。 */
void WifiLink_PollConnect(void);

/** 取 AP 信息快照（线程安全）。 */
void WifiLink_GetInfo(wifi_link_info_t *info);

/** 当前是否已拿到 IP。 */
bool WifiLink_IsConnected(void);

/** 实时读一次 AP 信标的 RSSI（未连接返回 false）。 */
bool WifiLink_GetLiveRssi(int8_t *rssi);

/** 取网关地址（板 A 就是 AP，网关即它自己）；未拿到 IP 返回 false。 */
bool WifiLink_GetGateway(esp_ip4_addr_t *gw);

/** 打印一行 #APINFO ssid=.. bssid=.. ch=.. rssi=..（P 命令用）。 */
void WifiLink_PrintApInfo(void);

const char *WifiLink_Ssid(void);
const char *WifiLink_BssidStr(void);
uint8_t WifiLink_Channel(void);

/**
 * TODO: 由板 A 提示词补全（SoftAP 模式）。
 * 本工程只保留可编译占位，不实现 AP 功能。
 */
esp_err_t WifiLink_StartSoftAp(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_LINK_H */
