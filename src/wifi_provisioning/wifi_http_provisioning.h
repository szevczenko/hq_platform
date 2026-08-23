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

/* Public functions --------------------------------------------------------- */

/**
 * @brief Start the provisioning application (idempotent).
 *
 * Requires the shared Mongoose process and the Wi-Fi management module to be
 * initialized, then requests AP+STA mode, opens the captive DNS listener and
 * the HTTP listener on the Mongoose poll thread, and transitions to
 * @c RUNNING only after both listeners are bound. If any step fails, partial
 * listeners are rolled back and the application enters @c ERROR.
 *
 * @return true when the application is (or already was) running, false when a
 *         dependency is missing, no listen URL applies, or a bind fails.
 * @note   Repeated calls while running are safe no-ops.
 */
bool wifi_http_provisioning_start( void );

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