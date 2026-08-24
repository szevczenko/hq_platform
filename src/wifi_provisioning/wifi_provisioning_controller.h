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
 * the Wi-Fi callback and the timer callback paths; blocking calls (provisioning
 * lifecycle, Wi-Fi mode requests, OSAL timer control) are always issued after
 * the lock has been released. A session/generation token makes a stale grace
 * expiry from a cancelled or prior grace window a no-op.
 *
 * The controller subscribes once per lifetime and starts provisioning at most
 * once per qualifying fallback (guarded by a per-session flag). An explicit
 * wifi_provisioning_controller_stop() overrides the pending grace timer and
 * shuts the portal down immediately. Init and deinit are idempotent; deinit
 * unsubscribes every callback the controller registered, cancels/deletes the
 * grace timer synchronously and waits out any active expiry callback before
 * clearing controller state.
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

/* Public functions --------------------------------------------------------- */

/**
 * @brief Initialize the automatic fallback controller (idempotent).
 *
 * Subscribes to @c WIFI_MGMT_EVENT_CONNECTED, @c WIFI_MGMT_EVENT_DISCONNECTED,
 * @c WIFI_MGMT_EVENT_CONNECT_FAILED and @c WIFI_MGMT_EVENT_MODE_CHANGED. If no
 * saved credential exists the provisioning application is started immediately
 * and the controller enters @c PROVISIONING. A success-grace override applied
 * before init is preserved.
 *
 * @return true always; repeated calls while already initialized are safe
 *         no-ops that do not re-subscribe or restart the provisioning
 *         application.
 */
bool wifi_provisioning_controller_init( void );

/**
 * @brief Deinitialize the automatic fallback controller (idempotent).
 *
 * Unsubscribes every typed Wi-Fi event callback the controller registered,
 * cancels and deletes the grace timer synchronously (waiting out any active
 * timer callback), invalidates the controller session and returns to
 * @c DISABLED. Calling while already deinitialized is a safe no-op.
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
void wifi_provisioning_controller_test_fire_grace_expiry( void );
#endif

#endif    /* WIFI_PROVISIONING_CONTROLLER_H */