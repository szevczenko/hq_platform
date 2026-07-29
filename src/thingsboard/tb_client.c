/**
 *******************************************************************************
 * @file    tb_client.c
 * @brief   ThingsBoard client – core implementation
 *******************************************************************************
 */

#include "tb_client.h"

#include <stdlib.h>
#include <string.h>

#include "mqtt_app.h"
#include "mqtt_config.h"
#include "osal_mutex.h"
#include "osal_log.h"

struct tb_client {
    tb_client_config_t config;
    osal_mutex_id_t    mutex;
    uint32_t           request_id;
    bool               connected;
    bool               initialized;
};

static void on_connect(void)
{
    /* This is called from the Mongoose event loop context */
    osal_log_info("[tb] Connected to ThingsBoard");
}

static void on_disconnect(void)
{
    osal_log_info("[tb] Disconnected from ThingsBoard");
}

int tb_client_init(tb_client_t **client, const tb_client_config_t *config)
{
    if (client == NULL || config == NULL) {
        return -1;
    }
    if (config->access_token[0] == '\0' || config->server_url[0] == '\0') {
        return -1;
    }

    tb_client_t *ctx = calloc(1, sizeof(tb_client_t));
    if (ctx == NULL) {
        return -1;
    }

    ctx->config = *config;
    ctx->config.server_url[TB_CLIENT_CONFIG_STR_SIZE - 1] = '\0';
    ctx->config.access_token[TB_CLIENT_CONFIG_STR_SIZE - 1] = '\0';
    ctx->config.client_id[TB_CLIENT_CONFIG_STR_SIZE - 1] = '\0';
    ctx->config.device_name[TB_CLIENT_CONFIG_STR_SIZE - 1] = '\0';
    ctx->request_id = 0;
    ctx->connected = false;

    if (osal_mutex_create(&ctx->mutex, "tb_client") != OSAL_SUCCESS) {
        free(ctx);
        return -1;
    }

    /* Configure the MQTT connection parameters */
    mqtt_config_init();
    mqtt_config_set_string(ctx->config.server_url, MQTT_CONFIG_VALUE_ADDRESS);
    mqtt_config_set_string(ctx->config.access_token, MQTT_CONFIG_VALUE_USERNAME);
    mqtt_config_set_string("", MQTT_CONFIG_VALUE_PASSWORD);

    if (ctx->config.client_id[0] != '\0') {
        mqtt_config_set_string(ctx->config.client_id, MQTT_CONFIG_VALUE_CLIENT_ID);
    } else {
        mqtt_config_set_string(ctx->config.access_token, MQTT_CONFIG_VALUE_CLIENT_ID);
    }

    mqtt_app_set_connect_callback(on_connect);
    mqtt_app_set_disconnect_callback(on_disconnect);

    ctx->initialized = true;
    *client = ctx;

    osal_log_info("[tb] ThingsBoard client initialized");
    return 0;
}

void tb_client_deinit(tb_client_t *client)
{
    if (client == NULL) {
        return;
    }
    if (client->connected) {
        tb_client_disconnect(client);
    }
    osal_mutex_delete(client->mutex);
    free(client);
}

int tb_client_connect(tb_client_t *client)
{
    if (client == NULL || !client->initialized) {
        return -1;
    }

    mqtt_app_init();
    client->connected = true;
    osal_log_info("[tb] ThingsBoard client connect requested");
    return 0;
}

void tb_client_disconnect(tb_client_t *client)
{
    if (client == NULL) {
        return;
    }
    mqtt_app_deinit();
    client->connected = false;
    osal_log_info("[tb] ThingsBoard client disconnected");
}

bool tb_client_is_connected(tb_client_t *client)
{
    if (client == NULL) {
        return false;
    }
    return mqtt_app_is_connected();
}

uint32_t tb_client_get_next_request_id(tb_client_t *client)
{
    if (client == NULL) {
        return 0;
    }
    uint32_t id;
    osal_mutex_take(client->mutex);
    client->request_id++;
    id = client->request_id;
    osal_mutex_give(client->mutex);
    return id;
}

int tb_client_publish(tb_client_t *client, const char *topic, const char *json)
{
    if (client == NULL || topic == NULL || json == NULL) {
        return -1;
    }
    if (!mqtt_app_is_connected()) {
        osal_log_warning("[tb] Cannot publish: not connected");
        return -1;
    }
    bool ok = mqtt_app_post_data(topic, json, 1);
    return ok ? 0 : -1;
}

int tb_client_subscribe(tb_client_t *client, const char *topic,
                        void (*callback)(const char *topic, const char *payload,
                                         size_t payload_len),
                        uint32_t timeout_ms)
{
    if (client == NULL || topic == NULL || callback == NULL) {
        return -1;
    }
    bool ok = mqtt_app_subscribe(topic, 1, callback, timeout_ms);
    return ok ? 0 : -1;
}

int tb_client_unsubscribe(tb_client_t *client, const char *topic,
                          uint32_t timeout_ms)
{
    if (client == NULL || topic == NULL) {
        return -1;
    }
    bool ok = mqtt_app_unsubscribe(topic, timeout_ms);
    return ok ? 0 : -1;
}
