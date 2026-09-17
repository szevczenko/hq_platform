/*
 * Wi-Fi provisioning automatic fallback controller - public API.
 *
 * Optional policy layer over wifi_http_provisioning. When compiled
 * (CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK=y) it starts the provisioning
 * application automatically so a device can be set up without a prior
 * network credential:
 *   - immediately during init when no saved Wi-Fi credential exists,
 *   - after the saved credential attempts are exhausted (CONNECT_FAILED),
 *   - never after a single transient disconnect (DISCONNECTED).
 *
 * Once a submitted credential connects (station IP acquisition) while the
 * portal is up, the controller keeps AP + HTTP and DNS available for a
 * configurable success grace period and then shuts the portal down and
 * requests a STA-only transition, so the fresh connection stays reachable
 * while the temporary access point is retired cleanly. If that connection
 * fails before the grace expires, the timer is cancelled and the portal stays
 * open for another credential attempt.
 *
 * The STA-only retirement is acknowledged asynchronously. After the listeners
 * are confirmed stopped the controller enters RETIRING_AP and only reports
 * ONLINE once WIFI_MGMT_EVENT_MODE_CHANGED confirms the requested STA-only
 * mode. A listener-stop failure or a mode-transition failure leaves the
 * controller in an explicit recoverable state instead of reporting ONLINE.
 *
 * All controller state is guarded by an internal OSAL mutex shared by the API,
 * the deferred worker and the timer callback paths. Policy work triggered by a
 * Wi-Fi event or the grace timer is never executed on the Wi-Fi worker (or
 * timer) thread: those callbacks only post a small work item to a queue owned
 * by the controller, and a dedicated controller-owned worker task applies the
 * items in FIFO order. Blocking calls (provisioning lifecycle, Wi-Fi mode
 * requests, OSAL timer control) are therefore always issued after the lock has
 * been released and always on the deferred worker (or the API caller) thread.
 * Every deferred item carries the session/generation token of the lifecycle in
 * which it was posted and is dropped - never applied - once that session has
 * been superseded, so a stale grace expiry or event from a cancelled or prior
 * window is a no-op.
 *
 * The controller subscribes once per lifetime and starts provisioning at most
 * once per qualifying fallback (guarded by a per-session flag). An explicit
 * wifi_provisioning_controller_stop() overrides the pending grace timer and
 * shuts the portal down immediately. Init and deinit are idempotent; deinit
 * unsubscribes every callback the controller registered, invalidates the
 * session, cancels/deletes the grace timer synchronously, drops any pending
 * deferred work and joins the deferred worker so no notification or lifecycle
 * callback can run after deinit returns.
 */

#ifndef WIFI_PROVISIONING_CONTROLLER_H
#define WIFI_PROVISIONING_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

/* Public types -------------------------------------------------------------*/

/**
 * @brief Controller policy states.
 *
 * @c DISABLED is reported before init, after deinit, and after an explicit
 * @c stop() completes. With saved credentials present the controller waits in
 * @c AWAITING_CONNECT for the manager to complete its auto-connect; a
 * successful connect moves it to @c ONLINE. @c PROVISIONING is entered when
 * the controller decided to start the provisioning application (no
 * credentials, or the saved credentials failed). @c GRACE is entered when the
 * station obtains an IP while the portal is active: the portal remains
 * available until the success grace period expires. Addressing the grace
 * transition and shutdown is explicit: @c RETIRING_AP is entered while the
 * listeners are shut down and the requested STA-only mode has been issued but
 * not yet confirmed; @c ONLINE is reached only after
 * @c WIFI_MGMT_EVENT_MODE_CHANGED confirms the STA-only transition.
 */
typedef enum
{
  WIFI_PROVISIONING_CONTROLLER_DISABLED         = 0,
  WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT = 1,
  WIFI_PROVISIONING_CONTROLLER_ONLINE           = 2,
  WIFI_PROVISIONING_CONTROLLER_PROVISIONING     = 3,
  WIFI_PROVISIONING_CONTROLLER_GRACE            = 4,
  WIFI_PROVISIONING_CONTROLLER_RETIRING_AP      = 5,
} wifi_provisioning_controller_state_t;

/**
 * @brief State-change notification callback (opt-in, NULL disables it).
 *
 * Invoked for every policy state transition the controller commits, so a
 * product with its own application state machine can mirror the full
 * controller lifecycle without polling
 * @c wifi_provisioning_controller_get_state().
 *
 * @param[in] previous State the controller is leaving.
 * @param[in] current  State the controller just entered.
 * @param[in] session  Session/generation token of the controller lifecycle in
 *                     which the transition was committed. It is incremented on
 *                     every init/deinit, so a notification captured in an
 *                     earlier lifecycle carries an older token; a product that
 *                     keeps the newest token it has seen can discard stale
 *                     notifications delivered from a prior controller
 *                     lifecycle.
 * @param[in] user_ctx Opaque pointer registered in
 *                     @c wifi_provisioning_controller_config_t.
 *
 * @par Execution context
 * Transitions driven by a Wi-Fi event or by the grace timer are delivered from
 * the controller's deferred worker task: the Wi-Fi event callback and the
 * grace timer callback only queue the transition, and the worker applies the
 * queued transitions in arrival order on a controller-owned thread, never on
 * the Wi-Fi worker or timer thread. Transitions initiated synchronously by
 * the public API - @c wifi_provisioning_controller_init_with_config()
 * (DISABLED -> AWAITING_CONNECT and the immediate fallback entry into
 * PROVISIONING), @c wifi_provisioning_controller_stop() (-> RETIRING_AP or
 * DISABLED) and @c wifi_provisioning_controller_deinit() (-> DISABLED) - are
 * delivered on the thread that called that API, which must itself honor the
 * rules below. In every case the callback runs on the thread that committed
 * the transition (always a controller-owned or API caller thread) and it must
 * therefore:
 *   - not block: no mutexes, semaphores, long loops, network or file I/O;
 *   - not call back into the controller
 *     (@c wifi_provisioning_controller_* APIs). The controller is
 *     mid-transition when the callback runs, so re-entering it can deadlock or
 *     corrupt the state machine. The read-only queries
 *     @c wifi_provisioning_controller_get_state() and
 *     @c wifi_provisioning_controller_is_provisioning() are the documented
 *     exception: they take only the short-lived controller lock (released
 *     before the callback is invoked) and never block; every other controller
 *     entry point (init, deinit, stop, set_success_grace_ms) must not be
 *     called from the callback;
 *   - not free or reuse @p user_ctx; its lifetime is the caller's
 *     responsibility and must cover every callback invocation.
 *
 * @par Payload guarantee
 * The payload is a pure state signal: two state enumerators, the session
 * token and @p user_ctx. No credential, SSID or other wireless content is
 * ever passed to this callback.
 */
typedef void (*wifi_provisioning_controller_state_cb_t)(
    wifi_provisioning_controller_state_t previous,
    wifi_provisioning_controller_state_t current,
    uint32_t session,
    void *user_ctx );

/**
 * @brief Init-time configuration for the automatic fallback controller.
 *
 * Passed to @c wifi_provisioning_controller_init_with_config(). Every field is
 * optional; a NULL configuration equals the default behavior of
 * @c wifi_provisioning_controller_init() (no notifications).
 */
typedef struct
{
  /** Optional state-change hook; NULL (default) disables notifications. See
   *  @c wifi_provisioning_controller_state_cb_t for the callback contract. */
  wifi_provisioning_controller_state_cb_t on_state_changed;

  /** Opaque user context passed back to @p on_state_changed. May be NULL. */
  void *user_ctx;
} wifi_provisioning_controller_config_t;

/* Public functions --------------------------------------------------------- */

/**
 * @brief Initialize the automatic fallback controller (idempotent).
 *
 * Equivalent to @c wifi_provisioning_controller_init_with_config(NULL):
 * the controller runs without a state-change notification callback.
 *
 * Subscribes to @c WIFI_MGMT_EVENT_CONNECTED, @c WIFI_MGMT_EVENT_DISCONNECTED,
 * @c WIFI_MGMT_EVENT_CONNECT_FAILED and @c WIFI_MGMT_EVENT_MODE_CHANGED. If no
 * saved credential exists the provisioning application is started immediately
 * and the controller enters @c PROVISIONING. A success-grace override applied
 * before init is preserved.
 *
 * @return true when initialized; false when the controller's deferred policy
 *         worker (queue + task) could not be created (a rare resource
 *         failure). Repeated calls while already initialized are safe no-ops
 *         that do not re-subscribe or restart the provisioning application.
 */
bool wifi_provisioning_controller_init( void );

/**
 * @brief Initialize the automatic fallback controller with an optional
 *        state-change notification hook (idempotent).
 *
 * Behaves exactly like @c wifi_provisioning_controller_init() and additionally
 * installs the opt-in notification callback carried by @p config, letting a
 * product observe the full controller lifecycle (DISABLED -> AWAITING_CONNECT,
 * fallback entry into PROVISIONING, PROVISIONING -> GRACE, GRACE ->
 * RETIRING_AP, RETIRING_AP -> ONLINE/DISABLED, the grace-abort path GRACE ->
 * PROVISIONING and the online-loss path ONLINE -> AWAITING_CONNECT) without
 * polling. See @c wifi_provisioning_controller_state_cb_t for the callback
 * contract and the re-entrancy rule.
 *
 * @param[in] config Optional configuration; may be NULL, in which case this
 *                   function behaves exactly like
 *                   @c wifi_provisioning_controller_init() (no notifications).
 *                   The structure is copied at init time; its storage need not
 *                   outlive the call.
 *
 * @return true when initialized; false when the controller's deferred policy
 *         worker (queue + task) could not be created (a rare resource
 *         failure). Repeated calls while already initialized are safe no-ops
 *         that do not re-subscribe, restart the provisioning application or
 *         replace an already installed callback. A fresh lifecycle started
 *         with @c wifi_provisioning_controller_deinit() followed by this
 *         function applies the new configuration.
 */
bool wifi_provisioning_controller_init_with_config(
    const wifi_provisioning_controller_config_t *config );

/**
 * @brief Deinitialize the automatic fallback controller (idempotent).
 *
 * Unsubscribes every typed Wi-Fi event callback the controller registered,
 * cancels and deletes the grace timer synchronously (waiting out any active
 * timer callback), invalidates the controller session, drops any pending
 * deferred work and joins the controller's deferred worker task, so no
 * notification or lifecycle callback can run after this function returns, and
 * returns to @c DISABLED. Calling while already deinitialized is a safe
 * no-op.
 */
void wifi_provisioning_controller_deinit( void );

/**
 * @brief Whether the controller has started (or is about to keep) the
 *        provisioning application running.
 * @return true when in @c PROVISIONING or @c GRACE.
 */
bool wifi_provisioning_controller_is_provisioning( void );

/**
 * @brief Explicitly stop automatic provisioning (idempotent).
 *
 * Cancels any pending success grace timer, stops the provisioning HTTP and
 * DNS listeners and requests a STA-only transition so the temporary access
 * point is retired. The controller transitions through @c RETIRING_AP and
 * reaches @c DISABLED only when the STA-only mode is confirmed.
 *
 * @return true when the provisioning application is (now or already) stopped
 *         and the retire was requested, false when the listener shutdown
 *         failed or the mode transition was rejected (the controller then
 *         stays recoverable without falsely reporting ONLINE).
 * @note  Calling while disabled is a safe no-op returning false.
 */
bool wifi_provisioning_controller_stop( void );

/**
 * @brief Override the success grace period used after station IP acquisition.
 *
 * The portal remains available for @p grace_ms after a successful connection
 * before the controller stops HTTP/DNS and requests STA-only mode. A value of
 * zero shuts the portal down immediately. The next grace period uses the new
 * value; a period already in progress is unaffected.
 *
 * @param[in] grace_ms Grace period duration in milliseconds.
 */
void wifi_provisioning_controller_set_success_grace_ms( uint32_t grace_ms );

/**
 * @brief Return the current controller policy state.
 * @return One of @c wifi_provisioning_controller_state_t.
 */
wifi_provisioning_controller_state_t wifi_provisioning_controller_get_state( void );

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
#include "wifi_managment.h"

/**
 * @brief Test-observability surface (test builds only).
 *
 * Lets a platform test prove that Wi-Fi event callbacks defer their policy
 * work to the controller-owned worker: nothing is applied while the worker is
 * held, and a burst of deferred actions is applied in the order it was
 * queued.
 */

/** One deferred work item the controller worker is about to apply. */
typedef struct
{
  bool              is_grace_expiry; /**< true: grace-timer expiry; false: mgmt event. */
  wifi_mgmt_event_t event;           /**< Raw event (valid when @p is_grace_expiry is false). */
  uint32_t          session;         /**< Lifecycle generation captured when the item was queued. */
} wifi_provisioning_controller_test_deferred_t;

/** Observer invoked by the deferred worker immediately before applying an item. */
typedef void ( *wifi_provisioning_controller_test_apply_cb_t )(
    const wifi_provisioning_controller_test_deferred_t *applied,
    void                                               *user_ctx );

/** Install/clear the apply observer (pass NULL to clear). */
void wifi_provisioning_controller_test_set_apply_hook(
    wifi_provisioning_controller_test_apply_cb_t cb, void *user_ctx );

/** Park the deferred worker so queued items are never applied while held.
 *  Passing false releases it; the worker then applies everything in FIFO
 *  order. */
void wifi_provisioning_controller_test_set_defer_hold( bool hold );

/** Queue a synthetic Wi-Fi management event through the exact same deferred
 *  path the real event callback uses (session snapshot + FIFO post). */
bool wifi_provisioning_controller_test_inject_event( wifi_mgmt_event_t event );

/** Wait until the deferred queue is empty and the worker is idle (nothing
 *  pending and nothing being applied). Returns false on timeout. */
bool wifi_provisioning_controller_test_wait_idle( uint32_t timeout_ms );

/** Fire the grace-timer expiry callback as if the real timer had expired. */
void wifi_provisioning_controller_test_fire_grace_expiry( void );
#endif

#endif    /* WIFI_PROVISIONING_CONTROLLER_H */