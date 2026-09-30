/*
 * Wi-Fi HTTP provisioning application - public lifecycle API.
 *
 * Defines the public provisioning application controller. The controller
 * owns exactly two network services while running:
 *   - the provisioning HTTP listener (bound on the shared Mongoose thread),
 *   - the captive DNS responder (delegated to captive_dns_server).
 *
 * The API is deliberately small: idempotent start/stop and state query. The
 * product owns the Mongoose process lifetime (MongooseProcess_Init/Deinit);
 * this module only requires it to be running and must not deinitialize it.
 * The product may provide runtime listen URLs for the two listeners so that
 * tests and POSIX simulations can bind non-privileged high ports.
 */

#ifndef WIFI_HTTP_PROVISIONING_H
#define WIFI_HTTP_PROVISIONING_H

#include <stdbool.h>

/* Public types --------------------------------------------------------------*/

/**
 * @brief Provisioning application lifecycle states.
 *
 * The application transitions through @c STARTING while listeners are being
 * opened and reaches @c RUNNING only after both the HTTP and the DNS listener
 * are bound. A failed start rolls back partial listeners and leaves the
 * application in @c ERROR (or @c STOPPED). Stop progresses through @c STOPPING
 * and always returns to @c STOPPED.
 */
typedef enum {
  WIFI_PROVISIONING_STOPPED  = 0,
  WIFI_PROVISIONING_STARTING = 1,
  WIFI_PROVISIONING_RUNNING  = 2,
  WIFI_PROVISIONING_STOPPING = 3,
  WIFI_PROVISIONING_ERROR    = 4,
} wifi_http_provisioning_state_t;

/**
 * @brief Result of one provisioning start attempt.
 *
 * A successful start is not just "state == RUNNING": the portal is only
 * truthful when the radio actually reached AP+STA mode AND both listeners
 * (HTTP + DNS) are bound. This enum lets callers distinguish every documented
 * failure mode of @ref wifi_http_provisioning_start_ex so a silent
 * "started without an AP" can never be mistaken for a reachable portal.
 *
 * Values are stable error codes; they may appear in log signatures and must
 * never be derived from (or formatted with) SSID, password, token, or
 * URL-with-credential content.
 */
typedef enum {
  /** Fresh start completed: AP+STA reached, both listeners bound. */
  WIFI_HTTP_PROVISIONING_START_OK = 0,
  /** start() ran while RUNNING/STARTING: idempotent no-op, still up. */
  WIFI_HTTP_PROVISIONING_START_ALREADY_RUNNING,
  /** A required dependency (shared Mongoose process, Wi-Fi mgmt) is down. */
  WIFI_HTTP_PROVISIONING_START_ERR_DEPENDENCY,
  /** The AP+STA mode request was refused by the Wi-Fi management layer. */
  WIFI_HTTP_PROVISIONING_START_ERR_MODE_TRANSITION,
  /** The provisioning HTTP listener bind was refused. */
  WIFI_HTTP_PROVISIONING_START_ERR_HTTP_BIND,
  /** The captive DNS listener bind was refused. */
  WIFI_HTTP_PROVISIONING_START_ERR_DNS_BIND,
  /** Listeners bound but the radio never actually reached AP+STA mode. */
  WIFI_HTTP_PROVISIONING_START_ERR_NO_AP,
} wifi_http_provisioning_start_status_t;

/* Public functions --------------------------------------------------------- */

/**
 * @brief Start the provisioning application (idempotent) with full result.
 *
 * Requires the shared Mongoose process and the Wi-Fi management module to be
 * initialized, then requests AP+STA mode, opens the captive DNS listener and
 * the HTTP listener on the Mongoose poll thread, and transitions to
 * @c RUNNING only after both listeners are bound. Afterwards it verifies that
 * the radio actually reached AP+STA mode (via the Wi-Fi management mode/query
 * surface) and reports @c WIFI_HTTP_PROVISIONING_START_ERR_NO_AP when the
 * listeners are up but the radio never did. If any step fails, partial
 * listeners are rolled back and the application enters @c ERROR.
 *
 * @return @c WIFI_HTTP_PROVISIONING_START_OK on a fresh successful start,
 *         @c WIFI_HTTP_PROVISIONING_START_ALREADY_RUNNING when called while
 *         already running/starting (a safe no-op), or a distinct
 *         @c WIFI_HTTP_PROVISIONING_START_ERR_* value describing the failure.
 * @note   Repeated calls while running are safe no-ops.
 */
wifi_http_provisioning_start_status_t wifi_http_provisioning_start_ex( void );

/**
 * @brief Start the provisioning application (idempotent).
 *
 * Thin bool wrapper over @ref wifi_http_provisioning_start_ex: succeeds when
 * the portal is fully up (or already was). Use the enum API when the caller
 * needs to distinguish the documented failure modes.
 *
 * @return true when the application is (or already was) running with the
 *         radio in AP+STA, false when a dependency is missing, a listener
 *         bind fails, or the radio never reached AP+STA.
 * @note   Repeated calls while running are safe no-ops.
 */
bool wifi_http_provisioning_start( void );

/**
 * @brief Return the result of the most recent start attempt.
 *
 * Callers that use the bool wrapper can still observe which failure mode
 * occurred. The value is updated at the end of every
 * @ref wifi_http_provisioning_start_ex() call (or a concurrent start).
 *
 * @return One of @c wifi_http_provisioning_start_status_t.
 */
wifi_http_provisioning_start_status_t wifi_http_provisioning_get_last_start_status( void );

/**
 * @brief Query whether the provisioning portal is fully reachable.
 *
 * A portal is reachable only when BOTH owned listeners (HTTP + DNS) are bound
 * AND the radio is actually serving AP+STA mode. Products and the controller
 * must use this instead of inferring reachability from
 * @ref wifi_http_provisioning_get_state alone, because @c RUNNING does not
 * prove the radio side reached AP+STA.
 *
 * @return true when the portal is fully up (AP + both listeners), false
 *         otherwise (stopped, error, starting, or running without AP).
 */
bool wifi_http_provisioning_is_reachable( void );

/**
 * @brief Stop the provisioning application (idempotent, single-owner).
 *
 * Closes only the HTTP and DNS listeners owned by the provisioning
 * application and clears temporary runtime state. Stop ordering is preserved:
 * Wi-Fi callbacks are unsubscribed first, then the HTTP and DNS listeners are
 * closed on the shared Mongoose poll thread, their closures are confirmed to
 * have completed, and only then is @c WIFI_PROVISIONING_STOPPED reported. The
 * shared Mongoose process (and any other listeners, such as MQTT) are left
 * untouched.
 *
 * start() and stop() are serialized by a dedicated lifecycle mutex, so
 * concurrent calls are single-owner. A caller arriving during
 * @c WIFI_PROVISIONING_STOPPING waits for (joins) the in-flight stop and
 * returns the same completed result.
 *
 * @return true when the listeners are confirmed closed and the application is
 *         stopped (now or already), false when an owned listener could not be
 *         closed while the shared Mongoose process is still running (the state
 *         is then @c WIFI_PROVISIONING_ERROR).
 */
bool wifi_http_provisioning_stop( void );

/**
 * @brief Return the current provisioning lifecycle state.
 * @return One of @c wifi_http_provisioning_state_t.
 */
wifi_http_provisioning_state_t wifi_http_provisioning_get_state( void );

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
void wifi_http_provisioning_test_set_stop_boundary_hook( void ( *hook )( void ) );

/**
 * @brief Test-only override of the AP+STA mode request outcome.
 *
 * When a hook is installed, the next start() uses its result instead of
 * calling @c wifi_mgmt_request_mode(), so a refused mode transition can be
 * reproduced deterministically against a real Wi-Fi management stack.
 * Install NULL to restore the real request path.
 */
void wifi_http_provisioning_test_set_mode_request_hook( bool ( *hook )( void ) );

/**
 * @brief Test-only override of the post-start radio verification.
 *
 * When a hook is installed, the next start() asks the hook (instead of
 * polling @c wifi_mgmt_get_mode()) whether the radio reached AP+STA. A hook
 * returning false makes start() fail with
 * @c WIFI_HTTP_PROVISIONING_START_ERR_NO_AP and roll the listeners back, so
 * the "started without an AP" path is testable against a real HAL that cannot
 * be told to fail a mode transition. Install NULL to restore verification
 * through the real mode/query surface.
 */
void wifi_http_provisioning_test_set_radio_verify_hook( bool ( *hook )( void ) );
#endif

/**
 * @brief Override the HTTP listen URL used by the next start().
 *
 * The URL must be a Mongoose HTTP listen URL (for example
 * "http://0.0.0.0:8080"). It is stored and applied the next time the
 * application is started. Changing the URL while running is not allowed.
 *
 * @param[in] url Non-NULL, non-empty HTTP URL.
 * @return true when accepted, false when NULL/empty/too long or when the
 *         application is already running/starting.
 */
bool wifi_http_provisioning_set_http_url( const char* url );

/**
 * @brief Override the captive DNS listen URL used by the next start().
 *
 * The URL must be a Mongoose UDP listen URL (for example
 * "udp://0.0.0.0:10053"). It is stored and applied the next time the
 * application is started. Changing the URL while running is not allowed.
 *
 * @param[in] url Non-NULL, non-empty UDP URL.
 * @return true when accepted, false when NULL/empty/too long or when the
 *         application is already running/starting.
 */
bool wifi_http_provisioning_set_dns_url( const char* url );

#endif    /* WIFI_HTTP_PROVISIONING_H */