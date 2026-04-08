#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <stdio.h>

#include "mqtt_app.h"
#include "mqtt_config.h"
#include "osal_file.h"

static const char *TAG = "hq_platform";

void app_main(void)
{
    bool mqtt_started = false;
    uint32_t test_counter = 0;

    ESP_LOGI(TAG, "HQ platform firmware started");

    osal_fstat_t st = { 0 };
    if (osal_stat(MQTT_CONFIG_FILE_PATH, &st) == OSAL_SUCCESS && st.file_size > 0)
    {
        MqttApp_Init();
        mqtt_started = true;
        ESP_LOGI(TAG, "MQTT app initialized (config found)");
    }
    else
    {
        ESP_LOGW(TAG, "MQTT config not found, skipping MQTT startup");
    }

    while (1)
    {
        if (mqtt_started && MqttApp_IsConnected())
        {
            const char *topic = MQTTConfig_GetString(MQTT_CONFIG_VALUE_POST_DATA_TOPIC);
            if (topic == NULL || topic[0] == '\0')
            {
                topic = "/kawalerski/test/periodic";
            }

            char payload[64];
            (void)snprintf(payload, sizeof(payload), "{\"counter\":%lu}", (unsigned long)test_counter++);
            (void)MqttApp_PostData(topic, payload, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
