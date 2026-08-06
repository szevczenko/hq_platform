/**
 *******************************************************************************
 * @file    tb_client.c
 * @brief   ThingsBoard client – core implementation
 *******************************************************************************
 */

#include "tb_client.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mqtt_app.h"
#include "mqtt_config.h"
#include "osal_mutex.h"
#include "osal_log.h"
#include "osal_task.h"
#include "tb_attributes.h"
#include "tb_provision.h"
#include "tb_rpc.h"

typedef struct {
    uint32_t message_rate;
    uint32_t telemetry_rate;
    uint32_t telemetry_datapoints_rate;
    uint32_t max_payload_size;
    uint32_t max_inflight_messages;
} tb_session_limits_t;

typedef struct {
    double tokens;
    double rate_per_sec;
    double capacity;
    uint32_t last_update_ms;
} tb_token_bucket_t;

typedef struct {
    char topic[256];
    char payload[1024];
    int qos;
    bool active;
} tb_deferred_message_t;

struct tb_client {
    tb_client_config_t config;
    osal_mutex_id_t    mutex;
    uint32_t           request_id;
    int                publish_qos_default;
    int                subscribe_qos_default;
    tb_session_limits_t limits;
    tb_token_bucket_t msg_bucket;
    tb_token_bucket_t datapoint_bucket;
    tb_deferred_message_t deferred[16];
    uint16_t deferred_capacity;
    uint16_t deferred_head;
    uint16_t deferred_tail;
    uint16_t deferred_count;
    bool session_limits_request_inflight;
    bool session_limits_ready;
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
#define TB_CLIENT_DEFAULT_DEFER_QUEUE_CAPACITY 8U
#define TB_CLIENT_MAX_DEFER_QUEUE_CAPACITY 16U

#define TB_DEFAULT_LIMIT_MESSAGE_RATE 100U
#define TB_DEFAULT_LIMIT_TELEMETRY_RATE 100U
#define TB_DEFAULT_LIMIT_TELEMETRY_DATAPOINTS_RATE 500U
#define TB_DEFAULT_LIMIT_MAX_PAYLOAD_SIZE 8192U
#define TB_DEFAULT_LIMIT_MAX_INFLIGHT_MESSAGES 16U

#define TB_TOPIC_TELEMETRY "v1/devices/me/telemetry"
#define TB_TOPIC_ATTRIBUTES "v1/devices/me/attributes"
#define TB_TOPIC_RPC_REQUEST_PREFIX "v1/devices/me/rpc/request/"

static void maybe_request_session_limits(tb_client_t *client);
static void flush_deferred_queue(tb_client_t *client);

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

static uint16_t clamp_defer_queue_capacity(uint16_t value)
{
    if (value == 0) {
        return TB_CLIENT_DEFAULT_DEFER_QUEUE_CAPACITY;
    }
    if (value > TB_CLIENT_MAX_DEFER_QUEUE_CAPACITY) {
        return TB_CLIENT_MAX_DEFER_QUEUE_CAPACITY;
    }
    return value;
}

static bool topic_is_rate_limited(const char *topic)
{
    return strcmp(topic, TB_TOPIC_TELEMETRY) == 0 ||
           strcmp(topic, TB_TOPIC_ATTRIBUTES) == 0;
}

static bool topic_is_internal_control(const char *topic)
{
    if (strncmp(topic, TB_TOPIC_RPC_REQUEST_PREFIX,
                strlen(TB_TOPIC_RPC_REQUEST_PREFIX)) == 0) {
        return true;
    }

    if (strncmp(topic, "v1/devices/me/attributes/request/",
                strlen("v1/devices/me/attributes/request/")) == 0) {
        return true;
    }

    return false;
}

static uint32_t count_top_level_datapoints(const char *json)
{
    uint32_t count = 1;
    cJSON *root;

    if (json == NULL) {
        return 0;
    }

    root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        return 1;
    }

    count = 0;
    for (cJSON *item = root->child; item != NULL; item = item->next) {
        count++;
    }

    cJSON_Delete(root);
    return count > 0 ? count : 1;
}

static void bucket_init(tb_token_bucket_t *bucket, double rate_per_sec,
                        uint32_t now_ms)
{
    bucket->rate_per_sec = rate_per_sec;
    bucket->capacity = rate_per_sec;
    bucket->tokens = rate_per_sec;
    bucket->last_update_ms = now_ms;
}

static void bucket_refill(tb_token_bucket_t *bucket, uint32_t now_ms)
{
    if (now_ms <= bucket->last_update_ms) {
        return;
    }

    double elapsed_sec = (double)(now_ms - bucket->last_update_ms) / 1000.0;
    bucket->tokens += elapsed_sec * bucket->rate_per_sec;
    if (bucket->tokens > bucket->capacity) {
        bucket->tokens = bucket->capacity;
    }
    bucket->last_update_ms = now_ms;
}

static bool bucket_consume(tb_token_bucket_t *bucket, double amount,
                           uint32_t now_ms)
{
    bucket_refill(bucket, now_ms);
    if (bucket->tokens + 1e-9 < amount) {
        return false;
    }
    bucket->tokens -= amount;
    return true;
}

static void apply_default_session_limits(tb_client_t *client)
{
    uint32_t now_ms = osal_task_get_time_ms();

    client->limits.message_rate = TB_DEFAULT_LIMIT_MESSAGE_RATE;
    client->limits.telemetry_rate = TB_DEFAULT_LIMIT_TELEMETRY_RATE;
    client->limits.telemetry_datapoints_rate =
        TB_DEFAULT_LIMIT_TELEMETRY_DATAPOINTS_RATE;
    client->limits.max_payload_size = TB_DEFAULT_LIMIT_MAX_PAYLOAD_SIZE;
    client->limits.max_inflight_messages = TB_DEFAULT_LIMIT_MAX_INFLIGHT_MESSAGES;

    bucket_init(&client->msg_bucket, (double)client->limits.message_rate,
                now_ms);
    bucket_init(&client->datapoint_bucket,
                (double)client->limits.telemetry_datapoints_rate, now_ms);
}

static bool queue_deferred_message(tb_client_t *client, const char *topic,
                                   const char *payload, int qos)
{
    if (client->deferred_count >= client->deferred_capacity) {
        return false;
    }

    /* Reject messages that do not fit the slot buffers instead of truncating. */
    if (strlen(topic) >= sizeof(((tb_deferred_message_t *)0)->topic) ||
        strlen(payload) >= sizeof(((tb_deferred_message_t *)0)->payload)) {
        return false;
    }

    tb_deferred_message_t *slot = &client->deferred[client->deferred_tail];
    strncpy(slot->topic, topic, sizeof(slot->topic) - 1);
    slot->topic[sizeof(slot->topic) - 1] = '\0';
    strncpy(slot->payload, payload, sizeof(slot->payload) - 1);
    slot->payload[sizeof(slot->payload) - 1] = '\0';
    slot->qos = qos;
    slot->active = true;

    client->deferred_tail =
        (uint16_t)((client->deferred_tail + 1) % client->deferred_capacity);
    client->deferred_count++;
    return true;
}

static bool send_direct_now(tb_client_t *client, const char *topic,
                            const char *payload, int qos)
{
    if (!mqtt_app_is_connected()) {
        return false;
    }

    if (client->config.enable_session_limits && topic_is_rate_limited(topic)) {
        uint32_t now_ms = osal_task_get_time_ms();
        uint32_t datapoints = count_top_level_datapoints(payload);

        if (!bucket_consume(&client->msg_bucket, 1.0, now_ms)) {
            return false;
        }

        if (strcmp(topic, TB_TOPIC_TELEMETRY) == 0 &&
            !bucket_consume(&client->datapoint_bucket, (double)datapoints,
                            now_ms)) {
            /* Refund the message token consumed above. */
            client->msg_bucket.tokens += 1.0;
            if (client->msg_bucket.tokens > client->msg_bucket.capacity) {
                client->msg_bucket.tokens = client->msg_bucket.capacity;
            }
            return false;
        }
    }

    if (!mqtt_app_post_data(topic, payload, qos)) {
        /* Refund consumed tokens so a transport failure does not drain the limiter. */
        if (client->config.enable_session_limits && topic_is_rate_limited(topic)) {
            client->msg_bucket.tokens += 1.0;
            if (client->msg_bucket.tokens > client->msg_bucket.capacity) {
                client->msg_bucket.tokens = client->msg_bucket.capacity;
            }
        }
        return false;
    }

    return true;
}

static void flush_deferred_queue(tb_client_t *client)
{
    while (client->deferred_count > 0) {
        tb_deferred_message_t *msg = &client->deferred[client->deferred_head];
        if (!msg->active) {
            client->deferred_head =
                (uint16_t)((client->deferred_head + 1) %
                           client->deferred_capacity);
            client->deferred_count--;
            continue;
        }

        if (!send_direct_now(client, msg->topic, msg->payload, msg->qos)) {
            break;
        }

        msg->active = false;
        client->deferred_head =
            (uint16_t)((client->deferred_head + 1) % client->deferred_capacity);
        client->deferred_count--;
    }
}

static bool enqueue_or_send(tb_client_t *client, const char *topic,
                            const char *payload, int qos)
{
    if (send_direct_now(client, topic, payload, qos)) {
        return true;
    }

    if (!client->config.enable_session_limits || topic_is_internal_control(topic)) {
        return false;
    }

    return queue_deferred_message(client, topic, payload, qos);
}

static int publish_split_object_if_needed(tb_client_t *client,
                                          const char *topic,
                                          const char *json,
                                          int qos)
{
    cJSON *root;
    cJSON *chunk;
    uint32_t datapoints_limit;
    uint32_t payload_limit;
    uint32_t chunk_points = 0;
    int sent_chunks = 0;

    if (!client->config.enable_session_limits ||
        !topic_is_rate_limited(topic)) {
        return enqueue_or_send(client, topic, json, qos) ? 0 : -1;
    }

    payload_limit = client->limits.max_payload_size;
    if (payload_limit == 0 || strlen(json) <= payload_limit) {
        return enqueue_or_send(client, topic, json, qos) ? 0 : -1;
    }

    root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        return -1;
    }

    datapoints_limit = client->limits.telemetry_datapoints_rate;
    if (datapoints_limit == 0) {
        datapoints_limit = 1;
    }

    chunk = cJSON_CreateObject();
    if (chunk == NULL) {
        cJSON_Delete(root);
        return -1;
    }

    for (cJSON *item = root->child; item != NULL; item = item->next) {
        cJSON *dup = cJSON_Duplicate(item, true);
        if (dup == NULL || !cJSON_AddItemToObject(chunk, item->string, dup)) {
            if (dup != NULL) {
                cJSON_Delete(dup);
            }
            cJSON_Delete(chunk);
            cJSON_Delete(root);
            return -1;
        }
        chunk_points++;

        char *candidate = cJSON_PrintUnformatted(chunk);
        bool chunk_full = false;
        if (candidate == NULL) {
            cJSON_Delete(chunk);
            cJSON_Delete(root);
            return -1;
        }

        if (strlen(candidate) > payload_limit || chunk_points >= datapoints_limit) {
            cJSON_DeleteItemFromObjectCaseSensitive(chunk, item->string);
            cJSON_free(candidate);

            char *emit = cJSON_PrintUnformatted(chunk);
            if (emit == NULL) {
                cJSON_Delete(chunk);
                cJSON_Delete(root);
                return -1;
            }
            if (!enqueue_or_send(client, topic, emit, qos)) {
                cJSON_free(emit);
                cJSON_Delete(chunk);
                cJSON_Delete(root);
                return -1;
            }
            cJSON_free(emit);
            sent_chunks++;

            cJSON_Delete(chunk);
            chunk = cJSON_CreateObject();
            if (chunk == NULL) {
                cJSON_Delete(root);
                return -1;
            }
            dup = cJSON_Duplicate(item, true);
            if (dup == NULL || !cJSON_AddItemToObject(chunk, item->string, dup)) {
                if (dup != NULL) {
                    cJSON_Delete(dup);
                }
                cJSON_Delete(chunk);
                cJSON_Delete(root);
                return -1;
            }
            chunk_points = 1;
            chunk_full = true;
        }

        if (!chunk_full) {
            cJSON_free(candidate);
        }
    }

    if (chunk->child != NULL) {
        char *emit = cJSON_PrintUnformatted(chunk);
        if (emit == NULL) {
            cJSON_Delete(chunk);
            cJSON_Delete(root);
            return -1;
        }
        if (!enqueue_or_send(client, topic, emit, qos)) {
            cJSON_free(emit);
            cJSON_Delete(chunk);
            cJSON_Delete(root);
            return -1;
        }
        cJSON_free(emit);
        sent_chunks++;
    }

    cJSON_Delete(chunk);
    cJSON_Delete(root);
    return sent_chunks > 0 ? 0 : -1;
}

static uint32_t get_u32_field(cJSON *obj, const char *name, uint32_t fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsNumber(item) && item->valuedouble > 0) {
        return (uint32_t)item->valuedouble;
    }
    return fallback;
}

static void on_session_limits_response(tb_request_result_t result,
                                       const char *response_json,
                                       void *user_data)
{
    tb_client_t *client = (tb_client_t *)user_data;
    uint32_t now_ms;

    if (client == NULL) {
        return;
    }

    client->session_limits_request_inflight = false;

    if (result != TB_REQUEST_RESULT_SUCCESS || response_json == NULL) {
        return;
    }

    cJSON *root = cJSON_Parse(response_json);
    if (root == NULL) {
        return;
    }

    cJSON *limits = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsObject(limits)) {
        limits = root;
    }

    client->limits.message_rate = get_u32_field(
        limits, "maxMessageRate", client->limits.message_rate);
    client->limits.telemetry_rate = get_u32_field(
        limits, "maxTelemetryRate", client->limits.telemetry_rate);
    client->limits.telemetry_datapoints_rate = get_u32_field(
        limits, "maxTelemetryDataPointsRate",
        client->limits.telemetry_datapoints_rate);
    client->limits.max_payload_size = get_u32_field(
        limits, "maxPayloadSize", client->limits.max_payload_size);
    client->limits.max_inflight_messages = get_u32_field(
        limits, "maxInflightMessages", client->limits.max_inflight_messages);

    cJSON_Delete(root);

    now_ms = osal_task_get_time_ms();
    bucket_init(&client->msg_bucket, (double)client->limits.message_rate,
                now_ms);
    bucket_init(&client->datapoint_bucket,
                (double)client->limits.telemetry_datapoints_rate, now_ms);

    client->session_limits_ready = true;
    flush_deferred_queue(client);
}

static void maybe_request_session_limits(tb_client_t *client)
{
    if (client == NULL || !client->config.enable_session_limits) {
        return;
    }
    if (client->session_limits_request_inflight || !client->connected) {
        return;
    }

    client->session_limits_request_inflight = true;
    if (tb_rpc_request(client, "getSessionLimits", "{}",
                       on_session_limits_response, client, 5000) != 0) {
        client->session_limits_request_inflight = false;
    }
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
    flush_deferred_queue(client);
    maybe_request_session_limits(client);
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
    client->session_limits_request_inflight = false;

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
    ctx->deferred_capacity = clamp_defer_queue_capacity(config->defer_queue_capacity);
    ctx->deferred_head = 0;
    ctx->deferred_tail = 0;
    ctx->deferred_count = 0;
    ctx->session_limits_request_inflight = false;
    ctx->session_limits_ready = false;
    apply_default_session_limits(ctx);

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
    tb_provision_deinit(client);
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
    if (client->connected) {
        flush_deferred_queue(client);
        maybe_request_session_limits(client);
    }
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

    flush_deferred_queue(client);

    if (client->config.enable_session_limits && topic_is_rate_limited(topic)) {
        if (strlen(json) > client->limits.max_payload_size) {
            return publish_split_object_if_needed(client, topic, json, qos);
        }
    }

    if (!enqueue_or_send(client, topic, json, qos)) {
        if (!mqtt_app_is_connected()) {
            osal_log_warning("[tb] Cannot publish: not connected");
        } else {
            osal_log_warning("[tb] Publish deferred queue full or throttled");
        }
        return -1;
    }

    return 0;
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
