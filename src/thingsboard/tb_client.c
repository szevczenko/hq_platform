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
#include "tb_attributes.h"
#include "tb_rpc.h"

struct tb_client {
    tb_client_config_t config;
    osal_mutex_id_t    mutex;
    uint32_t           request_id;
    int                publish_qos_default;
    int                subscribe_qos_default;
    bool               connected;
    bool               mqtt_started;
    bool               initialized;
};

static tb_client_t *s_active_client = NULL;

#define TB_CLIENT_DEFAULT_PUBLISH_QOS 1
#define TB_CLIENT_DEFAULT_SUBSCRIBE_QOS 1
#define TB_CLIENT_DEFAULT_KEEPALIVE_SEC 60U
#define TB_CLIENT_DEFAULT_RECONNECT_INITIAL_DELAY_MS 30000U
#define TB_CLIENT_DEFAULT_RECONNECT_MAX_DELAY_MS 300000U
#define TB_CLIENT_MIN_KEEPALIVE_SEC 15U
#define TB_CLIENT_MAX_KEEPALIVE_SEC 1200U
#define TB_CLIENT_MIN_RECONNECT_DELAY_MS 1000U
#define TB_CLIENT_MAX_RECONNECT_DELAY_MS 3600000U

static uint16_t clamp_keepalive_sec(uint16_t value)
{
    if (value == 0) {
        return TB_CLIENT_DEFAULT_KEEPALIVE_SEC;
    }
    if (value < TB_CLIENT_MIN_KEEPALIVE_SEC) {
        return TB_CLIENT_MIN_KEEPALIVE_SEC;
    }
    if (value > TB_CLIENT_MAX_KEEPALIVE_SEC) {
        return TB_CLIENT_MAX_KEEPALIVE_SEC;
    }
    return value;
}

static uint32_t clamp_reconnect_delay_ms(uint32_t value, uint32_t fallback)
{
    if (value == 0) {
        return fallback;
    }
    if (value < TB_CLIENT_MIN_RECONNECT_DELAY_MS) {
        return TB_CLIENT_MIN_RECONNECT_DELAY_MS;
    }
    if (value > TB_CLIENT_MAX_RECONNECT_DELAY_MS) {
        return TB_CLIENT_MAX_RECONNECT_DELAY_MS;
    }
    return value;
}

static tb_client_disconnect_reason_t map_disconnect_reason(
    mqtt_disconnect_reason_t reason)
{
    switch (reason) {
    case MQTT_DISCONNECT_REASON_REMOTE_CLOSE:
        return TB_CLIENT_DISCONNECT_REASON_REMOTE_CLOSE;
    case MQTT_DISCONNECT_REASON_ERROR:
        return TB_CLIENT_DISCONNECT_REASON_ERROR;
    case MQTT_DISCONNECT_REASON_EXPLICIT:
    default:
        return TB_CLIENT_DISCONNECT_REASON_EXPLICIT;
    }
}

static tb_client_connect_failure_reason_t map_connect_failure_reason(
    mqtt_connect_failure_reason_t reason)
{
    switch (reason) {
    case MQTT_CONNECT_FAILURE_REASON_CONNECT_CREATE_FAILED:
        return TB_CLIENT_CONNECT_FAILURE_REASON_CONNECT_CREATE_FAILED;
    case MQTT_CONNECT_FAILURE_REASON_CONNACK_REJECTED:
        return TB_CLIENT_CONNECT_FAILURE_REASON_CONNACK_REJECTED;
    case MQTT_CONNECT_FAILURE_REASON_TRANSPORT_ERROR:
    default:
        return TB_CLIENT_CONNECT_FAILURE_REASON_TRANSPORT_ERROR;
    }
}

static void on_connect(void)
{
    tb_client_t *client = s_active_client;

    if (!client)
        return;

    client->connected = true;
    /* This is called from the Mongoose event loop context */
    osal_log_info("[tb] Connected to ThingsBoard");

    if (client->config.on_connect)
        client->config.on_connect(client,
                     client->config.connection_user_data);
}

static void on_disconnect(mqtt_disconnect_reason_t reason)
{
    tb_client_t *client = s_active_client;

    if (!client)
        return;

    client->connected = false;

    /* Reset module pending state tied to the dropped transport session. */
    tb_attributes_handle_disconnect(client);
    tb_rpc_handle_disconnect(client);

    osal_log_info("[tb] Disconnected from ThingsBoard");

    if (client->config.on_disconnect) {
        client->config.on_disconnect(
            client, map_disconnect_reason(reason),
            client->config.connection_user_data);
    }
}

static void on_connect_failure(mqtt_connect_failure_reason_t reason)
{
    tb_client_t *client = s_active_client;

    if (!client)
        return;

    osal_log_warning("[tb] ThingsBoard connection attempt failed reason=%d",
             (int)reason);

    if (client->config.on_connect_failure) {
        client->config.on_connect_failure(
            client, map_connect_failure_reason(reason),
            client->config.connection_user_data);
    }
}

int tb_client_init(tb_client_t **client, const tb_client_config_t *config)
{
    if (client == NULL || config == NULL) {
        return -1;
    }
    if (s_active_client != NULL) {
        osal_log_error("[tb] tb_client singleton already initialized");
        return -1;
    }
    if (config->access_token[0] == '\0' || config->server_url[0] == '\0') {
        return -1;
    }

    tb_client_t *ctx = calloc(1, sizeof(tb_client_t));
    if (ctx == NULL) {
        return -1;
    }

    memcpy(&ctx->config, config, sizeof(*config));
    ctx->config.server_url[TB_CLIENT_CONFIG_STR_SIZE - 1] = '\0';
    ctx->config.access_token[TB_CLIENT_CONFIG_STR_SIZE - 1] = '\0';
    ctx->config.client_id[TB_CLIENT_CONFIG_STR_SIZE - 1] = '\0';
    ctx->config.device_name[TB_CLIENT_CONFIG_STR_SIZE - 1] = '\0';
    ctx->request_id = 0;
    ctx->publish_qos_default = TB_CLIENT_DEFAULT_PUBLISH_QOS;
    ctx->subscribe_qos_default = TB_CLIENT_DEFAULT_SUBSCRIBE_QOS;
    ctx->connected = false;
    ctx->mqtt_started = false;

    if (ctx->config.use_custom_qos_defaults) {
        if (ctx->config.default_publish_qos <= 1) {
            ctx->publish_qos_default = (int)ctx->config.default_publish_qos;
        }
        if (ctx->config.default_subscribe_qos <= 1) {
            ctx->subscribe_qos_default =
                (int)ctx->config.default_subscribe_qos;
        }
    }

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

    mqtt_connection_policy_t policy = {
        .keepalive_sec = clamp_keepalive_sec(ctx->config.keepalive_sec),
        .reconnect_initial_delay_ms = clamp_reconnect_delay_ms(
            ctx->config.reconnect_initial_delay_ms,
            TB_CLIENT_DEFAULT_RECONNECT_INITIAL_DELAY_MS),
        .reconnect_max_delay_ms = clamp_reconnect_delay_ms(
            ctx->config.reconnect_max_delay_ms,
            TB_CLIENT_DEFAULT_RECONNECT_MAX_DELAY_MS),
        .reconnect_exponential_backoff =
            ctx->config.reconnect_exponential_backoff,
    };
    if (policy.reconnect_max_delay_ms < policy.reconnect_initial_delay_ms) {
        policy.reconnect_max_delay_ms = policy.reconnect_initial_delay_ms;
    }
    mqtt_app_set_connection_policy(&policy);

    mqtt_app_set_connect_callback(on_connect);
    mqtt_app_set_disconnect_callback(on_disconnect);
    mqtt_app_set_connect_failure_callback(on_connect_failure);

    ctx->initialized = true;
    s_active_client = ctx;
    *client = ctx;

    osal_log_info("[tb] ThingsBoard client initialized");
    return 0;
}

void tb_client_deinit(tb_client_t *client)
{
    if (client == NULL) {
        return;
    }
    tb_attributes_deinit(client);
    tb_rpc_deinit(client);
    if (client->mqtt_started) {
        tb_client_disconnect(client);
    }
    if (s_active_client == client) {
        s_active_client = NULL;
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
    client->mqtt_started = true;
    client->connected = mqtt_app_is_connected();
    osal_log_info("[tb] ThingsBoard client connect requested");
    return 0;
}

void tb_client_disconnect(tb_client_t *client)
{
    if (client == NULL) {
        return;
    }
    if (!client->mqtt_started) {
        return;
    }
    mqtt_app_deinit();
    client->mqtt_started = false;
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
    return tb_client_publish_with_qos(client, topic, json,
                                      client ? client->publish_qos_default :
                                               TB_CLIENT_DEFAULT_PUBLISH_QOS);
}

int tb_client_publish_with_qos(tb_client_t *client, const char *topic,
                               const char *json, int qos)
{
    if (client == NULL || topic == NULL || json == NULL) {
        return -1;
    }
    if (qos != 0 && qos != 1) {
        return -1;
    }
    if (!mqtt_app_is_connected()) {
        osal_log_warning("[tb] Cannot publish: not connected");
        return -1;
    }
    bool ok = mqtt_app_post_data(topic, json, qos);
    return ok ? 0 : -1;
}

int tb_client_subscribe(tb_client_t *client, const char *topic,
                        void (*callback)(const char *topic, const char *payload,
                                         size_t payload_len),
                        uint32_t timeout_ms)
{
    return tb_client_subscribe_with_qos(
        client, topic,
        client ? client->subscribe_qos_default :
                 TB_CLIENT_DEFAULT_SUBSCRIBE_QOS,
        callback, timeout_ms);
}

int tb_client_subscribe_with_qos(tb_client_t *client, const char *topic,
                                 int qos,
                                 void (*callback)(const char *topic,
                                                  const char *payload,
                                                  size_t payload_len),
                                 uint32_t timeout_ms)
{
    if (client == NULL || topic == NULL || callback == NULL) {
        return -1;
    }
    if (qos != 0 && qos != 1) {
        return -1;
    }
    bool ok = mqtt_app_subscribe(topic, qos, callback, timeout_ms);
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
