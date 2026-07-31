/*
 * ThingsBoard RGB Lamp Example
 *
 * Software-only RGB lamp example that communicates with ThingsBoard via MQTT.
 * No real hardware control – prints state changes with printf.
 *
 * Features:
 * - Subscribes to shared attributes (power, brightness, r, g, b, effect)
 * - Handles server-side RPC: setState, setBrightness, setColor, setEffect, getState
 * - Publishes telemetry every 5 seconds
 * - Thread-safe lamp state protected with OSAL mutex
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "osal_task.h"
#include "osal_mutex.h"
#include "osal_log.h"

#include "tb_client.h"
#include "tb_attributes.h"
#include "tb_rpc.h"
#include "tb_telemetry.h"

#include "mongoose_process.h"
#include "mqtt_config.h"
#include "cJSON.h"
#include "hq_config.h"

/* --------------------------------------------------------------------------
 * Configuration – values come from cmake -D flags (passed as CONFIG_LAMP_*).
 * Fall back to sensible defaults when not provided.
 *
 * Example:
 *   cmake -B build_posix ... -DLAMP_MQTT_URL="mqtt://192.168.1.100:1883"
 * -------------------------------------------------------------------------- */

#ifndef CONFIG_LAMP_CLIENT_ID
#define CONFIG_LAMP_CLIENT_ID    "rgb_lamp_0001"
#endif
#ifndef CONFIG_LAMP_USERNAME
#define CONFIG_LAMP_USERNAME     "test_light"
#endif
#ifndef CONFIG_LAMP_PASSWORD
#define CONFIG_LAMP_PASSWORD     "test_light"
#endif
#ifndef CONFIG_LAMP_DEVICE_NAME
#define CONFIG_LAMP_DEVICE_NAME  "RGB Lamp Example"
#endif
#ifndef CONFIG_LAMP_MQTT_URL
#define CONFIG_LAMP_MQTT_URL     "mqtt://localhost:1883"
#endif
#ifndef CONFIG_LAMP_TELEMETRY_PERIOD_MS
#define CONFIG_LAMP_TELEMETRY_PERIOD_MS   5000
#endif
#ifndef CONFIG_LAMP_RECONNECT_DELAY_MS
#define CONFIG_LAMP_RECONNECT_DELAY_MS    3000
#endif

#define LAMP_CLIENT_ID        CONFIG_LAMP_CLIENT_ID
#define LAMP_USERNAME         CONFIG_LAMP_USERNAME
#define LAMP_PASSWORD         CONFIG_LAMP_PASSWORD
#define LAMP_DEVICE_NAME      CONFIG_LAMP_DEVICE_NAME
#define LAMP_MQTT_URL         CONFIG_LAMP_MQTT_URL
#define TELEMETRY_PERIOD_MS   CONFIG_LAMP_TELEMETRY_PERIOD_MS
#define RECONNECT_DELAY_MS    CONFIG_LAMP_RECONNECT_DELAY_MS
#define TASK_STACK_SIZE       (64 * 1024)

/* --------------------------------------------------------------------------
 * Lamp state
 * -------------------------------------------------------------------------- */

typedef struct {
    bool power;
    int brightness;   /* 0..100 */
    int r;            /* 0..255 */
    int g;            /* 0..255 */
    int b;            /* 0..255 */
    char effect[32];  /* solid/blink/rainbow */
} lamp_state_t;

static lamp_state_t g_lamp = {
    .power = false,
    .brightness = 100,
    .r = 255,
    .g = 255,
    .b = 255,
    .effect = "solid"
};

static osal_mutex_id_t g_lamp_mutex;
static tb_client_t *g_tb_client;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static void lamp_print_state(const char *source)
{
    printf("[LAMP] Source: %s | power=%s brightness=%d "
           "r=%d g=%d b=%d effect=%s\n",
           source,
           g_lamp.power ? "ON" : "OFF",
           g_lamp.brightness,
           g_lamp.r, g_lamp.g, g_lamp.b,
           g_lamp.effect);
}

static void lamp_lock(void)
{
    osal_mutex_take(g_lamp_mutex);
}

static void lamp_unlock(void)
{
    osal_mutex_give(g_lamp_mutex);
}

static int clamp_int(int val, int min_val, int max_val)
{
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

/* Build JSON string for current lamp state (caller must hold lock) */
static void lamp_build_state_json(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size,
             "{\"power\":%s,\"brightness\":%d,"
             "\"r\":%d,\"g\":%d,\"b\":%d,\"effect\":\"%s\"}",
             g_lamp.power ? "true" : "false",
             g_lamp.brightness,
             g_lamp.r, g_lamp.g, g_lamp.b,
             g_lamp.effect);
}

/* --------------------------------------------------------------------------
 * Shared attributes callback
 * -------------------------------------------------------------------------- */

static void on_shared_attributes(const char *json_payload, void *user_data)
{
    (void)user_data;
    cJSON *root = cJSON_Parse(json_payload);
    if (!root) {
        printf("[LAMP] WARN: failed to parse shared attributes JSON\n");
        return;
    }

    lamp_lock();

    cJSON *item;

    item = cJSON_GetObjectItemCaseSensitive(root, "power");
    if (!item) item = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (item && cJSON_IsBool(item)) {
        g_lamp.power = cJSON_IsTrue(item) ? true : false;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (item && cJSON_IsNumber(item)) {
        g_lamp.brightness = clamp_int(item->valueint, 0, 100);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "r");
    if (item && cJSON_IsNumber(item)) {
        g_lamp.r = clamp_int(item->valueint, 0, 255);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "g");
    if (item && cJSON_IsNumber(item)) {
        g_lamp.g = clamp_int(item->valueint, 0, 255);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "b");
    if (item && cJSON_IsNumber(item)) {
        g_lamp.b = clamp_int(item->valueint, 0, 255);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "effect");
    if (item && cJSON_IsString(item) && item->valuestring) {
        strncpy(g_lamp.effect, item->valuestring, sizeof(g_lamp.effect) - 1);
        g_lamp.effect[sizeof(g_lamp.effect) - 1] = '\0';
    }

    lamp_print_state("shared_attribute");
    lamp_unlock();

    cJSON_Delete(root);
}

/* --------------------------------------------------------------------------
 * RPC handlers
 * -------------------------------------------------------------------------- */

static void rpc_handle_set_state(const char *params_json, uint32_t request_id)
{
    cJSON *root = cJSON_Parse(params_json);
    if (root) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "power");
        if (!item) item = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (item && cJSON_IsBool(item)) {
            lamp_lock();
            g_lamp.power = cJSON_IsTrue(item) ? true : false;
            lamp_print_state("rpc/setState");
            lamp_unlock();
        }
        cJSON_Delete(root);
    }
    tb_rpc_respond(g_tb_client, request_id, "{\"success\":true}");
}

static void rpc_handle_set_brightness(const char *params_json, uint32_t request_id)
{
    cJSON *root = cJSON_Parse(params_json);
    if (root) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "brightness");
        if (item && cJSON_IsNumber(item)) {
            lamp_lock();
            g_lamp.brightness = clamp_int(item->valueint, 0, 100);
            lamp_print_state("rpc/setBrightness");
            lamp_unlock();
        }
        cJSON_Delete(root);
    }
    tb_rpc_respond(g_tb_client, request_id, "{\"success\":true}");
}

static void rpc_handle_set_color(const char *params_json, uint32_t request_id)
{
    cJSON *root = cJSON_Parse(params_json);
    if (root) {
        lamp_lock();
        cJSON *item;
        item = cJSON_GetObjectItemCaseSensitive(root, "r");
        if (item && cJSON_IsNumber(item))
            g_lamp.r = clamp_int(item->valueint, 0, 255);
        item = cJSON_GetObjectItemCaseSensitive(root, "g");
        if (item && cJSON_IsNumber(item))
            g_lamp.g = clamp_int(item->valueint, 0, 255);
        item = cJSON_GetObjectItemCaseSensitive(root, "b");
        if (item && cJSON_IsNumber(item))
            g_lamp.b = clamp_int(item->valueint, 0, 255);
        lamp_print_state("rpc/setColor");
        lamp_unlock();
        cJSON_Delete(root);
    }
    tb_rpc_respond(g_tb_client, request_id, "{\"success\":true}");
}

static void rpc_handle_set_effect(const char *params_json, uint32_t request_id)
{
    cJSON *root = cJSON_Parse(params_json);
    if (root) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "effect");
        if (item && cJSON_IsString(item) && item->valuestring) {
            lamp_lock();
            strncpy(g_lamp.effect, item->valuestring,
                    sizeof(g_lamp.effect) - 1);
            g_lamp.effect[sizeof(g_lamp.effect) - 1] = '\0';
            lamp_print_state("rpc/setEffect");
            lamp_unlock();
        }
        cJSON_Delete(root);
    }
    tb_rpc_respond(g_tb_client, request_id, "{\"success\":true}");
}

static void rpc_handle_get_state(uint32_t request_id)
{
    char buf[256];
    lamp_lock();
    lamp_build_state_json(buf, sizeof(buf));
    lamp_unlock();
    tb_rpc_respond(g_tb_client, request_id, buf);
}

static void on_server_rpc(const char *method, const char *params_json,
                          uint32_t request_id, void *user_data)
{
    (void)user_data;
    printf("[LAMP] RPC request: method=%s params=%s id=%u\n",
           method, params_json, (unsigned)request_id);

    if (strcmp(method, "setState") == 0) {
        rpc_handle_set_state(params_json, request_id);
    } else if (strcmp(method, "setBrightness") == 0) {
        rpc_handle_set_brightness(params_json, request_id);
    } else if (strcmp(method, "setColor") == 0) {
        rpc_handle_set_color(params_json, request_id);
    } else if (strcmp(method, "setEffect") == 0) {
        rpc_handle_set_effect(params_json, request_id);
    } else if (strcmp(method, "getState") == 0) {
        rpc_handle_get_state(request_id);
    } else {
        printf("[LAMP] Unknown RPC method: %s\n", method);
        tb_rpc_respond(g_tb_client, request_id,
                       "{\"error\":\"unknown method\"}");
    }
}

/* --------------------------------------------------------------------------
 * Telemetry task
 * -------------------------------------------------------------------------- */

static void telemetry_task_func(void *arg)
{
    (void)arg;
    char buf[256];

    printf("[LAMP] Telemetry task started (period=%d ms)\n",
           TELEMETRY_PERIOD_MS);

    while (1) {
        osal_task_delay_ms(TELEMETRY_PERIOD_MS);

        if (!tb_client_is_connected(g_tb_client)) {
            continue;
        }

        lamp_lock();
        lamp_build_state_json(buf, sizeof(buf));
        lamp_unlock();

        int rc = tb_telemetry_send_json(g_tb_client, buf);
        if (rc != 0) {
            printf("[LAMP] Telemetry send failed: %d\n", rc);
        } else {
            printf("[LAMP] Telemetry sent: %s\n", buf);
        }
    }
}

/* --------------------------------------------------------------------------
 * Connection management
 * -------------------------------------------------------------------------- */

static int lamp_connect(void)
{
    int rc = tb_client_connect(g_tb_client);
    if (rc != 0) {
        printf("[LAMP] ERROR: connection failed (%d)\n", rc);
        return rc;
    }

    /* Wait for the actual MQTT connection to be established */
    for (int i = 0; i < 20; i++) {
        if (tb_client_is_connected(g_tb_client)) {
            break;
        }
        osal_task_delay_ms(250);
    }

    if (!tb_client_is_connected(g_tb_client)) {
        printf("[LAMP] ERROR: MQTT connection timed out\n");
        tb_client_disconnect(g_tb_client);
        return -1;
    }

    printf("[LAMP] Connected to ThingsBoard\n");

    /* Small settle delay to let the MQTT layer fully stabilise */
    osal_task_delay_ms(500);

    rc = tb_attributes_subscribe(g_tb_client, on_shared_attributes, NULL);
    if (rc != 0) {
        printf("[LAMP] WARN: shared attribute subscribe failed (%d)\n", rc);
    }

    rc = tb_rpc_subscribe_server(g_tb_client, on_server_rpc, NULL);
    if (rc != 0) {
        printf("[LAMP] WARN: server RPC subscribe failed (%d)\n", rc);
    }

    /* Send initial client attributes */
    tb_attributes_send_string(g_tb_client, "fw_version", "1.0.0");
    tb_attributes_send_string(g_tb_client, "device_type", "rgb_lamp");

    return 0;
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */

int main_function(void)
{
    osal_status_t os_rc;
    int rc;

    printf("\n=============================================\n");
    printf("   ThingsBoard RGB Lamp Example\n");
    printf("=============================================\n");
    printf("  Client ID : %s\n", LAMP_CLIENT_ID);
    printf("  Broker    : %s\n", LAMP_MQTT_URL);
    printf("=============================================\n\n");

    /* Create mutex for lamp state */
    os_rc = osal_mutex_create(&g_lamp_mutex, "lamp_state");
    if (os_rc != OSAL_SUCCESS) {
        printf("[LAMP] FATAL: mutex creation failed (%d)\n", os_rc);
        return 1;
    }

    /* Start Mongoose event loop (must be before tb_client_init) */
    MongooseProcess_Init();

    /* Initialize ThingsBoard client */
    tb_client_config_t tb_cfg = {
        .server_url = LAMP_MQTT_URL,
        .access_token = LAMP_USERNAME,
        .client_id = LAMP_CLIENT_ID,
        .device_name = LAMP_DEVICE_NAME
    };

    rc = tb_client_init(&g_tb_client, &tb_cfg);
    if (rc != 0) {
        printf("[LAMP] FATAL: tb_client_init failed (%d)\n", rc);
        return 1;
    }

    /* tb_client_init sets password to ""; override with actual password */
    mqtt_config_set_string(LAMP_PASSWORD, MQTT_CONFIG_VALUE_PASSWORD);

    /* Print initial state */
    lamp_lock();
    lamp_print_state("startup");
    lamp_unlock();

    /* Start telemetry task */
    osal_task_id_t telemetry_task;
    os_rc = osal_task_create(&telemetry_task, "lamp_telem",
                             telemetry_task_func, NULL,
                             NULL, TASK_STACK_SIZE, 5, NULL);
    if (os_rc != OSAL_SUCCESS) {
        printf("[LAMP] FATAL: telemetry task creation failed (%d)\n", os_rc);
        return 1;
    }

    /* Main loop: connect and maintain connection */
    while (1) {
        if (!tb_client_is_connected(g_tb_client)) {
            printf("[LAMP] Attempting connection...\n");
            rc = lamp_connect();
            if (rc != 0) {
                printf("[LAMP] Reconnect in %d ms\n", RECONNECT_DELAY_MS);
                osal_task_delay_ms(RECONNECT_DELAY_MS);
                continue;
            }
        }
        osal_task_delay_ms(1000);
    }

    /* Not reached */
    tb_client_disconnect(g_tb_client);
    tb_client_deinit(g_tb_client);
    osal_mutex_delete(g_lamp_mutex);
    return 0;
}

#ifndef CONFIG_HQ_PLATFORM_ESP
int main(void)
{
    return main_function();
}
#endif
