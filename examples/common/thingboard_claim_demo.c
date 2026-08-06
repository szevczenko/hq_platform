/*
 * ThingsBoard Claim Demo
 *
 * Demonstrates claiming with and without secret key and reports outcomes
 * through telemetry and client attributes.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "osal_task.h"

#include "tb_attributes.h"
#include "tb_claim.h"
#include "tb_client.h"
#include "tb_telemetry.h"

#include "mongoose_process.h"
#include "mqtt_config.h"

#ifndef CONFIG_CLAIM_DEMO_CLIENT_ID
#define CONFIG_CLAIM_DEMO_CLIENT_ID "tb_claim_demo_0001"
#endif
#ifndef CONFIG_CLAIM_DEMO_USERNAME
#define CONFIG_CLAIM_DEMO_USERNAME "tb_claim_demo"
#endif
#ifndef CONFIG_CLAIM_DEMO_PASSWORD
#define CONFIG_CLAIM_DEMO_PASSWORD "tb_claim_demo"
#endif
#ifndef CONFIG_CLAIM_DEMO_DEVICE_NAME
#define CONFIG_CLAIM_DEMO_DEVICE_NAME "Claim Demo"
#endif
#ifndef CONFIG_CLAIM_DEMO_MQTT_URL
#define CONFIG_CLAIM_DEMO_MQTT_URL "mqtt://localhost:1883"
#endif
#ifndef CONFIG_CLAIM_DEMO_RECONNECT_DELAY_MS
#define CONFIG_CLAIM_DEMO_RECONNECT_DELAY_MS 3000
#endif
#ifndef CONFIG_CLAIM_DEMO_PERIOD_MS
#define CONFIG_CLAIM_DEMO_PERIOD_MS 30000
#endif
#ifndef CONFIG_CLAIM_DEMO_SECRET
#define CONFIG_CLAIM_DEMO_SECRET "demo_secret_key"
#endif
#ifndef CONFIG_CLAIM_DEMO_DURATION_SECRET_MS
#define CONFIG_CLAIM_DEMO_DURATION_SECRET_MS 60000
#endif
#ifndef CONFIG_CLAIM_DEMO_DURATION_NO_SECRET_MS
#define CONFIG_CLAIM_DEMO_DURATION_NO_SECRET_MS 30000
#endif

#define CLAIM_DEMO_CLIENT_ID CONFIG_CLAIM_DEMO_CLIENT_ID
#define CLAIM_DEMO_USERNAME CONFIG_CLAIM_DEMO_USERNAME
#define CLAIM_DEMO_PASSWORD CONFIG_CLAIM_DEMO_PASSWORD
#define CLAIM_DEMO_DEVICE_NAME CONFIG_CLAIM_DEMO_DEVICE_NAME
#define CLAIM_DEMO_MQTT_URL CONFIG_CLAIM_DEMO_MQTT_URL
#define CLAIM_DEMO_RECONNECT_DELAY_MS CONFIG_CLAIM_DEMO_RECONNECT_DELAY_MS
#define CLAIM_DEMO_PERIOD_MS CONFIG_CLAIM_DEMO_PERIOD_MS
#define CLAIM_DEMO_SECRET CONFIG_CLAIM_DEMO_SECRET
#define CLAIM_DEMO_DURATION_SECRET_MS CONFIG_CLAIM_DEMO_DURATION_SECRET_MS
#define CLAIM_DEMO_DURATION_NO_SECRET_MS CONFIG_CLAIM_DEMO_DURATION_NO_SECRET_MS

static tb_client_t *g_tb_client;

static int connect_and_wait(void)
{
    int rc = tb_client_connect(g_tb_client);
    if (rc != 0) {
        printf("[CLAIM_DEMO] connect request failed: %d\n", rc);
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

static void report_claim_result(const char *mode, int rc)
{
    char telemetry[128];

    snprintf(telemetry, sizeof(telemetry),
             "{\"claim_mode\":\"%s\",\"claim_rc\":%d}", mode, rc);

    if (tb_telemetry_send_json(g_tb_client, telemetry) != 0) {
        printf("[CLAIM_DEMO] WARN: claim telemetry report failed for %s\n", mode);
    }

    if (tb_attributes_send_string(g_tb_client, "last_claim_mode", mode) != 0) {
        printf("[CLAIM_DEMO] WARN: claim attribute report failed for %s\n", mode);
    }
}

static void run_claim_round(void)
{
    int rc = tb_claim_device(g_tb_client, CLAIM_DEMO_SECRET,
                             CLAIM_DEMO_DURATION_SECRET_MS);
    printf("[CLAIM_DEMO] claim with secret rc=%d\n", rc);
    report_claim_result("with_secret", rc);

    rc = tb_claim_device(g_tb_client, NULL,
                         CLAIM_DEMO_DURATION_NO_SECRET_MS);
    printf("[CLAIM_DEMO] claim without secret rc=%d\n", rc);
    report_claim_result("without_secret", rc);
}

int main_function(void)
{
    int rc;
    uint32_t last_round_ms = 0;

    printf("\n=============================================\n");
    printf("   ThingsBoard Claim Demo\n");
    printf("=============================================\n");
    printf("  Client ID : %s\n", CLAIM_DEMO_CLIENT_ID);
    printf("  Broker    : %s\n", CLAIM_DEMO_MQTT_URL);
    printf("=============================================\n\n");

    MongooseProcess_Init();

    tb_client_config_t tb_cfg = {
        .server_url = CLAIM_DEMO_MQTT_URL,
        .access_token = CLAIM_DEMO_USERNAME,
        .client_id = CLAIM_DEMO_CLIENT_ID,
        .device_name = CLAIM_DEMO_DEVICE_NAME,
    };

    rc = tb_client_init(&g_tb_client, &tb_cfg);
    if (rc != 0) {
        printf("[CLAIM_DEMO] FATAL: tb_client_init failed (%d)\n", rc);
        return 1;
    }

    mqtt_config_set_string(CLAIM_DEMO_PASSWORD, MQTT_CONFIG_VALUE_PASSWORD);

    while (1) {
        if (!tb_client_is_connected(g_tb_client)) {
            printf("[CLAIM_DEMO] Connecting...\n");
            rc = connect_and_wait();
            if (rc != 0) {
                printf("[CLAIM_DEMO] Reconnect in %d ms\n",
                       CLAIM_DEMO_RECONNECT_DELAY_MS);
                osal_task_delay_ms(CLAIM_DEMO_RECONNECT_DELAY_MS);
                continue;
            }

            printf("[CLAIM_DEMO] Connected\n");
            run_claim_round();
            last_round_ms = osal_task_get_time_ms();
        }

        uint32_t now = osal_task_get_time_ms();
        if ((uint32_t)(now - last_round_ms) >= CLAIM_DEMO_PERIOD_MS) {
            run_claim_round();
            last_round_ms = now;
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
