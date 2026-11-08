#include "udp_keepalive.h"

#include "app_config.h"
#include "console.h"
#include "display.h"
#include "wifi_link.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "esp_timer.h"

static volatile bool s_enabled;
static volatile uint32_t s_count;
static uint32_t s_seq;
static int s_sock = -1;
static int s_fail_streak;
static bool s_warned;
static int64_t s_next_socket_try_us;

void UdpKa_SetEnabled(bool enabled)
{
    s_enabled = enabled;
}

bool UdpKa_IsEnabled(void)
{
    return s_enabled;
}

uint32_t UdpKa_Count(void)
{
    return s_count;
}

static void open_socket_if_needed(void)
{
    if (s_sock >= 0) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now < s_next_socket_try_us) {
        return;
    }
    s_next_socket_try_us = now + 1000000;   /* 失败后 1 s 再试 */

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        Console_Line("#WARN udp_socket_fail");
    }
}

static void send_one(void)
{
    esp_ip4_addr_t gw;
    if (!WifiLink_GetGateway(&gw)) {
        /* 还没拿到 IP：不算失败，等联网 */
        return;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(APP_UDP_PORT);
    dst.sin_addr.s_addr = gw.addr;          /* 已经是网络字节序 */

    /* 固定 32 字节：3 字节魔数 + 32 位序号，其余补 0 */
    uint8_t payload[APP_UDP_PAYLOAD_LEN];
    memset(payload, 0, sizeof(payload));
    payload[0] = 'B';
    payload[1] = 'K';
    payload[2] = 'A';
    payload[3] = (uint8_t)(s_seq >> 24);
    payload[4] = (uint8_t)(s_seq >> 16);
    payload[5] = (uint8_t)(s_seq >> 8);
    payload[6] = (uint8_t)(s_seq);

    const int sent = sendto(s_sock, payload, sizeof(payload), 0,
                            (struct sockaddr *)&dst, sizeof(dst));
    if (sent == (int)sizeof(payload)) {
        s_seq++;
        s_count++;
        s_fail_streak = 0;
        s_warned = false;
    } else {
        s_fail_streak++;
        if (s_fail_streak >= APP_UDP_FAIL_WARN_EVERY && !s_warned) {
            /* 采集窗口里只允许 CSV 与 #SUMMARY/#END，警告延后到窗口外再打 */
            if (!g_ui_paused) {
                s_warned = true;
                Console_Line("#WARN udp_fail");
            }
        }
    }
}

void UdpKa_Task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        /* 断线重连也放在这个 50 ms 节拍里，避免额外任务与阻塞事件回调 */
        WifiLink_PollConnect();
        open_socket_if_needed();

        if (s_enabled && s_sock >= 0) {
            send_one();
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_UDP_PERIOD_MS));
    }
}
