/**
 *******************************************************************************
 * @file    mqtt_app_mock.c
 * @brief   Mock implementation of mqtt_app for ThingsBoard unit tests
 *******************************************************************************
 */

#include "mqtt_app.h"
#include "mqtt_app_mock.h"
#include "mqtt_config.h"

#include <string.h>
#include <stdio.h>

/* Mock state */
mock_publish_record_t mock_publishes[MOCK_MAX_PUBLISHES];
int mock_publish_count = 0;
mock_subscribe_record_t mock_subscribes[MOCK_MAX_PUBLISHES];
int mock_subscribe_count = 0;
bool mock_connected = false;
int mock_deinit_count = 0;
mqtt_connection_policy_t mock_connection_policy = {
    .keepalive_sec = 60,
    .reconnect_initial_delay_ms = 30000,
    .reconnect_max_delay_ms = 300000,
    .reconnect_exponential_backoff = false,
};
int mock_suback_count = 0;
int mock_puback_count = 0;
int mock_subscription_replay_count = 0;

static mqtt_connect_callback_t s_connect_cb = NULL;
static mqtt_disconnect_callback_t s_disconnect_cb = NULL;
static mqtt_connect_failure_callback_t s_connect_failure_cb = NULL;

#define MOCK_MAX_SUBS 16
#define MOCK_MAX_EVENTS 32
static struct {
    char topic[MOCK_MAX_TOPIC_LEN];
    mqtt_message_callback_t callback;
    bool active;
} s_subscriptions[MOCK_MAX_SUBS];

typedef enum {
    MOCK_EVENT_NONE = 0,
    MOCK_EVENT_MESSAGE,
    MOCK_EVENT_SUBACK,
    MOCK_EVENT_PUBACK,
} mock_event_type_t;

static struct {
    mock_event_type_t type;
    bool active;
    uint32_t due_ms;
    char topic[MOCK_MAX_TOPIC_LEN];
    char payload[MOCK_MAX_MSG_LEN];
    size_t payload_len;
} s_events[MOCK_MAX_EVENTS];

static bool s_auto_suback_enabled = false;
static bool s_auto_puback_enabled = false;
static uint32_t s_auto_suback_delay_ms = 0;
static uint32_t s_auto_puback_delay_ms = 0;
static bool s_replay_subscriptions_on_connect = false;
static uint32_t s_mock_time_ms = 0;
static bool s_has_connected_once = false;

static void schedule_event(mock_event_type_t type, const char *topic,
                           const char *payload, size_t payload_len,
                           uint32_t delay_ms)
{
    for (int i = 0; i < MOCK_MAX_EVENTS; i++) {
        if (s_events[i].active) {
            continue;
        }

        s_events[i].type = type;
        s_events[i].active = true;
        s_events[i].due_ms = s_mock_time_ms + delay_ms;
        s_events[i].payload_len = payload_len;
        s_events[i].topic[0] = '\0';
        s_events[i].payload[0] = '\0';

        if (topic != NULL) {
            strncpy(s_events[i].topic, topic, sizeof(s_events[i].topic) - 1);
        }
        if (payload != NULL && payload_len > 0) {
            size_t copy_len = payload_len < sizeof(s_events[i].payload) - 1 ?
                payload_len : sizeof(s_events[i].payload) - 1;
            memcpy(s_events[i].payload, payload, copy_len);
            s_events[i].payload[copy_len] = '\0';
            s_events[i].payload_len = copy_len;
        }
        return;
    }
}

static void replay_subscriptions_if_enabled(void)
{
    if (!s_replay_subscriptions_on_connect) {
        return;
    }

    for (int i = 0; i < MOCK_MAX_SUBS; i++) {
        if (!s_subscriptions[i].active) {
            continue;
        }
        if (mock_subscribe_count < MOCK_MAX_PUBLISHES) {
            strncpy(mock_subscribes[mock_subscribe_count].topic,
                    s_subscriptions[i].topic, MOCK_MAX_TOPIC_LEN - 1);
            mock_subscribes[mock_subscribe_count].qos = 0;
            mock_subscribe_count++;
            mock_subscription_replay_count++;
        }
    }
}

void mqtt_app_mock_reset(void)
{
    mock_publish_count = 0;
    mock_subscribe_count = 0;
    mock_connected = false;
    mock_deinit_count = 0;
    mock_suback_count = 0;
    mock_puback_count = 0;
    mock_subscription_replay_count = 0;
    s_connect_cb = NULL;
    s_disconnect_cb = NULL;
    s_connect_failure_cb = NULL;
    s_auto_suback_enabled = false;
    s_auto_puback_enabled = false;
    s_auto_suback_delay_ms = 0;
    s_auto_puback_delay_ms = 0;
    s_replay_subscriptions_on_connect = false;
    s_mock_time_ms = 0;
    s_has_connected_once = false;
    memset(mock_publishes, 0, sizeof(mock_publishes));
    memset(mock_subscribes, 0, sizeof(mock_subscribes));
    memset(s_subscriptions, 0, sizeof(s_subscriptions));
    memset(s_events, 0, sizeof(s_events));
}

void mqtt_app_mock_deliver_message(const char *topic, const char *payload,
                                   size_t payload_len)
{
    for (int i = 0; i < MOCK_MAX_SUBS; i++) {
        if (!s_subscriptions[i].active) {
            continue;
        }
        /* Simple wildcard matching for MQTT '+' */
        const char *sub = s_subscriptions[i].topic;
        const char *t = topic;

        bool match = true;
        while (*sub && *t) {
            if (*sub == '+') {
                /* Skip one level in topic */
                while (*t && *t != '/') {
                    t++;
                }
                sub++;
                continue;
            }
            if (*sub != *t) {
                match = false;
                break;
            }
            sub++;
            t++;
        }
        if (*sub == '+' && !*t) {
            sub++;
        }
        if (*sub || *t) {
            match = false;
        }

        if (match && s_subscriptions[i].callback != NULL) {
            s_subscriptions[i].callback(topic, payload, payload_len);
        }
    }
}

/* --- mqtt_app.h mock implementation --- */

void mqtt_app_init(void)
{
    mock_connected = true;
    s_has_connected_once = true;
    if (s_connect_cb) {
        s_connect_cb();
    }
}

void mqtt_app_deinit(void)
{
    bool was_connected = mock_connected;
    mock_deinit_count++;
    mock_connected = false;
    if (was_connected && s_disconnect_cb) {
        s_disconnect_cb(MQTT_DISCONNECT_REASON_EXPLICIT);
    }
}

bool mqtt_app_post_data(const char *topic, const char *message, int qos)
{
    if (!mock_connected) {
        return false;
    }
    if (mock_publish_count >= MOCK_MAX_PUBLISHES) {
        return false;
    }

    mock_publish_record_t *rec = &mock_publishes[mock_publish_count++];
    strncpy(rec->topic, topic, MOCK_MAX_TOPIC_LEN - 1);
    strncpy(rec->message, message, MOCK_MAX_MSG_LEN - 1);
    rec->qos = qos;

    if (s_auto_puback_enabled) {
        schedule_event(MOCK_EVENT_PUBACK, NULL, NULL, 0,
                       s_auto_puback_delay_ms);
    }
    return true;
}

bool mqtt_app_is_connected(void)
{
    return mock_connected;
}

bool mqtt_app_subscribe(const char *topic, int qos,
                        mqtt_message_callback_t callback, uint32_t timeout_ms)
{
    (void)timeout_ms;

    for (int i = 0; i < MOCK_MAX_SUBS; i++) {
        if (!s_subscriptions[i].active) {
            strncpy(s_subscriptions[i].topic, topic, MOCK_MAX_TOPIC_LEN - 1);
            s_subscriptions[i].callback = callback;
            s_subscriptions[i].active = true;

            if (mock_subscribe_count < MOCK_MAX_PUBLISHES) {
                strncpy(mock_subscribes[mock_subscribe_count].topic, topic,
                        MOCK_MAX_TOPIC_LEN - 1);
                mock_subscribes[mock_subscribe_count].qos = qos;
                mock_subscribe_count++;
            }

            if (s_auto_suback_enabled) {
                schedule_event(MOCK_EVENT_SUBACK, NULL, NULL, 0,
                               s_auto_suback_delay_ms);
            }
            return true;
        }
    }
    return false;
}

bool mqtt_app_unsubscribe(const char *topic, uint32_t timeout_ms)
{
    (void)timeout_ms;
    for (int i = 0; i < MOCK_MAX_SUBS; i++) {
        if (s_subscriptions[i].active &&
            strcmp(s_subscriptions[i].topic, topic) == 0) {
            s_subscriptions[i].active = false;
            return true;
        }
    }
    return false;
}

void mqtt_app_set_connect_callback(mqtt_connect_callback_t cb)
{
    s_connect_cb = cb;
}

void mqtt_app_set_disconnect_callback(mqtt_disconnect_callback_t cb)
{
    s_disconnect_cb = cb;
}

void mqtt_app_set_connect_failure_callback(mqtt_connect_failure_callback_t cb)
{
    s_connect_failure_cb = cb;
}

void mqtt_app_set_connection_policy(const mqtt_connection_policy_t *policy)
{
    if (policy != NULL) {
        mock_connection_policy = *policy;
    }
}

void mqtt_app_mock_simulate_connect(void)
{
    bool replay = s_has_connected_once && !mock_connected;
    mock_connected = true;
    s_has_connected_once = true;
    if (s_connect_cb) {
        s_connect_cb();
    }
    if (replay) {
        replay_subscriptions_if_enabled();
    }
}

void mqtt_app_mock_simulate_remote_disconnect(void)
{
    bool was_connected = mock_connected;
    mock_connected = false;
    if (was_connected && s_disconnect_cb) {
        s_disconnect_cb(MQTT_DISCONNECT_REASON_REMOTE_CLOSE);
    }
}

void mqtt_app_mock_simulate_error_disconnect(void)
{
    bool was_connected = mock_connected;
    mock_connected = false;
    if (was_connected && s_disconnect_cb) {
        s_disconnect_cb(MQTT_DISCONNECT_REASON_ERROR);
    }
}

void mqtt_app_mock_simulate_connect_failure(mqtt_connect_failure_reason_t reason)
{
    if (s_connect_failure_cb) {
        s_connect_failure_cb(reason);
    }
}

void mqtt_app_mock_schedule_message(const char *topic, const char *payload,
                                    size_t payload_len, uint32_t delay_ms)
{
    schedule_event(MOCK_EVENT_MESSAGE, topic, payload, payload_len, delay_ms);
}

void mqtt_app_mock_advance_time_ms(uint32_t elapsed_ms)
{
    s_mock_time_ms += elapsed_ms;

    bool progressed;
    do {
        progressed = false;
        for (int i = 0; i < MOCK_MAX_EVENTS; i++) {
            if (!s_events[i].active || s_events[i].due_ms > s_mock_time_ms) {
                continue;
            }

            mock_event_type_t type = s_events[i].type;
            char topic[MOCK_MAX_TOPIC_LEN];
            char payload[MOCK_MAX_MSG_LEN];
            size_t payload_len = s_events[i].payload_len;
            strncpy(topic, s_events[i].topic, sizeof(topic) - 1);
            topic[sizeof(topic) - 1] = '\0';
            strncpy(payload, s_events[i].payload, sizeof(payload) - 1);
            payload[sizeof(payload) - 1] = '\0';

            memset(&s_events[i], 0, sizeof(s_events[i]));
            progressed = true;

            switch (type) {
            case MOCK_EVENT_MESSAGE:
                mqtt_app_mock_deliver_message(topic, payload, payload_len);
                break;
            case MOCK_EVENT_SUBACK:
                mock_suback_count++;
                break;
            case MOCK_EVENT_PUBACK:
                mock_puback_count++;
                break;
            default:
                break;
            }
        }
    } while (progressed);
}

void mqtt_app_mock_set_auto_suback(bool enabled, uint32_t delay_ms)
{
    s_auto_suback_enabled = enabled;
    s_auto_suback_delay_ms = delay_ms;
}

void mqtt_app_mock_set_auto_puback(bool enabled, uint32_t delay_ms)
{
    s_auto_puback_enabled = enabled;
    s_auto_puback_delay_ms = delay_ms;
}

void mqtt_app_mock_set_replay_subscriptions_on_connect(bool enabled)
{
    s_replay_subscriptions_on_connect = enabled;
}

/* --- mqtt_config.h mock implementation --- */

static char s_cfg_address[256] = {0};
static char s_cfg_username[128] = {0};
static char s_cfg_password[128] = {0};
static char s_cfg_client_id[128] = {0};
static bool s_cfg_ssl = false;
static bool s_cfg_skip_verify = false;
typedef struct {
    mqtt_cert_source_t source;
    char value[512];
} mock_cert_cfg_t;
static mock_cert_cfg_t s_cfg_cert = {0};
static mock_cert_cfg_t s_cfg_client_cert = {0};
static mock_cert_cfg_t s_cfg_client_key = {0};

static mock_cert_cfg_t *get_cert_cfg(mqtt_config_value_t key)
{
    switch (key) {
    case MQTT_CONFIG_VALUE_CERT:
        return &s_cfg_cert;
    case MQTT_CONFIG_VALUE_CLIENT_CERT:
        return &s_cfg_client_cert;
    case MQTT_CONFIG_VALUE_CLIENT_KEY:
        return &s_cfg_client_key;
    default:
        return NULL;
    }
}

void mqtt_config_init(void)
{
    memset(s_cfg_address, 0, sizeof(s_cfg_address));
    memset(s_cfg_username, 0, sizeof(s_cfg_username));
    memset(s_cfg_password, 0, sizeof(s_cfg_password));
    memset(s_cfg_client_id, 0, sizeof(s_cfg_client_id));
    s_cfg_ssl = false;
    s_cfg_skip_verify = false;
    memset(&s_cfg_cert, 0, sizeof(s_cfg_cert));
    memset(&s_cfg_client_cert, 0, sizeof(s_cfg_client_cert));
    memset(&s_cfg_client_key, 0, sizeof(s_cfg_client_key));
}

bool mqtt_config_set_string(const char *string, mqtt_config_value_t key)
{
    switch (key) {
    case MQTT_CONFIG_VALUE_ADDRESS:
        strncpy(s_cfg_address, string, sizeof(s_cfg_address) - 1);
        return true;
    case MQTT_CONFIG_VALUE_USERNAME:
        strncpy(s_cfg_username, string, sizeof(s_cfg_username) - 1);
        return true;
    case MQTT_CONFIG_VALUE_PASSWORD:
        strncpy(s_cfg_password, string, sizeof(s_cfg_password) - 1);
        return true;
    case MQTT_CONFIG_VALUE_CLIENT_ID:
        strncpy(s_cfg_client_id, string, sizeof(s_cfg_client_id) - 1);
        return true;
    default:
        return false;
    }
}

bool mqtt_config_set_bool(bool value, mqtt_config_value_t key)
{
    switch (key) {
    case MQTT_CONFIG_VALUE_SSL:
        s_cfg_ssl = value;
        return true;
    case MQTT_CONFIG_VALUE_SKIP_VERIFY:
        s_cfg_skip_verify = value;
        return true;
    default:
        return false;
    }
}

bool mqtt_config_set_cert_source(mqtt_cert_source_t source, const char *value,
                                 mqtt_config_value_t key)
{
    mock_cert_cfg_t *cfg = get_cert_cfg(key);
    if (cfg == NULL) {
        return false;
    }

    cfg->source = source;
    cfg->value[0] = '\0';
    if (value != NULL) {
        strncpy(cfg->value, value, sizeof(cfg->value) - 1);
        cfg->value[sizeof(cfg->value) - 1] = '\0';
    }
    return true;
}

bool mqtt_config_get_bool(bool *value, mqtt_config_value_t key)
{
    if (value == NULL) {
        return false;
    }

    switch (key) {
    case MQTT_CONFIG_VALUE_SSL:
        *value = s_cfg_ssl;
        return true;
    case MQTT_CONFIG_VALUE_SKIP_VERIFY:
        *value = s_cfg_skip_verify;
        return true;
    default:
        return false;
    }
}

const char *mqtt_config_get_string(mqtt_config_value_t key)
{
    switch (key) {
    case MQTT_CONFIG_VALUE_ADDRESS:
        return s_cfg_address;
    case MQTT_CONFIG_VALUE_USERNAME:
        return s_cfg_username;
    case MQTT_CONFIG_VALUE_PASSWORD:
        return s_cfg_password;
    case MQTT_CONFIG_VALUE_CLIENT_ID:
        return s_cfg_client_id;
    default:
        return "";
    }
}

const char *mqtt_config_get_cert(mqtt_config_value_t key)
{
    mock_cert_cfg_t *cfg = get_cert_cfg(key);
    if (cfg == NULL) {
        return "";
    }
    return cfg->value;
}

bool mqtt_config_get_cert_source(mqtt_cert_source_t *source, const char **value,
                                 mqtt_config_value_t key)
{
    mock_cert_cfg_t *cfg = get_cert_cfg(key);
    if (cfg == NULL) {
        return false;
    }

    if (source != NULL) {
        *source = cfg->source;
    }
    if (value != NULL) {
        *value = cfg->value;
    }
    return true;
}

bool mqtt_config_save(void)
{
    return true;
}

void mqtt_config_set_callback(mqtt_apply_config_cb cb)
{
    (void)cb;
}
