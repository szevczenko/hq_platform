/*
 * ThingsBoard Attributes Demo
 *
 * Demonstrates client/shared attribute requests, shared-attribute wildcard
 * subscription, and optional per-key shared-attribute callbacks.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "osal_task.h"

#include "tb_attributes.h"
#include "tb_client.h"

#include "mongoose_process.h"
#include "mqtt_config.h"

#ifndef CONFIG_ATTR_DEMO_CLIENT_ID
#define CONFIG_ATTR_DEMO_CLIENT_ID "tb_attr_demo_0001"
#endif
#ifndef CONFIG_ATTR_DEMO_USERNAME
#define CONFIG_ATTR_DEMO_USERNAME "tb_attr_demo"
#endif
#ifndef CONFIG_ATTR_DEMO_PASSWORD
#define CONFIG_ATTR_DEMO_PASSWORD "tb_attr_demo"
#endif
#ifndef CONFIG_ATTR_DEMO_DEVICE_NAME
#define CONFIG_ATTR_DEMO_DEVICE_NAME "Attributes Demo"
#endif
#ifndef CONFIG_ATTR_DEMO_MQTT_URL
#define CONFIG_ATTR_DEMO_MQTT_URL "mqtt://localhost:1883"
#endif
#ifndef CONFIG_ATTR_DEMO_REQUEST_PERIOD_MS
#define CONFIG_ATTR_DEMO_REQUEST_PERIOD_MS 10000
#endif
#ifndef CONFIG_ATTR_DEMO_RECONNECT_DELAY_MS
#define CONFIG_ATTR_DEMO_RECONNECT_DELAY_MS 3000
#endif
#ifndef CONFIG_ATTR_DEMO_ENABLE_KEY_SUBS
#define CONFIG_ATTR_DEMO_ENABLE_KEY_SUBS 1
#endif

#define ATTR_DEMO_CLIENT_ID CONFIG_ATTR_DEMO_CLIENT_ID
#define ATTR_DEMO_USERNAME CONFIG_ATTR_DEMO_USERNAME
#define ATTR_DEMO_PASSWORD CONFIG_ATTR_DEMO_PASSWORD
#define ATTR_DEMO_DEVICE_NAME CONFIG_ATTR_DEMO_DEVICE_NAME
#define ATTR_DEMO_MQTT_URL CONFIG_ATTR_DEMO_MQTT_URL
#define ATTR_DEMO_REQUEST_PERIOD_MS CONFIG_ATTR_DEMO_REQUEST_PERIOD_MS
#define ATTR_DEMO_RECONNECT_DELAY_MS CONFIG_ATTR_DEMO_RECONNECT_DELAY_MS

static tb_client_t *g_tb_client;
static bool g_subscribed = false;

#if CONFIG_ATTR_DEMO_ENABLE_KEY_SUBS
static tb_shared_attribute_subscription_t *g_threshold_sub = NULL;
static tb_shared_attribute_subscription_t *g_mode_sub = NULL;
#endif

static void on_attr_response(tb_request_result_t result,
                             const char *json_response,
                             void *user_data)
{
    const char *scope = (const char *)user_data;
    const char *scope_label = (scope != NULL) ? scope : "unknown";

    if (result == TB_REQUEST_RESULT_SUCCESS && json_response != NULL) {
        printf("[ATTR_DEMO] %s request response: %s\n", scope_label,
               json_response);
        return;
    }

    printf("[ATTR_DEMO] %s request failed or timeout (result=%d)\n",
           scope_label, (int)result);
}

static void on_shared_update(const char *json_payload, void *user_data)
{
    (void)user_data;
    printf("[ATTR_DEMO] shared update: %s\n",
           json_payload != NULL ? json_payload : "(null)");
}

#if CONFIG_ATTR_DEMO_ENABLE_KEY_SUBS
static void on_shared_key_update(const char *key,
                                 const char *json_payload,
                                 void *user_data)
{
    const char *label = (const char *)user_data;
    printf("[ATTR_DEMO] shared key update [%s] key=%s payload=%s\n",
           label != NULL ? label : "key-cb",
           key != NULL ? key : "(null)",
           json_payload != NULL ? json_payload : "(null)");
}
#endif

static int connect_and_wait(void)
{
    int rc = tb_client_connect(g_tb_client);
    if (rc != 0) {
        printf("[ATTR_DEMO] connect request failed: %d\n", rc);
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

static void subscribe_shared_updates_if_needed(void)
{
    int rc;

    if (g_subscribed) {
        return;
    }

    rc = tb_attributes_subscribe(g_tb_client, on_shared_update, NULL);
    if (rc != 0) {
        printf("[ATTR_DEMO] shared subscribe failed: %d\n", rc);
        return;
    }

#if CONFIG_ATTR_DEMO_ENABLE_KEY_SUBS
    rc = tb_attributes_subscribe_key(g_tb_client, "threshold",
                                     on_shared_key_update,
                                     (void *)"threshold",
                                     &g_threshold_sub);
    if (rc != 0) {
        printf("[ATTR_DEMO] threshold key subscribe failed: %d\n", rc);
    }

    rc = tb_attributes_subscribe_key(g_tb_client, "mode",
                                     on_shared_key_update,
                                     (void *)"mode",
                                     &g_mode_sub);
    if (rc != 0) {
        printf("[ATTR_DEMO] mode key subscribe failed: %d\n", rc);
    }
#endif

    g_subscribed = true;
    printf("[ATTR_DEMO] shared subscriptions active\n");
}

static void request_client_and_shared_attributes(void)
{
    static const char *client_keys[] = { "firmware_version", "serial_number" };
    static const char *shared_keys[] = { "threshold", "mode" };

    int rc = tb_attributes_request_client(g_tb_client,
                                          client_keys,
                                          sizeof(client_keys) / sizeof(client_keys[0]),
                                          on_attr_response,
                                          (void *)"client",
                                          5000);
    if (rc != 0) {
        printf("[ATTR_DEMO] client attribute request failed: %d\n", rc);
    }

    rc = tb_attributes_request_shared(g_tb_client,
                                      shared_keys,
                                      sizeof(shared_keys) / sizeof(shared_keys[0]),
                                      on_attr_response,
                                      (void *)"shared",
                                      5000);
    if (rc != 0) {
        printf("[ATTR_DEMO] shared attribute request failed: %d\n", rc);
    }
}

static void publish_demo_client_attributes(void)
{
    int rc = tb_attributes_send_json(
        g_tb_client,
        "{\"firmware_version\":\"1.0.0\",\"serial_number\":\"ATTR-DEMO-001\"}");
    if (rc != 0) {
        printf("[ATTR_DEMO] client attribute publish failed: %d\n", rc);
    }
}

int main_function(void)
{
    int rc;
    uint32_t last_request_ms = 0;

    printf("\n=============================================\n");
    printf("   ThingsBoard Attributes Demo\n");
    printf("=============================================\n");
    printf("  Client ID : %s\n", ATTR_DEMO_CLIENT_ID);
    printf("  Broker    : %s\n", ATTR_DEMO_MQTT_URL);
    printf("=============================================\n\n");

    MongooseProcess_Init();

    tb_client_config_t tb_cfg = {
        .server_url = ATTR_DEMO_MQTT_URL,
        .access_token = ATTR_DEMO_USERNAME,
        .client_id = ATTR_DEMO_CLIENT_ID,
        .device_name = ATTR_DEMO_DEVICE_NAME,
    };

    rc = tb_client_init(&g_tb_client, &tb_cfg);
    if (rc != 0) {
        printf("[ATTR_DEMO] FATAL: tb_client_init failed (%d)\n", rc);
        return 1;
    }

    mqtt_config_set_string(ATTR_DEMO_PASSWORD, MQTT_CONFIG_VALUE_PASSWORD);

    while (1) {
        if (!tb_client_is_connected(g_tb_client)) {
            printf("[ATTR_DEMO] Connecting...\n");
            rc = connect_and_wait();
            if (rc != 0) {
                printf("[ATTR_DEMO] Reconnect in %d ms\n",
                       ATTR_DEMO_RECONNECT_DELAY_MS);
                osal_task_delay_ms(ATTR_DEMO_RECONNECT_DELAY_MS);
                continue;
            }

            printf("[ATTR_DEMO] Connected\n");
            subscribe_shared_updates_if_needed();
            publish_demo_client_attributes();
            request_client_and_shared_attributes();
            last_request_ms = osal_task_get_time_ms();
        }

        uint32_t now = osal_task_get_time_ms();
        if ((uint32_t)(now - last_request_ms) >= ATTR_DEMO_REQUEST_PERIOD_MS) {
            request_client_and_shared_attributes();
            last_request_ms = now;
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
