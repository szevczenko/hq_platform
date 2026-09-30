/**
 *******************************************************************************
 * @file    tb_state_sync.c
 * @brief   Generic ThingsBoard desired-state synchronization engine
 *******************************************************************************
 */

#include "tb_state_sync.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "tb_attributes.h"
#include "tb_rpc.h"

typedef enum tb_sync_state {
    TB_SYNC_INACTIVE = 0,
    TB_SYNCING,
    TB_SYNC_BACKOFF,
    TB_SYNCED
} tb_sync_state_t;

typedef struct tb_sync_attempt_token {
    uint32_t session;
    uint32_t attempt;
} tb_sync_attempt_token_t;

/* Product state structs are cast from these bytes: keep max alignment
 * (C99-portable stand-in for max_align_t). */
typedef union tb_sync_state_buffer {
    long double align_ld;
    uint64_t align_u64;
    void *align_ptr;
    void (*align_fn)(void);
    uint8_t bytes[TB_STATE_SYNC_MAX_STATE_BYTES];
} tb_sync_state_buffer_t;

typedef struct tb_sync_rpc_handler {
    char method[TB_STATE_SYNC_MAX_RPC_METHOD_LEN + 1u];
    tb_state_sync_rpc_fn_t callback;
    void *user_data;
} tb_sync_rpc_handler_t;

typedef struct tb_sync_module {
    bool initialized;
    bool connected;
    bool output_suspended;
    bool has_state;
    bool attempt_resolved;
    tb_client_t *client;
    tb_state_sync_config_t config;
    osal_mutex_id_t lock;
    tb_sync_state_t state;
    tb_sync_state_buffer_t applied_state;
    uint32_t session;
    uint32_t attempt;
    uint32_t retries;
    uint32_t deadline_ms;
    uint32_t backoff_until_ms;
    uint32_t backoff_delay_ms;
    uint32_t last_telemetry_ms;
    tb_sync_attempt_token_t request_token;
    tb_sync_rpc_handler_t handlers[TB_STATE_SYNC_MAX_RPC_HANDLERS];
    size_t handler_count;
} tb_sync_module_t;

static tb_sync_module_t s_sync;

static uint32_t tb_sync_now(void)
{
    return s_sync.config.now_ms != NULL ? s_sync.config.now_ms()
                                        : osal_task_get_time_ms();
}

static uint32_t tb_sync_clamp(uint32_t value, uint32_t minimum,
                              uint32_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static bool tb_sync_transport_connected(void)
{
    return s_sync.connected && s_sync.client != NULL &&
           tb_client_is_connected(s_sync.client);
}

static size_t tb_sync_bounded_length(const char *text, size_t maximum)
{
    size_t length = 0u;
    if (text == NULL) {
        return 0u;
    }
    while (length <= maximum && text[length] != '\0') {
        length++;
    }
    return length;
}

static void tb_sync_publish_telemetry_locked(void)
{
    if (s_sync.initialized && tb_sync_transport_connected() &&
        s_sync.config.publish_telemetry != NULL) {
        s_sync.config.publish_telemetry(s_sync.config.user_data);
        s_sync.last_telemetry_ms = tb_sync_now();
    }
}

static void tb_sync_publish_attributes_locked(void)
{
    if (s_sync.initialized && s_sync.has_state &&
        tb_sync_transport_connected() &&
        s_sync.config.publish_applied_attributes != NULL) {
        s_sync.config.publish_applied_attributes(s_sync.applied_state.bytes,
                                                  s_sync.config.user_data);
    }
}

static void tb_sync_start_attempt_locked(void)
{
    s_sync.attempt++;
    s_sync.attempt_resolved = false;
    s_sync.request_token.session = s_sync.session;
    s_sync.request_token.attempt = s_sync.attempt;
    s_sync.deadline_ms = tb_sync_now() + s_sync.config.sync_timeout_ms;
    s_sync.state = TB_SYNCING;
}

static void tb_sync_fail_attempt_locked(void)
{
    if (s_sync.config.force_inactive != NULL) {
        s_sync.config.force_inactive(s_sync.config.user_data);
    }
    s_sync.attempt_resolved = true;
    if (!tb_sync_transport_connected()) {
        s_sync.state = TB_SYNC_INACTIVE;
        return;
    }

    s_sync.retries++;
    if (s_sync.retries >= s_sync.config.max_retries) {
        s_sync.state = TB_SYNC_INACTIVE;
        osal_log_warning("[tb_state_sync] retry budget exhausted (%u)",
                         (unsigned)s_sync.retries);
        return;
    }

    if (s_sync.backoff_delay_ms == 0u) {
        s_sync.backoff_delay_ms = s_sync.config.retry_initial_delay_ms;
    } else {
        uint64_t doubled = (uint64_t)s_sync.backoff_delay_ms * 2u;
        if (doubled > s_sync.config.retry_max_delay_ms) {
            doubled = s_sync.config.retry_max_delay_ms;
        }
        s_sync.backoff_delay_ms = (uint32_t)doubled;
    }
    s_sync.backoff_until_ms = tb_sync_now() + s_sync.backoff_delay_ms;
    s_sync.state = TB_SYNC_BACKOFF;
}

static void tb_sync_on_shared_update(const char *json, void *user_data);
static void tb_sync_on_attribute_response(tb_request_result_t result,
                                         const char *json, void *user_data);

static int tb_sync_subscribe_and_request(void)
{
    const tb_state_sync_config_t *config = &s_sync.config;
    int result = tb_attributes_subscribe(s_sync.client,
                                     tb_sync_on_shared_update,
                                     (void *)(uintptr_t)s_sync.session);
    if (result != 0) {
        return result;
    }
    return tb_attributes_request(s_sync.client,
                                 config->client_keys,
                                 config->client_key_count,
                                 config->shared_keys,
                                 config->shared_key_count,
                                 tb_sync_on_attribute_response,
                                 &s_sync.request_token,
                                 config->sync_timeout_ms);
}

static void tb_sync_run_transport(void)
{
    int result = tb_sync_subscribe_and_request();
    if (result != 0 && s_sync.lock != NULL) {
        osal_mutex_take(s_sync.lock);
        if (s_sync.initialized && s_sync.state == TB_SYNCING &&
            !s_sync.attempt_resolved) {
            tb_sync_fail_attempt_locked();
        }
        osal_mutex_give(s_sync.lock);
    }
}

static tb_state_sync_parse_result_t tb_sync_parse_response(
    const char *json, uint8_t *state)
{
    static const tb_state_sync_source_t order[] = {
        TB_STATE_SYNC_SOURCE_CLIENT,
        TB_STATE_SYNC_SOURCE_SHARED,
        TB_STATE_SYNC_SOURCE_DEFAULT
    };
    for (size_t index = 0u; index < sizeof(order) / sizeof(order[0]); index++) {
        tb_state_sync_parse_result_t result = s_sync.config.parse_state(
            json, order[index], state, s_sync.config.user_data);
        if (result == TB_STATE_SYNC_PARSE_VALID) {
            return result;
        }
    }
    return TB_STATE_SYNC_PARSE_INVALID;
}

static bool tb_sync_states_equal(const void *left, const void *right)
{
    if (s_sync.config.states_equal != NULL) {
        return s_sync.config.states_equal(left, right,
                                         s_sync.config.user_data);
    }
    return memcmp(left, right, s_sync.config.state_size) == 0;
}

static bool tb_sync_apply_locked(const uint8_t *state)
{
    memcpy(s_sync.applied_state.bytes, state, s_sync.config.state_size);
    s_sync.has_state = true;
    s_sync.retries = 0u;
    s_sync.state = TB_SYNCED;
    return true;
}

static void tb_sync_publish_applied(const uint8_t *state, bool output_suspended,
                                    tb_state_sync_state_fn_t publish_attributes,
                                    tb_state_sync_void_fn_t publish_telemetry,
                                    void *user_data)
{
    if (publish_attributes != NULL) {
        publish_attributes(state, user_data);
    }
    if (!output_suspended && publish_telemetry != NULL) {
        publish_telemetry(user_data);
    }
}

static void tb_sync_on_shared_update(const char *json, void *user_data)
{
    tb_sync_state_buffer_t state;
    const uint32_t callback_session = (uint32_t)(uintptr_t)user_data;

    if (s_sync.lock == NULL) {
        return;
    }
    osal_mutex_take(s_sync.lock);
    if (!s_sync.initialized || !tb_sync_transport_connected() ||
        s_sync.state == TB_SYNC_INACTIVE || callback_session != s_sync.session) {
        osal_mutex_give(s_sync.lock);
        return;
    }
    if (json == NULL ||
        tb_sync_bounded_length(json, s_sync.config.max_payload_bytes) >
            s_sync.config.max_payload_bytes) {
        tb_sync_fail_attempt_locked();
        osal_mutex_give(s_sync.lock);
        return;
    }

    tb_state_sync_parse_result_t parsed = s_sync.config.parse_state(
        json, TB_STATE_SYNC_SOURCE_UPDATE, state.bytes,
        s_sync.config.user_data);
    if (parsed == TB_STATE_SYNC_PARSE_IGNORED) {
        osal_mutex_give(s_sync.lock);
        return;
    }
    if (parsed != TB_STATE_SYNC_PARSE_VALID) {
        tb_sync_fail_attempt_locked();
        osal_mutex_give(s_sync.lock);
        return;
    }
    if (s_sync.state == TB_SYNCED && s_sync.has_state &&
        tb_sync_states_equal(state.bytes, s_sync.applied_state.bytes)) {
        osal_mutex_give(s_sync.lock);
        return;
    }
    if (s_sync.state == TB_SYNCING) {
        s_sync.attempt_resolved = true;
    }
    tb_state_sync_apply_fn_t apply_state = s_sync.config.apply_state;
    void *callback_data = s_sync.config.user_data;
    const bool output_suspended = s_sync.output_suspended;
    const uint32_t session = s_sync.session;
    osal_mutex_give(s_sync.lock);
    if (!apply_state(state.bytes, output_suspended, callback_data)) {
        if (osal_mutex_take(s_sync.lock) == OSAL_SUCCESS) {
            if (s_sync.initialized && s_sync.session == session) {
                tb_sync_fail_attempt_locked();
            }
            osal_mutex_give(s_sync.lock);
        }
        return;
    }
    if (osal_mutex_take(s_sync.lock) != OSAL_SUCCESS) {
        return;
    }
    bool notify = false;
    tb_state_sync_state_fn_t publish_attributes = NULL;
    tb_state_sync_void_fn_t publish_telemetry = NULL;
    uint8_t applied_state[TB_STATE_SYNC_MAX_STATE_BYTES];
    if (!s_sync.initialized || s_sync.session != session ||
        !tb_sync_apply_locked(state.bytes)) {
        tb_sync_fail_attempt_locked();
    }
    if (s_sync.initialized && s_sync.session == session &&
        s_sync.state == TB_SYNCED) {
        memcpy(applied_state, s_sync.applied_state.bytes,
               s_sync.config.state_size);
        publish_attributes = s_sync.config.publish_applied_attributes;
        publish_telemetry = s_sync.config.publish_telemetry;
        notify = true;
    }
    osal_mutex_give(s_sync.lock);
    if (notify) {
        tb_sync_publish_applied(applied_state, output_suspended,
                                publish_attributes, publish_telemetry,
                                callback_data);
    }
}

static void tb_sync_on_attribute_response(tb_request_result_t result,
                                          const char *json, void *user_data)
{
    tb_sync_state_buffer_t state;
    const tb_sync_attempt_token_t *token = user_data;

    if (s_sync.lock == NULL) {
        return;
    }
    osal_mutex_take(s_sync.lock);
    if (!s_sync.initialized || s_sync.state != TB_SYNCING || token == NULL ||
        token->session != s_sync.session || token->attempt != s_sync.attempt ||
        s_sync.attempt_resolved) {
        osal_mutex_give(s_sync.lock);
        return;
    }
    if (result == TB_REQUEST_RESULT_CANCELLED) {
        osal_mutex_give(s_sync.lock);
        return;
    }
    if (result != TB_REQUEST_RESULT_SUCCESS || json == NULL ||
        tb_sync_bounded_length(json, s_sync.config.max_payload_bytes) >
            s_sync.config.max_payload_bytes ||
        tb_sync_parse_response(json, state.bytes) !=
            TB_STATE_SYNC_PARSE_VALID) {
        tb_sync_fail_attempt_locked();
        osal_mutex_give(s_sync.lock);
        return;
    }
    s_sync.attempt_resolved = true;
    tb_state_sync_apply_fn_t apply_state = s_sync.config.apply_state;
    void *callback_data = s_sync.config.user_data;
    const bool output_suspended = s_sync.output_suspended;
    const uint32_t session = s_sync.session;
    osal_mutex_give(s_sync.lock);
    if (!apply_state(state.bytes, output_suspended, callback_data)) {
        if (osal_mutex_take(s_sync.lock) == OSAL_SUCCESS) {
            if (s_sync.initialized && s_sync.session == session) {
                tb_sync_fail_attempt_locked();
            }
            osal_mutex_give(s_sync.lock);
        }
        return;
    }
    if (osal_mutex_take(s_sync.lock) != OSAL_SUCCESS) {
        return;
    }
    bool notify = false;
    tb_state_sync_state_fn_t publish_attributes = NULL;
    tb_state_sync_void_fn_t publish_telemetry = NULL;
    uint8_t applied_state[TB_STATE_SYNC_MAX_STATE_BYTES];
    if (!s_sync.initialized || s_sync.session != session ||
        !tb_sync_apply_locked(state.bytes)) {
        tb_sync_fail_attempt_locked();
    } else {
        memcpy(applied_state, s_sync.applied_state.bytes,
               s_sync.config.state_size);
        publish_attributes = s_sync.config.publish_applied_attributes;
        publish_telemetry = s_sync.config.publish_telemetry;
        notify = true;
    }
    osal_mutex_give(s_sync.lock);
    if (notify) {
        tb_sync_publish_applied(applied_state, output_suspended,
                                publish_attributes, publish_telemetry,
                                callback_data);
    }
}

int tb_state_sync_respond_rpc_error(tb_client_t *client, uint32_t request_id,
                                    const char *error, const char *reason,
                                    const char *method)
{
    cJSON *root = cJSON_CreateObject();
    char *json;
    int result;
    if (root == NULL) {
        return -1;
    }
    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "error", error != NULL ? error : "invalid payload");
    if (reason != NULL) {
        cJSON_AddStringToObject(root, "reason", reason);
    }
    if (method != NULL) {
        cJSON_AddStringToObject(root, "method", method);
    }
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return -1;
    }
    result = tb_rpc_respond(client, request_id, json);
    cJSON_free(json);
    return result;
}

static void tb_sync_on_server_rpc(const char *method, const char *params_json,
                                  uint32_t request_id, void *user_data)
{
    tb_state_sync_rpc_fn_t callback = NULL;
    void *callback_data = NULL;
    const char *name = method != NULL ? method : "";
    size_t method_length = tb_sync_bounded_length(
        name, TB_STATE_SYNC_MAX_RPC_METHOD_LEN);
    size_t payload_length = tb_sync_bounded_length(
        params_json, TB_STATE_SYNC_MAX_RPC_PAYLOAD_BYTES);
    tb_client_t *client;

    (void)user_data;
    if (s_sync.lock == NULL) {
        return;
    }
    osal_mutex_take(s_sync.lock);
    if (!s_sync.initialized || !tb_sync_transport_connected()) {
        osal_mutex_give(s_sync.lock);
        return;
    }
    client = s_sync.client;
    if (method_length == 0u ||
        method_length > TB_STATE_SYNC_MAX_RPC_METHOD_LEN) {
        osal_mutex_give(s_sync.lock);
        (void)tb_state_sync_respond_rpc_error(client, request_id,
                                               "invalid payload",
                                               method_length == 0u
                                                   ? "missing method"
                                                   : "method too long",
                                               NULL);
        return;
    }
    if (params_json != NULL &&
        payload_length > TB_STATE_SYNC_MAX_RPC_PAYLOAD_BYTES) {
        osal_mutex_give(s_sync.lock);
        (void)tb_state_sync_respond_rpc_error(client, request_id,
                                               "invalid payload",
                                               "payload too long", NULL);
        return;
    }
    for (size_t index = 0u; index < s_sync.handler_count; index++) {
        if (strcmp(s_sync.handlers[index].method, name) == 0) {
            callback = s_sync.handlers[index].callback;
            callback_data = s_sync.handlers[index].user_data;
            break;
        }
    }
    osal_mutex_give(s_sync.lock);

    if (callback == NULL) {
        (void)tb_state_sync_respond_rpc_error(client, request_id,
                                               "unknown method", NULL, name);
        return;
    }
    callback(name, params_json != NULL ? params_json : "{}", request_id,
             callback_data);
}

tb_state_sync_status_t tb_state_sync_init(const tb_state_sync_config_t *config)
{
    osal_status_t os_result;
    if (config == NULL || config->client == NULL ||
        config->parse_state == NULL || config->apply_state == NULL ||
        config->state_size == 0u ||
        config->state_size > TB_STATE_SYNC_MAX_STATE_BYTES ||
        config->max_payload_bytes == 0u ||
        (config->client_key_count != 0u && config->client_keys == NULL) ||
        (config->shared_key_count != 0u && config->shared_keys == NULL)) {
        return TB_STATE_SYNC_ERR_INVALID_ARGUMENT;
    }
    if (s_sync.lock == NULL) {
        os_result = osal_mutex_create(&s_sync.lock, "tb_state_sync");
        if (os_result != OSAL_SUCCESS) {
            return TB_STATE_SYNC_ERR_NO_RESOURCE;
        }
    }
    osal_mutex_take(s_sync.lock);
    if (s_sync.initialized) {
        osal_mutex_give(s_sync.lock);
        return TB_STATE_SYNC_ERR_ALREADY_INITIALIZED;
    }
    memset(&s_sync.config, 0, sizeof(s_sync.config));
    s_sync.config = *config;
    s_sync.config.sync_timeout_ms = tb_sync_clamp(
        config->sync_timeout_ms == 0u ? TB_STATE_SYNC_TIMEOUT_DEFAULT_MS
                                      : config->sync_timeout_ms,
        TB_STATE_SYNC_TIMEOUT_MIN_MS, TB_STATE_SYNC_TIMEOUT_MAX_MS);
    s_sync.config.retry_initial_delay_ms = tb_sync_clamp(
        config->retry_initial_delay_ms == 0u
            ? TB_STATE_SYNC_RETRY_INITIAL_DEFAULT_MS
            : config->retry_initial_delay_ms,
        TB_STATE_SYNC_RETRY_DELAY_MIN_MS, TB_STATE_SYNC_RETRY_DELAY_MAX_MS);
    s_sync.config.retry_max_delay_ms = tb_sync_clamp(
        config->retry_max_delay_ms == 0u ? TB_STATE_SYNC_RETRY_MAX_DEFAULT_MS
                                         : config->retry_max_delay_ms,
        TB_STATE_SYNC_RETRY_DELAY_MIN_MS, TB_STATE_SYNC_RETRY_DELAY_MAX_MS);
    if (s_sync.config.retry_max_delay_ms <
        s_sync.config.retry_initial_delay_ms) {
        s_sync.config.retry_max_delay_ms =
            s_sync.config.retry_initial_delay_ms;
    }
    s_sync.config.max_retries = config->max_retries == 0u
                                    ? TB_STATE_SYNC_MAX_RETRIES_DEFAULT
                                    : config->max_retries;
    s_sync.config.telemetry_period_ms = tb_sync_clamp(
        config->telemetry_period_ms == 0u
            ? TB_STATE_SYNC_TELEMETRY_PERIOD_DEFAULT_MS
            : config->telemetry_period_ms,
        TB_STATE_SYNC_TELEMETRY_PERIOD_MIN_MS,
        TB_STATE_SYNC_TELEMETRY_PERIOD_MAX_MS);
    s_sync.client = config->client;
    s_sync.initialized = true;
    s_sync.connected = false;
    s_sync.output_suspended = false;
    s_sync.has_state = false;
    s_sync.state = TB_SYNC_INACTIVE;
    s_sync.session = 0u;
    s_sync.attempt = 0u;
    s_sync.retries = 0u;
    s_sync.backoff_delay_ms = 0u;
    s_sync.last_telemetry_ms = 0u;
    s_sync.attempt_resolved = true;
    memset(&s_sync.applied_state, 0, sizeof(s_sync.applied_state));
    memset(&s_sync.request_token, 0, sizeof(s_sync.request_token));
    osal_mutex_give(s_sync.lock);
    return TB_STATE_SYNC_OK;
}

void tb_state_sync_deinit(void)
{
    tb_client_t *client;

    if (s_sync.lock == NULL) {
        return;
    }
    osal_mutex_take(s_sync.lock);
    client = s_sync.initialized ? s_sync.client : NULL;
    s_sync.initialized = false;
    s_sync.connected = false;
    s_sync.state = TB_SYNC_INACTIVE;
    s_sync.attempt_resolved = true;
    s_sync.handler_count = 0u;
    memset(s_sync.handlers, 0, sizeof(s_sync.handlers));
    osal_mutex_give(s_sync.lock);
    if (client != NULL) {
        (void)tb_attributes_unsubscribe(client);
        (void)tb_rpc_unsubscribe_server(client);
    }
}

int tb_state_sync_register_rpc_handler(const char *method,
                                       tb_state_sync_rpc_fn_t handler,
                                       void *user_data)
{
    size_t length = tb_sync_bounded_length(
        method, TB_STATE_SYNC_MAX_RPC_METHOD_LEN);
    if (method == NULL || handler == NULL || length == 0u ||
        length > TB_STATE_SYNC_MAX_RPC_METHOD_LEN || s_sync.lock == NULL) {
        return -1;
    }
    osal_mutex_take(s_sync.lock);
    if (s_sync.handler_count >= TB_STATE_SYNC_MAX_RPC_HANDLERS) {
        osal_mutex_give(s_sync.lock);
        return -1;
    }
    for (size_t index = 0u; index < s_sync.handler_count; index++) {
        if (strcmp(s_sync.handlers[index].method, method) == 0) {
            osal_mutex_give(s_sync.lock);
            return -1;
        }
    }
    memcpy(s_sync.handlers[s_sync.handler_count].method, method, length + 1u);
    s_sync.handlers[s_sync.handler_count].callback = handler;
    s_sync.handlers[s_sync.handler_count].user_data = user_data;
    s_sync.handler_count++;
    osal_mutex_give(s_sync.lock);
    return 0;
}

void tb_state_sync_on_connected(tb_client_t *client)
{
    bool begin = false;
    if (client == NULL || s_sync.lock == NULL) {
        return;
    }
    osal_mutex_take(s_sync.lock);
    if (s_sync.initialized && client == s_sync.client) {
        s_sync.session++;
        s_sync.retries = 0u;
        s_sync.backoff_delay_ms = 0u;
        s_sync.connected = true;
        tb_sync_start_attempt_locked();
        tb_sync_publish_telemetry_locked();
        begin = true;
    }
    osal_mutex_give(s_sync.lock);
    if (begin) {
        tb_sync_run_transport();
        if (tb_rpc_subscribe_server(client, tb_sync_on_server_rpc, NULL) != 0) {
            osal_log_warning("[tb_state_sync] RPC subscribe failed");
        }
    }
}

void tb_state_sync_on_disconnected(tb_client_t *client)
{
    if (client == NULL || s_sync.lock == NULL) {
        return;
    }
    osal_mutex_take(s_sync.lock);
    if (s_sync.initialized && client == s_sync.client) {
        s_sync.session++;
        s_sync.state = TB_SYNC_INACTIVE;
        s_sync.attempt_resolved = true;
        s_sync.connected = false;
        if (s_sync.config.force_inactive != NULL) {
            s_sync.config.force_inactive(s_sync.config.user_data);
        }
    }
    osal_mutex_give(s_sync.lock);
}

void tb_state_sync_poll(tb_client_t *client)
{
    bool run_attempt = false;
    if (client == NULL || s_sync.lock == NULL) {
        return;
    }
    osal_mutex_take(s_sync.lock);
    if (!s_sync.initialized || client != s_sync.client ||
        s_sync.state == TB_SYNC_INACTIVE || !tb_sync_transport_connected()) {
        osal_mutex_give(s_sync.lock);
        return;
    }
    uint32_t now = tb_sync_now();
    if (s_sync.state == TB_SYNCING && !s_sync.attempt_resolved &&
        now >= s_sync.deadline_ms) {
        tb_sync_fail_attempt_locked();
    }
    if (s_sync.state == TB_SYNC_BACKOFF && now >= s_sync.backoff_until_ms) {
        tb_sync_start_attempt_locked();
        run_attempt = true;
    }
    if ((uint32_t)(now - s_sync.last_telemetry_ms) >=
        s_sync.config.telemetry_period_ms) {
        tb_sync_publish_telemetry_locked();
    }
    osal_mutex_give(s_sync.lock);
    if (run_attempt) {
        tb_sync_run_transport();
    }
}

bool tb_state_sync_is_synchronized(tb_client_t *client)
{
    bool result = false;
    if (s_sync.lock == NULL) {
        return false;
    }
    osal_mutex_take(s_sync.lock);
    result = s_sync.initialized && s_sync.state == TB_SYNCED &&
             (client == NULL || client == s_sync.client) &&
             tb_sync_transport_connected();
    osal_mutex_give(s_sync.lock);
    return result;
}

bool tb_state_sync_is_connected(void)
{
    bool result = false;
    if (s_sync.lock == NULL) {
        return false;
    }
    osal_mutex_take(s_sync.lock);
    result = s_sync.initialized && tb_sync_transport_connected();
    osal_mutex_give(s_sync.lock);
    return result;
}

bool tb_state_sync_is_output_suspended(void)
{
    bool result = false;
    if (s_sync.lock == NULL) {
        return false;
    }
    osal_mutex_take(s_sync.lock);
    result = s_sync.output_suspended;
    osal_mutex_give(s_sync.lock);
    return result;
}

tb_state_sync_status_t tb_state_sync_get_state(tb_client_t *client,
                                                bool *has_state,
                                                void *state_out)
{
    tb_state_sync_status_t result = TB_STATE_SYNC_OK;
    if (has_state == NULL && state_out == NULL) {
        return TB_STATE_SYNC_ERR_INVALID_ARGUMENT;
    }
    if (s_sync.lock == NULL) {
        return TB_STATE_SYNC_ERR_NOT_INITIALIZED;
    }
    osal_mutex_take(s_sync.lock);
    if (!s_sync.initialized) {
        result = TB_STATE_SYNC_ERR_NOT_INITIALIZED;
    } else if (client != NULL && client != s_sync.client) {
        result = TB_STATE_SYNC_ERR_CLIENT_MISMATCH;
    } else {
        if (has_state != NULL) {
            *has_state = s_sync.has_state;
        }
        if (state_out != NULL) {
            memcpy(state_out, s_sync.applied_state.bytes,
                   s_sync.config.state_size);
        }
    }
    osal_mutex_give(s_sync.lock);
    return result;
}

tb_state_sync_status_t tb_state_sync_record_state(const void *state)
{
    if (state == NULL) {
        return TB_STATE_SYNC_ERR_INVALID_ARGUMENT;
    }
    if (s_sync.lock == NULL) {
        return TB_STATE_SYNC_ERR_NOT_INITIALIZED;
    }
    osal_mutex_take(s_sync.lock);
    if (!s_sync.initialized) {
        osal_mutex_give(s_sync.lock);
        return TB_STATE_SYNC_ERR_NOT_INITIALIZED;
    }
    memcpy(s_sync.applied_state.bytes, state, s_sync.config.state_size);
    s_sync.has_state = true;
    osal_mutex_give(s_sync.lock);
    return TB_STATE_SYNC_OK;
}

void tb_state_sync_publish_applied_state(void)
{
    if (s_sync.lock == NULL) {
        return;
    }
    osal_mutex_take(s_sync.lock);
    tb_sync_publish_attributes_locked();
    osal_mutex_give(s_sync.lock);
}

void tb_state_sync_note_state_change(void)
{
    if (s_sync.lock == NULL) {
        return;
    }
    osal_mutex_take(s_sync.lock);
    tb_sync_publish_telemetry_locked();
    osal_mutex_give(s_sync.lock);
}

void tb_state_sync_set_output_suspended(bool suspended)
{
    if (s_sync.lock == NULL) {
        return;
    }
    osal_mutex_take(s_sync.lock);
    if (!s_sync.initialized || s_sync.output_suspended == suspended) {
        osal_mutex_give(s_sync.lock);
        return;
    }
    s_sync.output_suspended = suspended;
    if (!suspended && s_sync.state == TB_SYNCED && s_sync.has_state &&
        tb_sync_transport_connected()) {
        if (s_sync.config.apply_state(s_sync.applied_state.bytes, false,
                                      s_sync.config.user_data)) {
            tb_sync_publish_attributes_locked();
            tb_sync_publish_telemetry_locked();
        } else {
            tb_sync_fail_attempt_locked();
        }
    } else if (!suspended && s_sync.config.force_inactive != NULL) {
        s_sync.config.force_inactive(s_sync.config.user_data);
    }
    osal_mutex_give(s_sync.lock);
}