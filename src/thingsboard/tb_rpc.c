/**
 *******************************************************************************
 * @file    tb_rpc.c
 * @brief   ThingsBoard client – RPC implementation (server-side and client-side)
 *******************************************************************************
 */

#include "tb_rpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "cJSON.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "osal_timer.h"



/* ThingsBoard RPC topics */
#define TB_RPC_SERVER_SUB_TOPIC   "v1/devices/me/rpc/request/+"
#define TB_RPC_SERVER_REQ_TOPIC   "v1/devices/me/rpc/request/"
#define TB_RPC_SERVER_RESP_TOPIC  "v1/devices/me/rpc/response/%"PRIu32
#define TB_RPC_CLIENT_REQ_TOPIC   "v1/devices/me/rpc/request/%"PRIu32
#define TB_RPC_CLIENT_RESP_SUB    "v1/devices/me/rpc/response/+"
#define TB_RPC_CLIENT_RESP_TOPIC  "v1/devices/me/rpc/response/"

#define TB_RPC_MAX_TOPIC_LEN  128
#define TB_RPC_TIMEOUT_MS     5000
#define TB_RPC_MAX_PENDING    8
#define TB_RPC_SWEEP_PERIOD_MS 50

/* Server-side RPC state */
static tb_server_rpc_cb_t s_server_rpc_cb = NULL;
static void *s_server_rpc_user_data = NULL;
static bool s_server_rpc_subscribed = false;

/* Client-side RPC pending requests */
typedef struct {
    uint32_t request_id;
    uint32_t deadline_ms;
    tb_client_rpc_cb_t cb;
    void *user_data;
    bool active;
} tb_rpc_pending_t;

static tb_rpc_pending_t s_rpc_pending[TB_RPC_MAX_PENDING];
static osal_mutex_id_t s_rpc_mutex;
static bool s_rpc_init = false;
static bool s_rpc_init_failed = false;
static osal_timer_id_t s_rpc_timeout_timer;
static bool s_client_rpc_subscribed = false;
static tb_client_t *s_owner_client = NULL;

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void complete_pending_requests(tb_request_result_t result)
{
    tb_client_rpc_cb_t callbacks[TB_RPC_MAX_PENDING] = { 0 };
    void *user_data[TB_RPC_MAX_PENDING] = { 0 };
    int callback_count = 0;

    if (!s_rpc_init) {
        return;
    }

    osal_mutex_take(s_rpc_mutex);
    for (int i = 0; i < TB_RPC_MAX_PENDING; i++) {
        if (!s_rpc_pending[i].active) {
            continue;
        }

        callbacks[callback_count] = s_rpc_pending[i].cb;
        user_data[callback_count] = s_rpc_pending[i].user_data;
        callback_count++;
        s_rpc_pending[i].active = false;
    }
    osal_mutex_give(s_rpc_mutex);

    for (int i = 0; i < callback_count; i++) {
        if (callbacks[i] != NULL) {
            callbacks[i](result, NULL, user_data[i]);
        }
    }
}

static void rpc_timeout_timer_cb(osal_timer_id_t timer_id)
{
    tb_client_rpc_cb_t callbacks[TB_RPC_MAX_PENDING] = { 0 };
    void *user_data[TB_RPC_MAX_PENDING] = { 0 };
    int callback_count = 0;
    uint32_t now_ms;

    (void)timer_id;

    if (!s_rpc_init) {
        return;
    }

    now_ms = osal_task_get_time_ms();

    osal_mutex_take(s_rpc_mutex);
    for (int i = 0; i < TB_RPC_MAX_PENDING; i++) {
        if (!s_rpc_pending[i].active ||
            !time_reached(now_ms, s_rpc_pending[i].deadline_ms)) {
            continue;
        }

        callbacks[callback_count] = s_rpc_pending[i].cb;
        user_data[callback_count] = s_rpc_pending[i].user_data;
        callback_count++;
        s_rpc_pending[i].active = false;
    }
    osal_mutex_give(s_rpc_mutex);

    for (int i = 0; i < callback_count; i++) {
        if (callbacks[i] != NULL) {
            callbacks[i](TB_REQUEST_RESULT_TIMEOUT, NULL, user_data[i]);
        }
    }
}

static void teardown_rpc_resources(void)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void complete_pending_requests(tb_request_result_t result)
{
    tb_client_rpc_cb_t callbacks[TB_RPC_MAX_PENDING] = { 0 };
    void *user_data[TB_RPC_MAX_PENDING] = { 0 };
    int callback_count = 0;

    if (!s_rpc_init) {
        return;
    }

    (void)osal_timer_stop(s_rpc_timeout_timer, 0);
    (void)osal_timer_delete(s_rpc_timeout_timer, 0);
    (void)osal_mutex_delete(s_rpc_mutex);
    memset(s_rpc_pending, 0, sizeof(s_rpc_pending));
    s_rpc_init = false;
}

static bool ensure_rpc_init(void)
{
    if (s_rpc_init) {
        return true;
    }

    if (s_rpc_init_failed) {
        return false;
    }

    if (osal_mutex_create(&s_rpc_mutex, "tb_rpc") != OSAL_SUCCESS) {
        s_rpc_init_failed = true;
        return false;
    }

    if (osal_timer_create(&s_rpc_timeout_timer, "tb_rpc_timeout",
                          TB_RPC_SWEEP_PERIOD_MS, true,
                          rpc_timeout_timer_cb, NULL, NULL,
                          0) != OSAL_SUCCESS) {
        (void)osal_mutex_delete(s_rpc_mutex);
        s_rpc_init_failed = true;
        return false;
    }

    memset(s_rpc_pending, 0, sizeof(s_rpc_pending));
    if (osal_timer_start(s_rpc_timeout_timer, 0) != OSAL_SUCCESS) {
        (void)osal_timer_delete(s_rpc_timeout_timer, 0);
        (void)osal_mutex_delete(s_rpc_mutex);
        s_rpc_init_failed = true;
        return false;
    }
    s_rpc_init = true;
    return true;
}

static void server_rpc_handler(const char *topic, const char *payload,
                               size_t payload_len)
{
    if (s_server_rpc_cb == NULL) {
        return;
    }

    /* Extract request ID from topic: v1/devices/me/rpc/request/{id} */
    const char *id_str = topic + strlen(TB_RPC_SERVER_REQ_TOPIC);
    uint32_t request_id = (uint32_t)strtoul(id_str, NULL, 10);

    /* Parse JSON to extract method and params */
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (root == NULL) {
        osal_log_warning("[tb_rpc] Failed to parse RPC request JSON");
        return;
    }

    cJSON *method_item = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON *params_item = cJSON_GetObjectItemCaseSensitive(root, "params");

    const char *method = (method_item != NULL && cJSON_IsString(method_item))
                             ? method_item->valuestring
                             : "";

    char *params_json = NULL;
    if (params_item != NULL) {
        params_json = cJSON_PrintUnformatted(params_item);
    }

    s_server_rpc_cb(method, params_json ? params_json : "{}",
                    request_id, s_server_rpc_user_data);

    if (params_json != NULL) {
        cJSON_free(params_json);
    }
    cJSON_Delete(root);
}

static void client_rpc_response_handler(const char *topic, const char *payload,
                                        size_t payload_len)
{
    /* Extract request ID from topic: v1/devices/me/rpc/response/{id} */
    const char *id_str = topic + strlen(TB_RPC_CLIENT_RESP_TOPIC);
    uint32_t req_id = (uint32_t)strtoul(id_str, NULL, 10);
    tb_client_rpc_cb_t cb = NULL;
    void *ud = NULL;
    bool timed_out = false;
    uint32_t now_ms = osal_task_get_time_ms();

    (void)payload_len;

    osal_mutex_take(s_rpc_mutex);
    for (int i = 0; i < TB_RPC_MAX_PENDING; i++) {
        if (s_rpc_pending[i].active && s_rpc_pending[i].request_id == req_id) {
            cb = s_rpc_pending[i].cb;
            ud = s_rpc_pending[i].user_data;
            timed_out = time_reached(now_ms, s_rpc_pending[i].deadline_ms);
            s_rpc_pending[i].active = false;
            osal_mutex_give(s_rpc_mutex);
            break;
        }
    }

    if (cb == NULL) {
        osal_mutex_give(s_rpc_mutex);
        return;
    }

    if (timed_out) {
        cb(TB_REQUEST_RESULT_TIMEOUT, NULL, ud);
        return;
    }

    char *payload_copy = malloc(payload_len + 1);
    if (payload_copy == NULL) {
        cb(TB_REQUEST_RESULT_ERROR, NULL, ud);
        return;
    }

    memcpy(payload_copy, payload, payload_len);
    payload_copy[payload_len] = '\0';
    cb(TB_REQUEST_RESULT_SUCCESS, payload_copy, ud);
    free(payload_copy);
}

int tb_rpc_subscribe_server(tb_client_t *client, tb_server_rpc_cb_t cb,
                            void *user_data)
{
    if (client == NULL || cb == NULL) {
        return -1;
    }

    if (s_owner_client == NULL) {
        s_owner_client = client;
    } else if (s_owner_client != client) {
        return -1;
    }

    s_server_rpc_cb = cb;
    s_server_rpc_user_data = user_data;

    if (!s_server_rpc_subscribed) {
        int ret = tb_client_subscribe(client, TB_RPC_SERVER_SUB_TOPIC,
                                      server_rpc_handler, TB_RPC_TIMEOUT_MS);
        if (ret == 0) {
            s_server_rpc_subscribed = true;
        }
        return ret;
    }
    return 0;
}

int tb_rpc_unsubscribe_server(tb_client_t *client)
{
    if (client == NULL) {
        return -1;
    }
    if (s_owner_client != NULL && s_owner_client != client) {
        return -1;
    }

    s_server_rpc_cb = NULL;
    s_server_rpc_user_data = NULL;

    if (s_server_rpc_subscribed) {
        s_server_rpc_subscribed = false;
        return tb_client_unsubscribe(client, TB_RPC_SERVER_SUB_TOPIC,
                                     TB_RPC_TIMEOUT_MS);
    }
    return 0;
}

int tb_rpc_respond(tb_client_t *client, uint32_t request_id,
                   const char *response_json)
{
    if (client == NULL) {
        return -1;
    }

    const char *payload = response_json ? response_json : "{}";
    char topic[TB_RPC_MAX_TOPIC_LEN];
    snprintf(topic, sizeof(topic), TB_RPC_SERVER_RESP_TOPIC, request_id);

    return tb_client_publish(client, topic, payload);
}

int tb_rpc_request(tb_client_t *client, const char *method,
                   const char *params_json, tb_client_rpc_cb_t cb,
                   void *user_data, uint32_t timeout_ms)
{
    if (client == NULL || method == NULL) {
        return -1;
    }

    if (s_owner_client == NULL) {
        s_owner_client = client;
    } else if (s_owner_client != client) {
        return -1;
    }

    if (!ensure_rpc_init()) {
        return -1;
    }

    /* Subscribe to response topic if not already */
    if (!s_client_rpc_subscribed) {
        uint32_t subscribe_timeout = timeout_ms > 0 ? timeout_ms : TB_RPC_TIMEOUT_MS;
        int ret = tb_client_subscribe(client, TB_RPC_CLIENT_RESP_SUB,
                                      client_rpc_response_handler,
                                      subscribe_timeout);
        if (ret != 0) {
            return ret;
        }
        s_client_rpc_subscribed = true;
    }

    uint32_t req_id = tb_client_get_next_request_id(client);
    uint32_t effective_timeout = timeout_ms > 0 ? timeout_ms : TB_RPC_TIMEOUT_MS;

    /* Register pending request */
    /* TODO(osal): use timed mutex lock once OSAL exposes mutex take with timeout.
     * This path should respect timeout_ms instead of blocking indefinitely. */
    osal_mutex_take(s_rpc_mutex);
    int slot = -1;
    for (int i = 0; i < TB_RPC_MAX_PENDING; i++) {
        if (!s_rpc_pending[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        osal_mutex_give(s_rpc_mutex);
        osal_log_warning("[tb_rpc] No free RPC pending slots");
        return -1;
    }
    s_rpc_pending[slot].request_id = req_id;
    s_rpc_pending[slot].deadline_ms = osal_task_get_time_ms() + effective_timeout;
    s_rpc_pending[slot].cb = cb;
    s_rpc_pending[slot].user_data = user_data;
    s_rpc_pending[slot].active = true;
    osal_mutex_give(s_rpc_mutex);

    /* Build request JSON: {"method":"name","params":{...}} */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        osal_mutex_take(s_rpc_mutex);
        s_rpc_pending[slot].active = false;
        tb_client_rpc_cb_t slot_cb = s_rpc_pending[slot].cb;
        void *slot_ud = s_rpc_pending[slot].user_data;
        osal_mutex_give(s_rpc_mutex);
        if (slot_cb != NULL) {
            slot_cb(TB_REQUEST_RESULT_ERROR, NULL, slot_ud);
        }
        return -1;
    }

    cJSON_AddStringToObject(root, "method", method);

    if (params_json != NULL && params_json[0] != '\0') {
        cJSON *params = cJSON_Parse(params_json);
        if (params != NULL) {
            cJSON_AddItemToObject(root, "params", params);
        } else {
            cJSON_AddRawToObject(root, "params", "{}");
        }
    } else {
        cJSON_AddRawToObject(root, "params", "{}");
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        osal_mutex_take(s_rpc_mutex);
        s_rpc_pending[slot].active = false;
        tb_client_rpc_cb_t slot_cb = s_rpc_pending[slot].cb;
        void *slot_ud = s_rpc_pending[slot].user_data;
        osal_mutex_give(s_rpc_mutex);
        if (slot_cb != NULL) {
            slot_cb(TB_REQUEST_RESULT_ERROR, NULL, slot_ud);
        }
        return -1;
    }

    char topic[TB_RPC_MAX_TOPIC_LEN];
    snprintf(topic, sizeof(topic), TB_RPC_CLIENT_REQ_TOPIC, req_id);

    int ret = tb_client_publish(client, topic, json);
    cJSON_free(json);

    if (ret != 0) {
        osal_mutex_take(s_rpc_mutex);
        s_rpc_pending[slot].active = false;
        tb_client_rpc_cb_t slot_cb = s_rpc_pending[slot].cb;
        void *slot_ud = s_rpc_pending[slot].user_data;
        osal_mutex_give(s_rpc_mutex);
        if (slot_cb != NULL) {
            slot_cb(TB_REQUEST_RESULT_ERROR, NULL, slot_ud);
        }
    }

    return ret;
}

void tb_rpc_handle_disconnect(tb_client_t *client)
{
    if (client == NULL || s_owner_client == NULL || s_owner_client != client ||
        !s_rpc_init) {
        return;
    }

    complete_pending_requests(TB_REQUEST_RESULT_CANCELLED);
}

void tb_rpc_deinit(tb_client_t *client)
{
    if (client == NULL || s_owner_client == NULL || s_owner_client != client) {
        return;
    }

    tb_rpc_handle_disconnect(client);
    s_server_rpc_cb = NULL;
    s_server_rpc_user_data = NULL;
    s_server_rpc_subscribed = false;
    s_client_rpc_subscribed = false;
    s_owner_client = NULL;
    teardown_rpc_resources();
    s_rpc_init_failed = false;
}
