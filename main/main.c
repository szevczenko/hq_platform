#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "hq_platform";

void app_main(void)
{
    ESP_LOGI(TAG, "HQ platform firmware started");

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
