/*
 * ThingsBoard Firmware Update Demo
 *
 * Demonstrates firmware update flow over MQTT using:
 * - shared attributes for firmware metadata
 * - v2/fw request/response chunk transfer
 * - OSAL OTA abstraction for writing firmware data
 */

#include <stdio.h>

#include "osal_task.h"

#include "tb_attributes.h"
#include "tb_client.h"
#include "tb_firmware_update.h"
#include "tb_telemetry.h"

#include "mongoose_process.h"
#include "mqtt_config.h"

#ifndef CONFIG_FW_DEMO_CLIENT_ID
#define CONFIG_FW_DEMO_CLIENT_ID "fw_demo_0001"
#endif
#ifndef CONFIG_FW_DEMO_USERNAME
#define CONFIG_FW_DEMO_USERNAME "test_firmware_device"
#endif
#ifndef CONFIG_FW_DEMO_PASSWORD
#define CONFIG_FW_DEMO_PASSWORD "test_firmware_device"
#endif
#ifndef CONFIG_FW_DEMO_DEVICE_NAME
#define CONFIG_FW_DEMO_DEVICE_NAME "Firmware Update Demo"
#endif
#ifndef CONFIG_FW_DEMO_MQTT_URL
#define CONFIG_FW_DEMO_MQTT_URL "mqtt://localhost:1883"
#endif
#ifndef CONFIG_FW_DEMO_CHECK_PERIOD_MS
#define CONFIG_FW_DEMO_CHECK_PERIOD_MS 10000
#endif
#ifndef CONFIG_FW_DEMO_RECONNECT_DELAY_MS
#define CONFIG_FW_DEMO_RECONNECT_DELAY_MS 3000
#endif
#ifndef CONFIG_FW_DEMO_CHUNK_SIZE
#define CONFIG_FW_DEMO_CHUNK_SIZE 1024
#endif

#define FW_DEMO_CLIENT_ID CONFIG_FW_DEMO_CLIENT_ID
#define FW_DEMO_USERNAME CONFIG_FW_DEMO_USERNAME
#define FW_DEMO_PASSWORD CONFIG_FW_DEMO_PASSWORD
#define FW_DEMO_DEVICE_NAME CONFIG_FW_DEMO_DEVICE_NAME
#define FW_DEMO_MQTT_URL CONFIG_FW_DEMO_MQTT_URL
#define FW_DEMO_CHECK_PERIOD_MS CONFIG_FW_DEMO_CHECK_PERIOD_MS
#define FW_DEMO_RECONNECT_DELAY_MS CONFIG_FW_DEMO_RECONNECT_DELAY_MS
#define FW_DEMO_CHUNK_SIZE CONFIG_FW_DEMO_CHUNK_SIZE

static tb_client_t *g_tb_client;

static void on_fw_applied(const char *new_title, const char *new_version,
                          void *user_data)
{
    (void)user_data;
    printf("[FW_DEMO] Firmware applied: title=%s version=%s\n",
           new_title, new_version);
}

static int connect_and_wait(void)
{
    int rc = tb_client_connect(g_tb_client);
    if (rc != 0) {
        printf("[FW_DEMO] connect request failed: %d\n", rc);
        return rc;
    }

    for (int i = 0; i < 20; i++) {
        if (tb_client_is_connected(g_tb_client)) {
            return 0;
        }
        osal_task_delay_ms(250);
    }

    tb_client_disconnect(g_tb_client);
    return -1;
}

int main_function(void)
{
    int rc;

    printf("\n=============================================\n");
    printf("   ThingsBoard Firmware Update Demo\n");
    printf("=============================================\n");
    printf("  Client ID : %s\n", FW_DEMO_CLIENT_ID);
    printf("  Broker    : %s\n", FW_DEMO_MQTT_URL);
    printf("=============================================\n\n");

    MongooseProcess_Init();

    tb_client_config_t tb_cfg = {
        .server_url = FW_DEMO_MQTT_URL,
        .access_token = FW_DEMO_USERNAME,
        .client_id = FW_DEMO_CLIENT_ID,
        .device_name = FW_DEMO_DEVICE_NAME,
    };

    rc = tb_client_init(&g_tb_client, &tb_cfg);
    if (rc != 0) {
        printf("[FW_DEMO] FATAL: tb_client_init failed (%d)\n", rc);
        return 1;
    }

    mqtt_config_set_string(FW_DEMO_PASSWORD, MQTT_CONFIG_VALUE_PASSWORD);

    tb_firmware_update_config_t fw_cfg = {
        .current_title = "test_update_fw",
        .current_version = "1.0.0",
        .chunk_size = FW_DEMO_CHUNK_SIZE,
        .on_applied = on_fw_applied,
        .user_data = NULL,
    };

    bool fw_initialized = false;
    bool fw_health_confirmed = false;
    uint32_t last_check_ms = 0;

    while (1) {
        if (!tb_client_is_connected(g_tb_client)) {
            printf("[FW_DEMO] Connecting...\n");
            rc = connect_and_wait();
            if (rc != 0) {
                printf("[FW_DEMO] Reconnect in %d ms\n",
                       FW_DEMO_RECONNECT_DELAY_MS);
                osal_task_delay_ms(FW_DEMO_RECONNECT_DELAY_MS);
                continue;
            }
            printf("[FW_DEMO] Connected\n");

            if (!fw_initialized) {
                rc = tb_firmware_update_init(g_tb_client, &fw_cfg);
                if (rc != 0) {
                    printf("[FW_DEMO] firmware updater init failed: %d\n", rc);
                    osal_task_delay_ms(FW_DEMO_RECONNECT_DELAY_MS);
                    continue;
                }
                fw_initialized = true;
            }

            if (!fw_health_confirmed) {
                rc = tb_telemetry_send_string(g_tb_client,
                             "fw_health", "ready");
                if (rc == 0) {
                    rc = tb_firmware_update_confirm_health(g_tb_client);
                    if (rc == 0) {
                        fw_health_confirmed = true;
                        printf("[FW_DEMO] Running image confirmed healthy\n");
                    }
                }
            }
        }

        uint32_t now = osal_task_get_time_ms();
        if ((uint32_t)(now - last_check_ms) >= FW_DEMO_CHECK_PERIOD_MS) {
            last_check_ms = now;
            rc = tb_firmware_update_request_check(g_tb_client);
            if (rc != 0) {
                printf("[FW_DEMO] firmware check request failed: %d\n", rc);
            } else {
                printf("[FW_DEMO] firmware metadata check requested\n");
            }
        }

        osal_task_delay_ms(200);
    }
}

#ifndef CONFIG_HQ_PLATFORM_ESP
int main(void)
{
    return main_function();
}
#endif