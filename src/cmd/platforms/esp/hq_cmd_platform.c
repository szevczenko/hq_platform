#include <stdio.h>

#include "hq_config.h"
#include "hq_cmd_internal.h"

#if CONFIG_CMD_ESP_OUTPUT_UART
#include <stdint.h>
#include "driver/uart.h"

static const uart_port_t g_cmd_uart_port = (uart_port_t)CONFIG_CMD_ESP_UART_NUM;

void hq_cmd_platform_output_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = CONFIG_CMD_ESP_UART_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    (void)uart_driver_install(g_cmd_uart_port, 256, 0, 0, NULL, 0);
    (void)uart_param_config(g_cmd_uart_port, &cfg);
}

void hq_cmd_platform_write_char(char c)
{
    uint8_t byte = (uint8_t)c;
    (void)uart_write_bytes(g_cmd_uart_port, (const char *)&byte, 1);
}

int hq_cmd_platform_read_char(void)
{
    uint8_t byte;
    int len = uart_read_bytes(g_cmd_uart_port, &byte, 1, pdMS_TO_TICKS(100));
    return (len > 0) ? (int)byte : -1;
}

#else /* CMD_ESP_OUTPUT_CONSOLE — uses ESP-IDF stdio/console UART */

#include <fcntl.h>
#include <unistd.h>

void hq_cmd_platform_output_init(void)
{
    /* Disable stdout buffering so each writeChar is visible immediately. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Set stdin non-blocking so read_char returns immediately when idle. */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0)
    {
        (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
}

void hq_cmd_platform_write_char(char c)
{
    (void)putchar((int)c);
}

int hq_cmd_platform_read_char(void)
{
    uint8_t byte;
    if (read(STDIN_FILENO, &byte, 1) == 1)
    {
        return (int)byte;
    }
    return -1;
}

#endif
