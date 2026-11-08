#include "console.h"

#include "app_config.h"

#include <stdarg.h>
#include <stdio.h>

#include "driver/uart.h"
#include "esp_err.h"

void Console_Init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = APP_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(APP_UART_PORT, APP_UART_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(APP_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(APP_UART_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    uart_flush_input(APP_UART_PORT);
}

void Console_Line(const char *fmt, ...)
{
    char line[192];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    /* 一次性 printf：newlib 对 stdout 加锁，保证整行不被别的任务切断。 */
    printf("%s\n", line);
    fflush(stdout);
}
