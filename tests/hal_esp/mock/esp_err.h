/*
 * Test double for ESP-IDF esp_err.h (TASK-009)
 *
 * Minimal host-side mock of the ESP-IDF error type and the error codes used
 * by the LEDC/GPIO API surface modeled by the driver test double.
 */

#ifndef MOCK_ESP_ERR_H
#define MOCK_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                (-1)
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107

#endif /* MOCK_ESP_ERR_H */