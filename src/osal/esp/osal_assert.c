#include "esp_log.h"

#include "osal_assert.h"

static const char *TAG = "OSAL";

void osal_assert(const char *file, const char *function, int line, const char *message)
{
    ESP_LOGE(TAG, "Assertion Failed:");
    ESP_LOGE(TAG, "  File:     %s", file);
    ESP_LOGE(TAG, "  Function: %s", function);
    ESP_LOGE(TAG, "  Line:     %d", line);
    ESP_LOGE(TAG, "  Message:  %s", message);
}
