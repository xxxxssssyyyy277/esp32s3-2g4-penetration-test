#include "udp_sink.h"

#include "app_config.h"
#include "console.h"

#include <errno.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static int s_sock = -1;
static volatile uint32_t s_count;
static volatile TickType_t s_last_rx_tick;
static volatile bool s_has_rx;

esp_err_t UdpSink_Init(void)
{
    if (s_sock >= 0) {
        return ESP_OK;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        Console_Line("#ERR udp_socket errno=%d", errno);
        return ESP_FAIL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(APP_UDP_PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        Console_Line("#ERR udp_bind port=%d errno=%d", APP_UDP_PORT, errno);
        close(sock);
        return ESP_FAIL;
    }

    /* 超时返回让任务有机会检查 socket 状态，不会永久堵死 */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = APP_UDP_RECV_TIMEOUT_MS * 1000;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_sock = sock;
    ESP_LOGI("udp", "listening on port %d", APP_UDP_PORT);
    return ESP_OK;
}

void UdpSink_Task(void *arg)
{
    (void)arg;

    uint8_t buf[APP_UDP_PAYLOAD_MAX];
    struct sockaddr_in src;

    for (;;) {
        if (s_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            (void)UdpSink_Init();
            continue;
        }

        socklen_t src_len = sizeof(src);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);

        if (n > 0) {
            /* 读到即丢弃：必须真的读走，否则 lwIP 接收缓冲会溢出 */
            s_count++;
            s_last_rx_tick = xTaskGetTickCount();
            s_has_rx = true;
            continue;
        }

        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            /* socket 异常：关掉，下一轮重建，STA 连接不受影响 */
            close(s_sock);
            s_sock = -1;
        }
    }
}

uint32_t UdpSink_Count(void)
{
    return s_count;
}

bool UdpSink_Alive(void)
{
    if (!s_has_rx) {
        return false;
    }
    TickType_t now = xTaskGetTickCount();
    return (TickType_t)(now - s_last_rx_tick) < pdMS_TO_TICKS(APP_UDP_ALIVE_TIMEOUT_MS);
}
