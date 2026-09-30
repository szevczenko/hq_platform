#include "hq_net.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "wifi_managment.h"

typedef struct hq_net_context {
    _Atomic(osal_mutex_id_t) lock;
    hq_net_callbacks_t callbacks;
    atomic_bool started;
    bool start_inflight;
    uint32_t active_token;
    uint32_t generation;
    uint32_t stop_count;
} hq_net_context_t;

static hq_net_context_t s_ctx = {
    .lock = NULL,
    .callbacks = { NULL, NULL },
    .started = false,
    .start_inflight = false,
    .active_token = 0u,
    .generation = 0u,
    .stop_count = 0u,
};

static bool net_ensure_lock(void)
{
    osal_mutex_id_t current =
        atomic_load_explicit(&s_ctx.lock, memory_order_acquire);
    if (current != NULL) {
        return true;
    }

    osal_mutex_id_t created = NULL;
    if (osal_mutex_create(&created, "hq_net") != OSAL_SUCCESS) {
        return false;
    }

    osal_mutex_id_t expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&s_ctx.lock, &expected,
                                                created,
                                                memory_order_release,
                                                memory_order_acquire)) {
        return true;
    }

    (void)osal_mutex_delete(created);
    return true;
}

static bool net_lock(void)
{
    osal_mutex_id_t lock =
        atomic_load_explicit(&s_ctx.lock, memory_order_acquire);
    return lock != NULL && osal_mutex_take(lock) == OSAL_SUCCESS;
}

static void net_unlock(void)
{
    osal_mutex_id_t lock =
        atomic_load_explicit(&s_ctx.lock, memory_order_acquire);
    if (lock != NULL) {
        (void)osal_mutex_give(lock);
    }
}

static void net_disarm_locked(void)
{
    s_ctx.callbacks.on_connected = NULL;
    s_ctx.callbacks.before_disconnected = NULL;
    s_ctx.callbacks.on_disconnected = NULL;
    s_ctx.callbacks.context = NULL;
    atomic_store_explicit(&s_ctx.started, false, memory_order_release);
}

static void net_deliver(uint32_t token, bool connected)
{
    if (!net_lock()) {
        return;
    }

    if (!atomic_load_explicit(&s_ctx.started, memory_order_acquire) ||
        token != s_ctx.active_token ||
        s_ctx.callbacks.on_connected == NULL ||
        s_ctx.callbacks.on_disconnected == NULL) {
        net_unlock();
        osal_log_debug("[net] dropped late event");
        return;
    }

    hq_net_callbacks_t callbacks = s_ctx.callbacks;
    if (connected) {
        osal_log_info("[net] network connected");
    } else {
        osal_log_warning("[net] network disconnected");
    }
    if (connected) {
        callbacks.on_connected(callbacks.context);
    } else {
        if (callbacks.before_disconnected != NULL) {
            callbacks.before_disconnected(callbacks.context);
        }
        callbacks.on_disconnected(callbacks.context);
    }
    net_unlock();
}

static void net_wifi_event_cb(wifi_mgmt_event_t event, void *user_data)
{
    const uint32_t token = (uint32_t)(uintptr_t)user_data;
    switch (event) {
    case WIFI_MGMT_EVENT_CONNECTED:
        net_deliver(token, true);
        break;
    case WIFI_MGMT_EVENT_DISCONNECTED:
    case WIFI_MGMT_EVENT_CONNECT_FAILED:
        net_deliver(token, false);
        break;
    default:
        break;
    }
}

static void net_unsubscribe(uint32_t token)
{
    void *user_data = (void *)(uintptr_t)token;
    (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECTED,
                                net_wifi_event_cb, user_data);
    (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_DISCONNECTED,
                                net_wifi_event_cb, user_data);
    (void)wifi_mgmt_unsubscribe(WIFI_MGMT_EVENT_CONNECT_FAILED,
                                net_wifi_event_cb, user_data);
}

static bool net_start_was_stopped(uint32_t stop_count_before)
{
    bool stopped = true;
    if (net_lock()) {
        stopped = s_ctx.stop_count != stop_count_before;
        if (stopped) {
            net_disarm_locked();
        }
        net_unlock();
    }
    return stopped;
}

static void net_finish_start(void)
{
    if (net_lock()) {
        s_ctx.start_inflight = false;
        net_unlock();
    }
}

static void net_rollback(uint32_t token)
{
    if (net_lock()) {
        net_disarm_locked();
        net_unlock();
    }
    net_unsubscribe(token);
    (void)wifi_mgmt_stop();
    net_finish_start();
}

int hq_net_start(const hq_net_config_t *config,
                 const hq_net_callbacks_t *callbacks)
{
    if (config == NULL || callbacks == NULL ||
        callbacks->on_connected == NULL ||
        callbacks->on_disconnected == NULL ||
        config->backend != HQ_NET_BACKEND_WIFI ||
                (config->startup_mode != HQ_NET_STARTUP_MODE_PROVISIONING &&
                 config->startup_mode != HQ_NET_STARTUP_MODE_STATION_ONLY) ||
                (config->startup_mode == HQ_NET_STARTUP_MODE_PROVISIONING &&
                 (config->provisioning_ap_name == NULL ||
                    config->provisioning_ap_password == NULL ||
                    config->provisioning_ap_name[0] == '\0' ||
                    config->provisioning_ap_password[0] == '\0'))) {
        return HQ_NET_ERR_INVALID_ARGUMENT;
    }
    if (!net_ensure_lock() || !net_lock()) {
        return HQ_NET_ERR_START_FAILED;
    }
    if (atomic_load_explicit(&s_ctx.started, memory_order_acquire) ||
        s_ctx.start_inflight) {
        net_unlock();
        return HQ_NET_ERR_ALREADY_STARTED;
    }

    s_ctx.callbacks = *callbacks;
    s_ctx.active_token = ++s_ctx.generation;
    const uint32_t token = s_ctx.active_token;
    const uint32_t stop_count_before = s_ctx.stop_count;
    s_ctx.start_inflight = true;
    atomic_store_explicit(&s_ctx.started, true, memory_order_release);
    net_unlock();

    wifi_mgmt_init();
    if (config->startup_mode == HQ_NET_STARTUP_MODE_PROVISIONING &&
        !wifi_mgmt_set_ap_credentials(config->provisioning_ap_name,
                                      config->provisioning_ap_password)) {
        net_rollback(token);
        return HQ_NET_ERR_START_FAILED;
    }
    if (config->startup_mode == HQ_NET_STARTUP_MODE_STATION_ONLY) {
        wifi_mgmt_set_wifi_type(T_WIFI_TYPE_CLIENT);
    } else {
        wifi_mgmt_set_wifi_type(wifi_mgmt_is_read_data()
                                    ? T_WIFI_TYPE_CLIENT
                                    : T_WIFI_TYPE_CLI_SER);
    }

    void *user_data = (void *)(uintptr_t)token;
    const bool subscribed =
        wifi_mgmt_subscribe(WIFI_MGMT_EVENT_CONNECTED,
                            net_wifi_event_cb, user_data) &&
        wifi_mgmt_subscribe(WIFI_MGMT_EVENT_DISCONNECTED,
                            net_wifi_event_cb, user_data) &&
        wifi_mgmt_subscribe(WIFI_MGMT_EVENT_CONNECT_FAILED,
                            net_wifi_event_cb, user_data);
    wifi_mgmt_start();

    const bool stopped_during_start = net_start_was_stopped(stop_count_before);
    const bool ready = !stopped_during_start &&
                       wifi_mgmt_wait_ready(HQ_NET_START_TIMEOUT_MS);
    const bool stopped_during_wait =
        stopped_during_start || net_start_was_stopped(stop_count_before);

    if (!subscribed || !ready || stopped_during_wait) {
        osal_log_error("[net] Wi-Fi startup failed (subscribed=%d ready=%d "
                       "stopped=%d)",
                       (int)subscribed, (int)ready, (int)stopped_during_wait);
        net_rollback(token);
        return HQ_NET_ERR_START_FAILED;
    }

    if (wifi_mgmt_is_read_data() && !wifi_mgmt_connect()) {
        osal_log_error("[net] Wi-Fi connect request rejected");
        net_deliver(token, false);
        net_rollback(token);
        return HQ_NET_ERR_START_FAILED;
    }

    if (net_start_was_stopped(stop_count_before)) {
        osal_log_error("[net] Wi-Fi startup aborted by concurrent stop");
        net_unsubscribe(token);
        (void)wifi_mgmt_stop();
        net_finish_start();
        return HQ_NET_ERR_START_FAILED;
    }

    osal_log_info("[net] network started");
    net_finish_start();
    return HQ_NET_OK;
}

void hq_net_stop(void)
{
    if (!net_ensure_lock() || !net_lock()) {
        return;
    }
    if (!atomic_load_explicit(&s_ctx.started, memory_order_acquire)) {
        net_unlock();
        return;
    }

    ++s_ctx.stop_count;
    const uint32_t token = s_ctx.active_token;
    net_disarm_locked();
    net_unlock();

    net_unsubscribe(token);
    (void)wifi_mgmt_disconnect();
    osal_log_info("[net] network stopped");
}

bool hq_net_is_connected(void)
{
    return atomic_load_explicit(&s_ctx.started, memory_order_acquire) &&
           wifi_mgmt_is_connected();
}

bool hq_net_wait_connected(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0u;
    while (!hq_net_is_connected()) {
        if (timeout_ms == 0u) {
            return false;
        }
        if (timeout_ms - waited_ms < HQ_NET_WAIT_POLL_INTERVAL_MS) {
            (void)osal_task_delay_ms(timeout_ms - waited_ms);
            return hq_net_is_connected();
        }
        (void)osal_task_delay_ms(HQ_NET_WAIT_POLL_INTERVAL_MS);
        waited_ms += HQ_NET_WAIT_POLL_INTERVAL_MS;
    }
    return true;
}

int hq_net_reconnect(void)
{
    if (!atomic_load_explicit(&s_ctx.started, memory_order_acquire) ||
        hq_net_is_connected()) {
        return HQ_NET_OK;
    }
    return wifi_mgmt_connect() ? HQ_NET_OK : HQ_NET_ERR_START_FAILED;
}

bool hq_net_get_rssi(int *rssi_dbm)
{
    if (rssi_dbm == NULL || !hq_net_is_connected()) {
        return false;
    }
    *rssi_dbm = wifi_mgmt_get_rssi();
    return true;
}

bool hq_net_get_link_quality(uint8_t *quality_percent, int *rssi_dbm)
{
    int rssi;
    if (quality_percent == NULL || !hq_net_get_rssi(&rssi)) {
        return false;
    }
    if (rssi <= -100) {
        *quality_percent = 0u;
    } else if (rssi >= -50) {
        *quality_percent = 100u;
    } else {
        *quality_percent = (uint8_t)((rssi + 100) * 2);
    }
    if (rssi_dbm != NULL) {
        *rssi_dbm = rssi;
    }
    return true;
}

bool hq_net_forget_credentials(void)
{
    return wifi_mgmt_erase_credentials();
}

#ifdef HQ_NET_TEST_OBSERVABILITY
osal_mutex_id_t hq_net_test_get_lock(void)
{
    return atomic_load_explicit(&s_ctx.lock, memory_order_acquire);
}
#endif