/**
 * @file hq_app_state.c
 * @brief Application state machine and watchdog policy (TASK-115)
 *
 * See hq_app_state.h for the normative contract.  Implementation notes:
 *
 *   - the legal-transition table is data (a static array) and every
 *     delivery validates event, owner AND session against the machine's
 *     current state before any transition is performed; everything that
 *     does not match is dropped and counted,
 *   - safe-state behavior is delegated to configured callbacks,
 *   - the retry schedule is a single (attempt counter, delay, deadline)
 *     triple driven by poll(); the delay grows exponentially
 *     (retry_delay_ms * factor, capped) and parks when the per-episode
 *     budget is spent, which bounds the reconnect storm,
 *   - the watchdog is a single deadline refreshed by poll() (the one feed
 *     point) and by every successful transition; an expired deadline is
 *     only observed on the next poll and always ends in
 *     #HQ_APP_STATE_FATAL — never in a spontaneous resume,
 *   - lock discipline: one module mutex guards every public operation;
 *     the optional observer and watchdog callbacks run with the lock held
 *     (short, non-blocking, no API re-entry — same contract as the
 *     network adapter's callbacks).
 */

#include "hq_app_state.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"

/* --------------------------------------------------------------------- */
/* Module state                                                           */
/* --------------------------------------------------------------------- */

/* Sentinel used for "no retry target" table slots; must not match a
 * real state (real states are 0..10, so -1 is safe). */
#define HQ_APP_STATE_NONE_SAFE ((hq_app_state_t)-1)

/** @brief One legal transition of the machine. */
typedef struct hq_app_state_transition {
    hq_app_state_t           from;          /**< Source state. */
    hq_app_state_event_t     event;         /**< Event that fires it. */
    hq_app_state_t           to;            /**< Destination state. */
    hq_app_transition_owner_t owner;        /**< REQUIRED transition owner. */
    hq_app_failure_class_t fcls;         /**< Failure class (failures only). */
    hq_app_state_t           retry_target;  /**< Retry re-entry state (retryable only). */
    bool                  new_episode;   /**< Bump the session/generation. */
} hq_app_state_transition_t;

/**
 * @brief Legal transitions (TASK-115 table; see hq_app_state.h).
 *
 * The special transitions are handled before the table:
 *   - start():      BOOT -> FILESYSTEM (owner BOOTSTRAP),
 *   - RESET:        any state -> BOOT (owner EXTERNAL; new episode),
 *   - FATAL:        any state -> FATAL (any owner; always honored),
 *   - RETRY_DUE:    SAFE_OFF -> retry target (owner TIMER; new episode).
 *
 * The DISCONNECTED rows below are encoded with owner HQ_APP_OWNER_MQTT;
 * app_owner_ok() deliberately widens DISCONNECT-class rows to ALSO accept
 * HQ_APP_OWNER_NETWORK (both transports may report "transport down" — see the
 * header table, which documents those rows as owned by NETWORK/MQTT).
 *
 * The PROVISIONING rows are owned by HQ_APP_OWNER_NETWORK: the provisioning
 * adapter belongs to the network stage, so NETWORK may enter PROVISIONING
 * on PROVISIONING_STARTED (no saved credential) and PROVISIONING returns
 * to NETWORK on PROVISIONING_SUCCEEDED (credential saved) or degrades to
 * SAFE_OFF on PROVISIONING_FAILED with the bounded retry target NETWORK
 * (the retry re-enters the network stage, never PROVISIONING directly).
 */
static const hq_app_state_transition_t HQ_APP_STATE_TRANSITIONS[] = {
    { HQ_APP_STATE_FILESYSTEM,    HQ_APP_EVENT_FS_OK,             HQ_APP_STATE_CONFIGURATION, HQ_APP_OWNER_FILESYSTEM,    HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NONE_SAFE,          false },
    { HQ_APP_STATE_FILESYSTEM,    HQ_APP_EVENT_FS_FAIL,           HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_FILESYSTEM,    HQ_APP_FAILURE_DEGRADED,  HQ_APP_STATE_NONE_SAFE,          false },
    { HQ_APP_STATE_FILESYSTEM,    HQ_APP_EVENT_DISCONNECTED,      HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_MQTT,          HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NETWORK,           false },
    { HQ_APP_STATE_CONFIGURATION, HQ_APP_EVENT_CONFIG_OK,         HQ_APP_STATE_NETWORK,       HQ_APP_OWNER_CONFIGURATION, HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NONE_SAFE,          false },
    { HQ_APP_STATE_CONFIGURATION, HQ_APP_EVENT_CONFIG_FAIL,       HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_CONFIGURATION, HQ_APP_FAILURE_DEGRADED,  HQ_APP_STATE_NONE_SAFE,          false },
    { HQ_APP_STATE_CONFIGURATION, HQ_APP_EVENT_DISCONNECTED,      HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_MQTT,          HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NETWORK,           false },
    { HQ_APP_STATE_NETWORK,       HQ_APP_EVENT_NETWORK_CONNECTED, HQ_APP_STATE_TLS,           HQ_APP_OWNER_NETWORK,       HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NONE_SAFE,          false },
    { HQ_APP_STATE_NETWORK,       HQ_APP_EVENT_NETWORK_FAILED,    HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_NETWORK,       HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NETWORK,           false },
    { HQ_APP_STATE_NETWORK,       HQ_APP_EVENT_PROVISIONING_STARTED,   HQ_APP_STATE_PROVISIONING, HQ_APP_OWNER_NETWORK,  HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NONE_SAFE,          false },
    { HQ_APP_STATE_PROVISIONING,  HQ_APP_EVENT_PROVISIONING_SUCCEEDED, HQ_APP_STATE_NETWORK,      HQ_APP_OWNER_NETWORK,  HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NONE_SAFE,          false },
    { HQ_APP_STATE_PROVISIONING,  HQ_APP_EVENT_PROVISIONING_FAILED,    HQ_APP_STATE_SAFE_OFF,     HQ_APP_OWNER_NETWORK,  HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NETWORK,           false },
    { HQ_APP_STATE_NETWORK,       HQ_APP_EVENT_DISCONNECTED,      HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_MQTT,          HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NETWORK,           false },
    { HQ_APP_STATE_TLS,           HQ_APP_EVENT_TLS_CONNECTED,     HQ_APP_STATE_SYNC,          HQ_APP_OWNER_MQTT,          HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NONE_SAFE,          false },
    { HQ_APP_STATE_TLS,           HQ_APP_EVENT_TLS_FAILED,        HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_MQTT,          HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_TLS,               false },
    { HQ_APP_STATE_TLS,           HQ_APP_EVENT_DISCONNECTED,      HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_MQTT,          HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NETWORK,           false },
    { HQ_APP_STATE_SYNC,          HQ_APP_EVENT_SYNC_COMPLETE,     HQ_APP_STATE_ONLINE,        HQ_APP_OWNER_THINGSBOARD,   HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NONE_SAFE,          false },
    { HQ_APP_STATE_SYNC,          HQ_APP_EVENT_SYNC_FAILED,       HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_THINGSBOARD,   HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_SYNC,              false },
    { HQ_APP_STATE_SYNC,          HQ_APP_EVENT_INVALID_STATE,     HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_THINGSBOARD,   HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_SYNC,              false },
    { HQ_APP_STATE_SYNC,          HQ_APP_EVENT_DISCONNECTED,      HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_MQTT,          HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NETWORK,           false },
    { HQ_APP_STATE_ONLINE,        HQ_APP_EVENT_DISCONNECTED,      HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_MQTT,          HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NETWORK,           false },
    { HQ_APP_STATE_ONLINE,        HQ_APP_EVENT_INVALID_STATE,     HQ_APP_STATE_SAFE_OFF,      HQ_APP_OWNER_THINGSBOARD,   HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_SYNC,              false },
    { HQ_APP_STATE_ONLINE,        HQ_APP_EVENT_OTA_BEGIN,         HQ_APP_STATE_OTA,           HQ_APP_OWNER_OTA,           HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NONE_SAFE,          true  },
    { HQ_APP_STATE_SAFE_OFF,      HQ_APP_EVENT_OTA_BEGIN,         HQ_APP_STATE_OTA,           HQ_APP_OWNER_OTA,           HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NONE_SAFE,          true  },
    { HQ_APP_STATE_OTA,           HQ_APP_EVENT_OTA_END,           HQ_APP_STATE_BOOT,          HQ_APP_OWNER_OTA,           HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NONE_SAFE,          true  },
    { HQ_APP_STATE_OTA,           HQ_APP_EVENT_OTA_FAILED,        HQ_APP_STATE_ONLINE,        HQ_APP_OWNER_OTA,           HQ_APP_FAILURE_RETRYABLE, HQ_APP_STATE_NONE_SAFE,          true  },
};

/** @brief Resolved module configuration (immutable after init). */
typedef struct hq_app_state_runtime {
    hq_app_state_now_fn_t       now_fn;
    uint32_t                 retry_initial_delay_ms;
    uint32_t                 retry_max_delay_ms;
    uint32_t                 retry_max_attempts;
    uint32_t                 retry_backoff_factor;
    uint32_t                 watchdog_timeout_ms;
    hq_app_state_watchdog_fn_t  on_watchdog_expired;
    hq_app_state_enter_fn_t on_state_enter;
    hq_app_state_safe_fn_t on_safe_state;
    hq_app_state_observer_fn_t  observer;
} hq_app_state_runtime_t;

typedef struct hq_app_state_internal {
    /* The module mutex is created lazily and adopted with a CAS (first-use
     * race, exactly like the network adapter's lock): the very first
     * init() publishes it, every later operation loads it atomically. */
    _Atomic(osal_mutex_id_t) lock;
    bool                   initialized;
    hq_app_state_runtime_t    cfg;
    hq_app_state_t            state;
    uint32_t               session;
    hq_app_transition_owner_t last_owner;

    /* Watchdog */
    bool                   wdt_armed;
    uint32_t               deadline_ms;

    /* Retry schedule */
    bool                   retry_pending;
    uint32_t               retry_due_ms;
    uint32_t               retry_delay_ms;
    uint32_t               retry_attempts;
    bool                   retry_exhausted;
    hq_app_state_t            retry_target;

    /* Drop counters */
    uint32_t               stale_dropped;
    uint32_t               invalid_dropped;
} hq_app_state_internal_t;

static hq_app_state_internal_t s_state = {
    .lock        = NULL,
    .initialized = false,
    .state       = HQ_APP_STATE_BOOT,
    .last_owner  = HQ_APP_OWNER_EXTERNAL,
};

/* --------------------------------------------------------------------- */
/* Small helpers                                                          */
/* --------------------------------------------------------------------- */

/** @brief Wrap-safe "now >= when" comparison (valid within a 2^31 ms
 *         window, i.e. for delays far below the 49.7-day clock wrap). */
static bool app_time_ge(uint32_t now, uint32_t when)
{
    return (int32_t)(now - when) >= 0;
}

static uint32_t app_now(void)
{
    return s_state.cfg.now_fn();
}

static bool hq_app_state_is_disconnect_class(hq_app_state_event_t event)
{
    switch (event)
    {
    case HQ_APP_EVENT_FS_FAIL:
    case HQ_APP_EVENT_CONFIG_FAIL:
    case HQ_APP_EVENT_NETWORK_FAILED:
    case HQ_APP_EVENT_PROVISIONING_FAILED:
    case HQ_APP_EVENT_TLS_FAILED:
    case HQ_APP_EVENT_SYNC_FAILED:
    case HQ_APP_EVENT_DISCONNECTED:
    case HQ_APP_EVENT_INVALID_STATE:
    case HQ_APP_EVENT_OTA_FAILED:
        return true;
    default:
        return false;
    }
}

static const char *hq_app_state_name(hq_app_state_t state)
{
    switch (state)
    {
    case HQ_APP_STATE_BOOT:          return "boot";
    case HQ_APP_STATE_FILESYSTEM:    return "filesystem";
    case HQ_APP_STATE_CONFIGURATION: return "configuration";
    case HQ_APP_STATE_NETWORK:       return "network";
    case HQ_APP_STATE_PROVISIONING:  return "provisioning";
    case HQ_APP_STATE_TLS:           return "tls";
    case HQ_APP_STATE_SYNC:          return "sync";
    case HQ_APP_STATE_ONLINE:        return "online";
    case HQ_APP_STATE_SAFE_OFF:      return "safe-off";
    case HQ_APP_STATE_FATAL:         return "fatal";
    case HQ_APP_STATE_OTA:           return "ota";
    }
    return "?";
}

static const char *app_event_name(hq_app_state_event_t event)
{
    switch (event)
    {
    case HQ_APP_EVENT_START:             return "start";
    case HQ_APP_EVENT_FS_OK:             return "fs-ok";
    case HQ_APP_EVENT_FS_FAIL:           return "fs-fail";
    case HQ_APP_EVENT_CONFIG_OK:         return "config-ok";
    case HQ_APP_EVENT_CONFIG_FAIL:       return "config-fail";
    case HQ_APP_EVENT_NETWORK_CONNECTED: return "network-connected";
    case HQ_APP_EVENT_NETWORK_FAILED:    return "network-failed";
    case HQ_APP_EVENT_PROVISIONING_STARTED:   return "provisioning-started";
    case HQ_APP_EVENT_PROVISIONING_SUCCEEDED: return "provisioning-succeeded";
    case HQ_APP_EVENT_PROVISIONING_FAILED:    return "provisioning-failed";
    case HQ_APP_EVENT_TLS_CONNECTED:     return "tls-connected";
    case HQ_APP_EVENT_TLS_FAILED:        return "tls-failed";
    case HQ_APP_EVENT_SYNC_COMPLETE:     return "sync-complete";
    case HQ_APP_EVENT_SYNC_FAILED:       return "sync-failed";
    case HQ_APP_EVENT_DISCONNECTED:      return "disconnected";
    case HQ_APP_EVENT_INVALID_STATE:     return "invalid-state";
    case HQ_APP_EVENT_OTA_BEGIN:         return "ota-begin";
    case HQ_APP_EVENT_OTA_END:           return "ota-end";
    case HQ_APP_EVENT_OTA_FAILED:        return "ota-failed";
    case HQ_APP_EVENT_RETRY_DUE:         return "retry-due";
    case HQ_APP_EVENT_RESET:             return "reset";
    case HQ_APP_EVENT_FATAL:             return "fatal";
    }
    return "?";
}

static const char *app_owner_name(hq_app_transition_owner_t owner)
{
    switch (owner)
    {
    case HQ_APP_OWNER_BOOTSTRAP:    return "bootstrap";
    case HQ_APP_OWNER_FILESYSTEM:   return "filesystem";
    case HQ_APP_OWNER_CONFIGURATION:return "configuration";
    case HQ_APP_OWNER_NETWORK:      return "network";
    case HQ_APP_OWNER_MQTT:         return "mqtt";
    case HQ_APP_OWNER_THINGSBOARD:  return "thingsboard";
    case HQ_APP_OWNER_TIMER:        return "timer";
    case HQ_APP_OWNER_OTA:          return "ota";
    case HQ_APP_OWNER_WATCHDOG:     return "watchdog";
    case HQ_APP_OWNER_EXTERNAL:     return "external";
    }
    return "?";
}

/* --------------------------------------------------------------------- */
/* Fail-off / transition helpers (called with the lock held)              */
/* --------------------------------------------------------------------- */

static void app_notify_safe(hq_app_state_t state,
                            hq_app_state_event_t event,
                            hq_app_transition_owner_t owner,
                            uint32_t session)
{
    if (s_state.cfg.on_safe_state != NULL)
    {
        s_state.cfg.on_safe_state(state, event, owner, session);
    }
}

static void app_clear_retry(void)
{
    s_state.retry_pending   = false;
    s_state.retry_due_ms    = 0U;
    s_state.retry_delay_ms  = 0U;
    s_state.retry_attempts  = 0U;
    s_state.retry_exhausted = false;
    s_state.retry_target    = HQ_APP_STATE_NONE_SAFE;
}

/**
 * @brief Execute one state transition (lock held).
 *
 * Runs configured state callbacks synchronously before transition completion.
 */
static void app_do_transition(hq_app_state_t to, hq_app_state_event_t event,
                              hq_app_transition_owner_t owner)
{
    hq_app_state_t from = s_state.state;

    s_state.state     = to;
    s_state.last_owner = owner;

    if (to != HQ_APP_STATE_ONLINE)
    {
        app_notify_safe(to, event, owner, s_state.session);
    }

    if (s_state.wdt_armed)
    {
        s_state.deadline_ms = app_now();
    }

    if (s_state.cfg.on_state_enter != NULL)
    {
        s_state.cfg.on_state_enter(from, to, event, owner, s_state.session);
    }

    if (s_state.cfg.observer != NULL)
    {
        s_state.cfg.observer(from, to, event, owner, s_state.session);
    }

    osal_log_info("[hq_app_state] %s --%s(%s)--> %s (session %u)",
                  hq_app_state_name(from), app_event_name(event),
                  app_owner_name(owner), hq_app_state_name(to),
                  (unsigned)s_state.session);
}

/**
 * @brief Schedule (or refuse) the bounded backoff retry for a retryable
 *        failure that just parked the machine in SAFE_OFF (lock held).
 *
 * The delay grows per consumed attempt (exponentially, capped); when the
 * per-episode budget is spent the machine parks with no further retry.
 */
static void app_arm_retry(hq_app_state_t target)
{
    if (s_state.retry_attempts >= s_state.cfg.retry_max_attempts)
    {
        /* Budget exhausted: park silently — this is the anti-storm rule.
         * Nothing is scheduled, so the reported delay is cleared too. */
        s_state.retry_pending   = false;
        s_state.retry_due_ms    = 0U;
        s_state.retry_delay_ms  = 0U;
        s_state.retry_exhausted = true;
        return;
    }

    uint32_t delay;
    if (s_state.retry_attempts == 0U)
    {
        delay = s_state.cfg.retry_initial_delay_ms;
    }
    else
    {
        uint64_t grown = (uint64_t)s_state.retry_delay_ms *
                         (uint64_t)s_state.cfg.retry_backoff_factor;
        delay = (grown >= s_state.cfg.retry_max_delay_ms)
                    ? s_state.cfg.retry_max_delay_ms
                    : (uint32_t)grown;
    }

    s_state.retry_delay_ms  = delay;
    s_state.retry_due_ms    = app_now() + delay;
    s_state.retry_pending   = true;
    s_state.retry_exhausted = false;
    s_state.retry_target    = target;
}

/**
 * @brief Execute a failure transition to SAFE_OFF (lock held).
 *
 * @param[in] entry The table entry that fired.
 */
static void app_enter_safe_off(const hq_app_state_transition_t *entry)
{
    if (entry->fcls == HQ_APP_FAILURE_RETRYABLE)
    {
        app_arm_retry(entry->retry_target);
    }
    else
    {
        /* Degraded (non-retryable): park without an automatic retry.
         * retry_attempts is deliberately NOT cleared: a degraded park must
         * not refill the retry budget (that is the anti-storm rule — do not
         * "reset the budget on degraded park" without revisiting this). */
        s_state.retry_pending   = false;
        s_state.retry_due_ms    = 0U;
        s_state.retry_exhausted = false;
        s_state.retry_target    = HQ_APP_STATE_NONE_SAFE;
    }
    app_do_transition(HQ_APP_STATE_SAFE_OFF, entry->event, entry->owner);
}

/* --------------------------------------------------------------------- */
/* Table lookup                                                           */
/* --------------------------------------------------------------------- */

static const hq_app_state_transition_t *app_lookup(hq_app_state_t state,
                                                hq_app_state_event_t event)
{
    size_t i;

    for (i = 0; i < (sizeof(HQ_APP_STATE_TRANSITIONS) /
                     sizeof(HQ_APP_STATE_TRANSITIONS[0])); ++i)
    {
        if ((HQ_APP_STATE_TRANSITIONS[i].from == state) &&
            (HQ_APP_STATE_TRANSITIONS[i].event == event))
        {
            return &HQ_APP_STATE_TRANSITIONS[i];
        }
    }
    return NULL;
}

/**
 * @brief Owner acceptance rule (lock held).
 *
 * The DISCONNECTED event may be reported by either transport context —
 * the Wi-Fi adapter (HQ_APP_OWNER_NETWORK) or the MQTT/TLS layer
 * (HQ_APP_OWNER_MQTT) — both are legitimate owners of "transport down".
 */
static bool app_owner_ok(const hq_app_state_transition_t *entry,
                         hq_app_transition_owner_t owner)
{
    if (entry->owner == owner)
    {
        return true;
    }
    return (entry->event == HQ_APP_EVENT_DISCONNECTED) &&
           ((owner == HQ_APP_OWNER_NETWORK) || (owner == HQ_APP_OWNER_MQTT));
}

/* --------------------------------------------------------------------- */
/* Locking                                                                */
/* --------------------------------------------------------------------- */

static bool app_lock(void)
{
    osal_mutex_id_t lock =
        atomic_load_explicit(&s_state.lock, memory_order_acquire);

    if ((lock == NULL) || (osal_mutex_take(lock) != OSAL_SUCCESS))
    {
        osal_log_error("[hq_app_state] lock unavailable");
        return false;
    }
    return true;
}

static void app_unlock(void)
{
    osal_mutex_id_t lock =
        atomic_load_explicit(&s_state.lock, memory_order_acquire);

    if (lock != NULL)
    {
        (void)osal_mutex_give(lock);
    }
}

/* --------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* --------------------------------------------------------------------- */

hq_app_state_status_t hq_app_state_init(const hq_app_state_config_t *config)
{
    osal_mutex_id_t created;
    osal_mutex_id_t expected;

    if (config == NULL)
    {
        return HQ_APP_STATE_ERR_INVALID_ARGUMENT;
    }

    /* Create the module mutex and CAS-adopt it into the published slot
     * (first-use race, see the lock comment above).  Only one init can win
     * the adoption for a given lifecycle generation. */
    created = NULL;
    if (osal_mutex_create(&created, "hq_app_state") != OSAL_SUCCESS)
    {
        return HQ_APP_STATE_ERR_NOT_INITIALIZED;
    }

    expected = NULL;
    if (!atomic_compare_exchange_strong(&s_state.lock, &expected, created))
    {
        /* Another thread already published a lock: the module is (or was,
         * pending deinit) initialized.  We never call back into the winner
         * under any lock here - the caller-visible guarantee is "already
         * initialized". */
        (void)osal_mutex_delete(created);
        return HQ_APP_STATE_ERR_ALREADY_INITIALIZED;
    }

    /* We own the fresh lock; nobody can be inside the module yet, so no
     * take() is needed while we fill the state.  The lock is released
     * (given back) at the end for the first real user. */
    s_state.cfg.now_fn = config->now_ms;
    if (s_state.cfg.now_fn == NULL)
    {
        s_state.cfg.now_fn = osal_task_get_time_ms;
    }
    s_state.cfg.retry_initial_delay_ms =
        (config->retry_initial_delay_ms == 0U)
            ? HQ_APP_STATE_RETRY_INITIAL_DELAY_DEFAULT_MS
            : config->retry_initial_delay_ms;
    if (s_state.cfg.retry_initial_delay_ms < HQ_APP_STATE_RETRY_DELAY_MIN_MS)
    {
        s_state.cfg.retry_initial_delay_ms = HQ_APP_STATE_RETRY_DELAY_MIN_MS;
    }
    if (s_state.cfg.retry_initial_delay_ms > HQ_APP_STATE_RETRY_DELAY_MAX_MS)
    {
        s_state.cfg.retry_initial_delay_ms = HQ_APP_STATE_RETRY_DELAY_MAX_MS;
    }
    s_state.cfg.retry_max_delay_ms =
        (config->retry_max_delay_ms == 0U)
            ? HQ_APP_STATE_RETRY_MAX_DELAY_DEFAULT_MS
            : config->retry_max_delay_ms;
    if (s_state.cfg.retry_max_delay_ms < HQ_APP_STATE_RETRY_DELAY_MIN_MS)
    {
        s_state.cfg.retry_max_delay_ms = HQ_APP_STATE_RETRY_DELAY_MIN_MS;
    }
    if (s_state.cfg.retry_max_delay_ms > HQ_APP_STATE_RETRY_DELAY_MAX_MS)
    {
        s_state.cfg.retry_max_delay_ms = HQ_APP_STATE_RETRY_DELAY_MAX_MS;
    }
    if (s_state.cfg.retry_max_delay_ms <
        s_state.cfg.retry_initial_delay_ms)
    {
        /* Keep the schedule sane: cap >= initial. */
        s_state.cfg.retry_max_delay_ms = s_state.cfg.retry_initial_delay_ms;
    }
    s_state.cfg.retry_max_attempts =
        (config->retry_max_attempts == 0U)
            ? HQ_APP_STATE_RETRY_MAX_ATTEMPTS_DEFAULT
            : config->retry_max_attempts;
    if (s_state.cfg.retry_max_attempts == 0U)
    {
        /* Clamp keeps at least one retry possible. */
        s_state.cfg.retry_max_attempts = 1U;
    }
    s_state.cfg.retry_backoff_factor =
        (config->retry_backoff_factor == 0U)
            ? HQ_APP_STATE_RETRY_BACKOFF_FACTOR_DEFAULT
            : config->retry_backoff_factor;
    if (s_state.cfg.retry_backoff_factor < 1U)
    {
        s_state.cfg.retry_backoff_factor = 1U;
    }
    if (s_state.cfg.retry_backoff_factor > 10U)
    {
        s_state.cfg.retry_backoff_factor = 10U;
    }
    s_state.cfg.watchdog_timeout_ms =
        (config->watchdog_timeout_ms == 0U)
            ? HQ_APP_STATE_WATCHDOG_TIMEOUT_DEFAULT_MS
            : config->watchdog_timeout_ms;
    if (s_state.cfg.watchdog_timeout_ms < HQ_APP_STATE_WATCHDOG_TIMEOUT_MIN_MS)
    {
        s_state.cfg.watchdog_timeout_ms = HQ_APP_STATE_WATCHDOG_TIMEOUT_MIN_MS;
    }
    if (s_state.cfg.watchdog_timeout_ms > HQ_APP_STATE_WATCHDOG_TIMEOUT_MAX_MS)
    {
        s_state.cfg.watchdog_timeout_ms = HQ_APP_STATE_WATCHDOG_TIMEOUT_MAX_MS;
    }
    s_state.cfg.on_watchdog_expired = config->on_watchdog_expired;
    s_state.cfg.on_state_enter       = config->on_state_enter;
    s_state.cfg.on_safe_state        = config->on_safe_state;
    s_state.cfg.observer            = config->observer;

    /* Fresh machine: Boot, session 0, watchdog armed but parked until
     * start() provides the first deadline. */
    s_state.state         = HQ_APP_STATE_BOOT;
    s_state.session       = 0U;
    s_state.last_owner    = HQ_APP_OWNER_EXTERNAL;
    s_state.wdt_armed     = true;
    s_state.deadline_ms   = s_state.cfg.now_fn();
    app_clear_retry();
    s_state.stale_dropped   = 0U;
    s_state.invalid_dropped = 0U;
    s_state.initialized     = true;

    osal_log_info("[hq_app_state] initialized (watchdog %u ms, retry "
                  "initial %u ms / cap %u ms / %u attempts)",
                  (unsigned)s_state.cfg.watchdog_timeout_ms,
                  (unsigned)s_state.cfg.retry_initial_delay_ms,
                  (unsigned)s_state.cfg.retry_max_delay_ms,
                  (unsigned)s_state.cfg.retry_max_attempts);

    /* Release the fresh lock for the first real user, then report
     * success.  The mutex give cannot fail on the freshly adopted lock. */
    (void)osal_mutex_give(s_state.lock);
    return HQ_APP_STATE_OK;
}

void hq_app_state_deinit(void)
{
    osal_mutex_id_t lock;

    if (!app_lock())
    {
        return;
    }

    if (s_state.initialized)
    {
        lock = atomic_load_explicit(&s_state.lock, memory_order_acquire);
        osal_log_info("[hq_app_state] deinitialized");
        s_state.initialized = false;
        s_state.state       = HQ_APP_STATE_BOOT;
        s_state.session     = 0U;
        s_state.wdt_armed   = false;
        /* Give the module mutex back, unpublish it, THEN delete it: no
         * later operation can observe a lock that is being destroyed. */
        app_unlock();
        atomic_store_explicit(&s_state.lock, NULL, memory_order_release);
        (void)osal_mutex_delete(lock);
    }
    else
    {
        app_unlock();
    }
}

hq_app_state_status_t hq_app_state_start(void)
{
    if (!app_lock())
    {
        return HQ_APP_STATE_ERR_NOT_INITIALIZED;
    }

    if (!s_state.initialized)
    {
        app_unlock();
        return HQ_APP_STATE_ERR_NOT_INITIALIZED;
    }

    if (s_state.state != HQ_APP_STATE_BOOT)
    {
        osal_log_warning("[hq_app_state] start() rejected: not in boot "
                         "(state %s)", hq_app_state_name(s_state.state));
        app_unlock();
        return HQ_APP_STATE_ERR_STATE;
    }

    /* New boot episode: fresh session, watchdog (re)armed, output off. */
    s_state.session++;
    s_state.wdt_armed   = true;
    s_state.deadline_ms = app_now();
    app_do_transition(HQ_APP_STATE_FILESYSTEM, HQ_APP_EVENT_START,
                      HQ_APP_OWNER_BOOTSTRAP);
    app_unlock();
    return HQ_APP_STATE_OK;
}

/* --------------------------------------------------------------------- */
/* Event delivery                                                         */
/* --------------------------------------------------------------------- */

static hq_app_state_status_t hq_app_state_deliver_locked(hq_app_state_event_t event,
                                                   hq_app_transition_owner_t owner,
                                                   uint32_t session)
{
    /* Fatal reports are always honored: any session, any owner, any state.
     * A device that reaches FATAL stays there until an explicit reset. */
    if (event == HQ_APP_EVENT_FATAL)
    {
        if (s_state.state != HQ_APP_STATE_FATAL)
        {
            app_clear_retry();
            s_state.session++;
            app_do_transition(HQ_APP_STATE_FATAL, event, owner);
        }
        return HQ_APP_STATE_OK;
    }

    /* Events that only the machine itself may fire. */
    if ((event == HQ_APP_EVENT_START) || (event == HQ_APP_EVENT_RETRY_DUE))
    {
        s_state.invalid_dropped++;
        osal_log_warning("[hq_app_state] non-deliverable event %s dropped",
                         app_event_name(event));
        return HQ_APP_STATE_ERR_ILLEGAL;
    }

    /* Stale session check: the callback belongs to a previous episode. */
    if (!hq_app_state_is_disconnect_class(event) && (session != s_state.session))
    {
        /* Non-safety events from a stale session carry no obligation. */
        s_state.stale_dropped++;
        osal_log_warning("[hq_app_state] stale event %s (session %u != %u) "
                         "dropped", app_event_name(event), (unsigned)session,
                         (unsigned)s_state.session);
        return HQ_APP_STATE_ERR_STALE;
    }
    if (hq_app_state_is_disconnect_class(event) && (session != s_state.session))
    {
        /* Safety-first stale disconnect: fail off BEFORE dropping, exactly
         * like the network adapter's stale-disconnect contract. */
        app_notify_safe(s_state.state, event, owner, session);
        s_state.stale_dropped++;
        osal_log_warning("[hq_app_state] stale %s (session %u != %u) dropped "
                         "after fail-off", app_event_name(event),
                         (unsigned)session, (unsigned)s_state.session);
        return HQ_APP_STATE_ERR_STALE;
    }

    /* External reset is legal from any state: back to boot, new episode. */
    if (event == HQ_APP_EVENT_RESET)
    {
        app_clear_retry();
        s_state.session++;
        s_state.wdt_armed   = true;
        s_state.deadline_ms = app_now();
        app_do_transition(HQ_APP_STATE_BOOT, event, HQ_APP_OWNER_EXTERNAL);
        return HQ_APP_STATE_OK;
    }

    /* Table-driven legal transition + owner validation. */
    const hq_app_state_transition_t *entry = app_lookup(s_state.state, event);
    if ((entry == NULL) || !app_owner_ok(entry, owner))
    {
        /* Illegal event for the current state, or wrong transition owner:
         * disconnect-class events still fail off before the drop. */
        if (hq_app_state_is_disconnect_class(event))
        {
            app_notify_safe(s_state.state, event, owner, session);
        }
        s_state.invalid_dropped++;
        osal_log_warning("[hq_app_state] illegal %s from %s owner %s dropped",
                         app_event_name(event), hq_app_state_name(s_state.state),
                         app_owner_name(owner));
        return HQ_APP_STATE_ERR_ILLEGAL;
    }

    if (entry->new_episode)
    {
        s_state.session++;
    }

    if (entry->to == HQ_APP_STATE_SAFE_OFF)
    {
        app_enter_safe_off(entry);
    }
    else
    {
        app_do_transition(entry->to, entry->event, entry->owner);
        if (entry->to == HQ_APP_STATE_ONLINE)
        {
            /* Arriving online resets the retry budget: a healed link may
             * consume a full fresh budget for the next outage. */
            app_clear_retry();
        }
    }

    return HQ_APP_STATE_OK;
}

hq_app_state_status_t hq_app_state_deliver(hq_app_state_event_t event,
                                     hq_app_transition_owner_t owner)
{
    hq_app_state_status_t status;

    if (!app_lock())
    {
        return HQ_APP_STATE_ERR_NOT_INITIALIZED;
    }
    if (!s_state.initialized)
    {
        app_unlock();
        return HQ_APP_STATE_ERR_NOT_INITIALIZED;
    }
    status = hq_app_state_deliver_locked(event, owner, s_state.session);
    app_unlock();
    return status;
}

hq_app_state_status_t hq_app_state_deliver_session(hq_app_state_event_t event,
                                             hq_app_transition_owner_t owner,
                                             uint32_t session)
{
    hq_app_state_status_t status;

    if (!app_lock())
    {
        return HQ_APP_STATE_ERR_NOT_INITIALIZED;
    }
    if (!s_state.initialized)
    {
        app_unlock();
        return HQ_APP_STATE_ERR_NOT_INITIALIZED;
    }
    status = hq_app_state_deliver_locked(event, owner, session);
    app_unlock();
    return status;
}

/* --------------------------------------------------------------------- */
/* Polling / watchdog / retry timer                                       */
/* --------------------------------------------------------------------- */

hq_app_state_status_t hq_app_state_poll_at(uint32_t now_ms)
{
    uint32_t elapsed;

    if (!app_lock())
    {
        return HQ_APP_STATE_ERR_NOT_INITIALIZED;
    }
    if (!s_state.initialized)
    {
        app_unlock();
        return HQ_APP_STATE_ERR_NOT_INITIALIZED;
    }

    /* 1. Watchdog check: the deadline was not refreshed within the
     *    timeout (no poll, no successful transition).  The detecting poll
     *    fails the machine, in order: output off (explicit, before the
     *    callback), expiry callback, disarm, clear the pending retry and
     *    transition to FATAL (whose non-online entry enforces fail-off
     *    again through the transition helper). */
    if (s_state.wdt_armed)
    {
        elapsed = (uint32_t)(now_ms - s_state.deadline_ms);
        if (elapsed >= s_state.cfg.watchdog_timeout_ms)
        {
            app_notify_safe(HQ_APP_STATE_FATAL, HQ_APP_EVENT_FATAL,
                            HQ_APP_OWNER_WATCHDOG, s_state.session);
            s_state.wdt_armed = false;
            app_clear_retry();
            if (s_state.cfg.on_watchdog_expired != NULL)
            {
                s_state.cfg.on_watchdog_expired();
            }
            if (s_state.state != HQ_APP_STATE_FATAL)
            {
                s_state.session++;
                app_do_transition(HQ_APP_STATE_FATAL, HQ_APP_EVENT_FATAL,
                                  HQ_APP_OWNER_WATCHDOG);
            }
            app_unlock();
            return HQ_APP_STATE_ERR_WATCHDOG;
        }
    }

    /* 2. Feed: the poll itself is the single kept-alive mechanism. */
    if (s_state.wdt_armed)
    {
        s_state.deadline_ms = now_ms;
    }

    /* 3. Retry schedule: consume one attempt and return to the failed
     *    stage when the backoff delay has elapsed. */
    if (s_state.state == HQ_APP_STATE_SAFE_OFF && s_state.retry_pending &&
        app_time_ge(now_ms, s_state.retry_due_ms))
    {
        s_state.retry_pending = false;
        s_state.retry_attempts++;
        s_state.session++;
        app_do_transition(s_state.retry_target, HQ_APP_EVENT_RETRY_DUE,
                          HQ_APP_OWNER_TIMER);
    }

    app_unlock();
    return HQ_APP_STATE_OK;
}

hq_app_state_status_t hq_app_state_poll(void)
{
    uint32_t now;

    /* app_now() dereferences the configured clock, which only exists
     * after init — check first, then read the clock. */
    if (!app_lock())
    {
        return HQ_APP_STATE_ERR_NOT_INITIALIZED;
    }
    if (!s_state.initialized)
    {
        app_unlock();
        return HQ_APP_STATE_ERR_NOT_INITIALIZED;
    }
    now = app_now();
    app_unlock();

    return hq_app_state_poll_at(now);
}

/* --------------------------------------------------------------------- */
/* State queries                                                          */
/* --------------------------------------------------------------------- */

hq_app_state_t hq_app_state_current(void)
{
    hq_app_state_t state;

    if (!app_lock())
    {
        return HQ_APP_STATE_BOOT;
    }
    state = s_state.initialized ? s_state.state : HQ_APP_STATE_BOOT;
    app_unlock();
    return state;
}

uint32_t hq_app_state_session(void)
{
    uint32_t session;

    if (!app_lock())
    {
        return 0U;
    }
    session = s_state.initialized ? s_state.session : 0U;
    app_unlock();
    return session;
}

hq_app_transition_owner_t hq_app_state_last_owner(void)
{
    hq_app_transition_owner_t owner;

    if (!app_lock())
    {
        return HQ_APP_OWNER_EXTERNAL;
    }
    owner = s_state.initialized ? s_state.last_owner : HQ_APP_OWNER_EXTERNAL;
    app_unlock();
    return owner;
}

bool hq_app_state_is_online(void)
{
    return hq_app_state_current() == HQ_APP_STATE_ONLINE;
}

bool hq_app_state_retry_pending(void)
{
    bool pending;

    if (!app_lock())
    {
        return false;
    }
    pending = s_state.initialized && s_state.retry_pending;
    app_unlock();
    return pending;
}

uint32_t hq_app_state_retry_delay_ms(void)
{
    uint32_t delay;

    if (!app_lock())
    {
        return 0U;
    }
    delay = s_state.initialized ? s_state.retry_delay_ms : 0U;
    app_unlock();
    return delay;
}

uint32_t hq_app_state_retry_attempts_used(void)
{
    uint32_t attempts;

    if (!app_lock())
    {
        return 0U;
    }
    attempts = s_state.initialized ? s_state.retry_attempts : 0U;
    app_unlock();
    return attempts;
}

bool hq_app_state_retry_exhausted(void)
{
    bool exhausted;

    if (!app_lock())
    {
        return false;
    }
    exhausted = s_state.initialized && s_state.retry_exhausted;
    app_unlock();
    return exhausted;
}

uint32_t hq_app_state_stale_dropped(void)
{
    uint32_t count;

    if (!app_lock())
    {
        return 0U;
    }
    count = s_state.initialized ? s_state.stale_dropped : 0U;
    app_unlock();
    return count;
}

uint32_t hq_app_state_invalid_dropped(void)
{
    uint32_t count;

    if (!app_lock())
    {
        return 0U;
    }
    count = s_state.initialized ? s_state.invalid_dropped : 0U;
    app_unlock();
    return count;
}