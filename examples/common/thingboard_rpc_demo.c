/*
 * ThingsBoard RPC Demo
 *
 * Demonstrates server-side RPC subscription/response and client-side RPC
 * request handling with asynchronous success/timeout callbacks.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "osal_task.h"

#include "tb_client.h"
#include "tb_rpc.h"

#include "cJSON.h"
#include "mongoose_process.h"
#include "mqtt_config.h"

#ifndef CONFIG_RPC_DEMO_CLIENT_ID
#define CONFIG_RPC_DEMO_CLIENT_ID "tb_rpc_demo_0001"
#endif
#ifndef CONFIG_RPC_DEMO_USERNAME
#define CONFIG_RPC_DEMO_USERNAME "tb_rpc_demo"
#endif
#ifndef CONFIG_RPC_DEMO_PASSWORD
#define CONFIG_RPC_DEMO_PASSWORD "tb_rpc_demo"
#endif
#ifndef CONFIG_RPC_DEMO_DEVICE_NAME
#define CONFIG_RPC_DEMO_DEVICE_NAME "RPC Demo"
#endif
#ifndef CONFIG_RPC_DEMO_MQTT_URL
#define CONFIG_RPC_DEMO_MQTT_URL "mqtt://localhost:1883"
#endif
#ifndef CONFIG_RPC_DEMO_REQUEST_PERIOD_MS
#define CONFIG_RPC_DEMO_REQUEST_PERIOD_MS 15000
#endif
#ifndef CONFIG_RPC_DEMO_REQUEST_TIMEOUT_MS
#define CONFIG_RPC_DEMO_REQUEST_TIMEOUT_MS 5000
#endif
#ifndef CONFIG_RPC_DEMO_RECONNECT_DELAY_MS
#define CONFIG_RPC_DEMO_RECONNECT_DELAY_MS 3000
#endif

#define RPC_DEMO_CLIENT_ID CONFIG_RPC_DEMO_CLIENT_ID
#define RPC_DEMO_USERNAME CONFIG_RPC_DEMO_USERNAME
#define RPC_DEMO_PASSWORD CONFIG_RPC_DEMO_PASSWORD
#define RPC_DEMO_DEVICE_NAME CONFIG_RPC_DEMO_DEVICE_NAME
#define RPC_DEMO_MQTT_URL CONFIG_RPC_DEMO_MQTT_URL
#define RPC_DEMO_REQUEST_PERIOD_MS CONFIG_RPC_DEMO_REQUEST_PERIOD_MS
#define RPC_DEMO_REQUEST_TIMEOUT_MS CONFIG_RPC_DEMO_REQUEST_TIMEOUT_MS
#define RPC_DEMO_RECONNECT_DELAY_MS CONFIG_RPC_DEMO_RECONNECT_DELAY_MS

static tb_client_t *g_tb_client;
static bool g_server_rpc_subscribed = false;

static void on_client_rpc_response(tb_request_result_t result,
                                   const char *response_json,
                                   void *user_data)
{
    const char *method = (const char *)user_data;

    if (result == TB_REQUEST_RESULT_SUCCESS && response_json != NULL) {
        printf("[RPC_DEMO] client RPC response for %s: %s\n",
               method != NULL ? method : "(null)", response_json);
        return;
    }

    printf("[RPC_DEMO] client RPC result for %s: %d\n",
           method != NULL ? method : "(null)", (int)result);
}

static void on_server_rpc(const char *method,
                          const char *params_json,
                          uint32_t request_id,
                          void *user_data)
{
    (void)user_data;

    printf("[RPC_DEMO] server RPC: method=%s request_id=%u params=%s\n",
           method != NULL ? method : "(null)", (unsigned)request_id,
           params_json != NULL ? params_json : "{}");

    if (method == NULL || method[0] == '\0') {
        (void)tb_rpc_respond(g_tb_client, request_id,
                             "{\"error\":\"empty method\"}");
        return;
    }

    if (strcmp(method, "setValue") == 0) {
        cJSON *root = cJSON_Parse(params_json != NULL ? params_json : "{}");
        if (root == NULL) {
            (void)tb_rpc_respond(g_tb_client, request_id,
                                 "{\"error\":\"invalid params json\"}");
            return;
        }

        cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (!cJSON_IsNumber(value)) {
            cJSON_Delete(root);
            (void)tb_rpc_respond(g_tb_client, request_id,
                                 "{\"error\":\"missing numeric value\"}");
            return;
        }

        char response[128];
        snprintf(response, sizeof(response),
                 "{\"success\":true,\"acceptedValue\":%d}", value->valueint);
        cJSON_Delete(root);
        (void)tb_rpc_respond(g_tb_client, request_id, response);
        return;
    }

    if (strcmp(method, "getStatus") == 0) {
        (void)tb_rpc_respond(g_tb_client, request_id,
                             "{\"status\":\"ok\",\"source\":\"rpc_demo\"}");
        return;
    }

    (void)tb_rpc_respond(g_tb_client, request_id,
                         "{\"error\":\"unknown method\"}");
}

static int connect_and_wait(void)
{
    int rc = tb_client_connect(g_tb_client);
    if (rc != 0) {
        printf("[RPC_DEMO] connect request failed: %d\n", rc);
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

static void subscribe_server_rpc_if_needed(void)
{
    if (g_server_rpc_subscribed) {
        return;
    }

    int rc = tb_rpc_subscribe_server(g_tb_client, on_server_rpc, NULL);
    if (rc != 0) {
        printf("[RPC_DEMO] server RPC subscribe failed: %d\n", rc);
        return;
    }

    g_server_rpc_subscribed = true;
    printf("[RPC_DEMO] server RPC subscription active\n");
}

static void send_client_rpc_request(void)
{
    const char *method = "getServerTime";
    const char *params = "{\"tz\":\"UTC\"}";

    int rc = tb_rpc_request(g_tb_client, method, params,
                            on_client_rpc_response, (void *)method,
                            RPC_DEMO_REQUEST_TIMEOUT_MS);
    if (rc != 0) {
        printf("[RPC_DEMO] client RPC request failed: %d\n", rc);
    } else {
        printf("[RPC_DEMO] client RPC request sent: method=%s\n", method);
    }
}

int main_function(void)
{
    int rc;
    uint32_t last_request_ms = 0;

    printf("\n=============================================\n");
    printf("   ThingsBoard RPC Demo\n");
    printf("=============================================\n");
    printf("  Client ID : %s\n", RPC_DEMO_CLIENT_ID);
    printf("  Broker    : %s\n", RPC_DEMO_MQTT_URL);
    printf("=============================================\n\n");

    MongooseProcess_Init();

    tb_client_config_t tb_cfg = {
        .server_url = RPC_DEMO_MQTT_URL,
        .access_token = RPC_DEMO_USERNAME,
        .client_id = RPC_DEMO_CLIENT_ID,
        .device_name = RPC_DEMO_DEVICE_NAME,
    };

    rc = tb_client_init(&g_tb_client, &tb_cfg);
    if (rc != 0) {
        printf("[RPC_DEMO] FATAL: tb_client_init failed (%d)\n", rc);
        return 1;
    }

    mqtt_config_set_string(RPC_DEMO_PASSWORD, MQTT_CONFIG_VALUE_PASSWORD);

    while (1) {
        if (!tb_client_is_connected(g_tb_client)) {
            printf("[RPC_DEMO] Connecting...\n");
            rc = connect_and_wait();
            if (rc != 0) {
                printf("[RPC_DEMO] Reconnect in %d ms\n",
                       RPC_DEMO_RECONNECT_DELAY_MS);
                osal_task_delay_ms(RPC_DEMO_RECONNECT_DELAY_MS);
                continue;
            }

            printf("[RPC_DEMO] Connected\n");
            subscribe_server_rpc_if_needed();
            send_client_rpc_request();
            last_request_ms = osal_task_get_time_ms();
        }

        uint32_t now = osal_task_get_time_ms();
        if ((uint32_t)(now - last_request_ms) >= RPC_DEMO_REQUEST_PERIOD_MS) {
            send_client_rpc_request();
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
