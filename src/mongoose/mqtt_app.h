/**
 *******************************************************************************
 * @file    mqtt_app.h
 * @author  Dmytro Shevchenko
 * @brief   MQTT application layer
 *******************************************************************************
 */

#ifndef MQTT_APP_H
#define MQTT_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mqtt_config.h"

typedef void (*mqtt_message_callback_t)(const char *topic, const char *message,
					size_t message_len);

typedef enum {
	MQTT_DISCONNECT_REASON_REMOTE_CLOSE = 0,
	MQTT_DISCONNECT_REASON_ERROR,
	MQTT_DISCONNECT_REASON_EXPLICIT
} mqtt_disconnect_reason_t;

typedef enum {
	MQTT_CONNECT_FAILURE_REASON_CONNECT_CREATE_FAILED = 0,
	MQTT_CONNECT_FAILURE_REASON_CONNACK_REJECTED,
	MQTT_CONNECT_FAILURE_REASON_TRANSPORT_ERROR,
	MQTT_CONNECT_FAILURE_REASON_CONFIG_REJECTED
} mqtt_connect_failure_reason_t;

typedef void (*mqtt_connect_callback_t)(void);
typedef void (*mqtt_disconnect_callback_t)(mqtt_disconnect_reason_t reason);
typedef void (*mqtt_connect_failure_callback_t)(
	mqtt_connect_failure_reason_t reason);

/**
 * @brief Snapshot of the transport configuration the apply-config handler
 * would use for its reconnect.
 *
 * The snapshot is captured immediately before the reconnect decision and is
 * passed to the configuration-owner validation callback (see
 * mqtt_app_set_config_validation_callback()).  It contains owned copies of
 * the exact values the reconnect would use: the broker address, the SSL
 * flag, skip-verify, and the certificate sources/values (a file path for
 * MQTT_CERT_SOURCE_FILE_PATH or the raw PEM text for MQTT_CERT_SOURCE_RAW).
 *
 * The platform guarantees that a reconnect approved through this gate uses
 * these validated values and never rereads the mutable mqtt_config state, so
 * a concurrent setter cannot swap in unvalidated values between approval and
 * the transport actually using them.  The pointer fields are owned by the
 * platform and remain valid only for the duration of the callback.
 */
typedef struct {
	const char *address;
	bool ssl_enabled;
	bool skip_verify;
	mqtt_cert_source_t cert_source;
	const char *cert_value;
	mqtt_cert_source_t client_cert_source;
	const char *client_cert_value;
	mqtt_cert_source_t client_key_source;
	const char *client_key_value;
} mqtt_config_snapshot_t;

/**
 * @brief Configuration-owner validation callback.
 *
 * Returns true to allow the apply-config handler to reconnect with the
 * candidate snapshot, or false to reject it.  The platform is policy-neutral:
 * it never interprets the fields itself, it only provides the gate and the
 * failure observer.
 */
typedef bool (*mqtt_config_validation_callback_t)(
	const mqtt_config_snapshot_t *candidate);

typedef struct {
	uint16_t keepalive_sec;
	uint32_t reconnect_initial_delay_ms;
	uint32_t reconnect_max_delay_ms;
	bool reconnect_exponential_backoff;
} mqtt_connection_policy_t;

void mqtt_app_init(void);
void mqtt_app_deinit(void);

bool mqtt_app_post_data(const char *topic, const char *message, int qos);
bool mqtt_app_is_connected(void);

bool mqtt_app_subscribe(const char *topic, int qos,
			mqtt_message_callback_t callback, uint32_t timeout_ms);
bool mqtt_app_unsubscribe(const char *topic, uint32_t timeout_ms);

/**
 * @brief Register the product/client MQTT lifecycle callbacks.
 *
 * These callbacks are owned by the MQTT consumer (for example ThingsBoard)
 * and may be replaced at any time.  They are invoked on the Mongoose poll
 * thread and are always invoked IN ADDITION to the platform safety observer
 * (see mqtt_app_set_safety_callbacks()).
 */
void mqtt_app_set_connect_callback(mqtt_connect_callback_t cb);
void mqtt_app_set_disconnect_callback(mqtt_disconnect_callback_t cb);
void mqtt_app_set_connect_failure_callback(mqtt_connect_failure_callback_t cb);

/**
 * @brief Register the platform safety observer for MQTT lifecycle events.
 *
 * The three callbacks are fan-out observers: each one is invoked IN ADDITION
 * to (never instead of) the product/client callbacks above.  A later
 * mqtt_app_set_connect_callback() / set_disconnect_callback() /
 * set_connect_failure_callback() call can only replace the client slot, so
 * ThingsBoard (or any other MQTT consumer) can neither overwrite nor
 * silently inherit the safety observer slot.
 *
 * Registration is idempotent (a repeated call simply replaces the whole
 * safety set) and thread-safe, and is legal before mqtt_app_init() and while
 * the transport is stopped.  All three callbacks run on the Mongoose poll
 * thread and must not block.
 *
 * Only the platform safety owner (the designated configuration owner, e.g.
 * the verified-TLS product component) should call this API.
 */
void mqtt_app_set_safety_callbacks(mqtt_connect_callback_t connect_cb,
				   mqtt_disconnect_callback_t disconnect_cb,
				   mqtt_connect_failure_callback_t failure_cb);

/**
 * @brief Register the configuration-owner validation gate.
 *
 * Lets a designated configuration owner (for example the verified-TLS product
 * component) veto the generic apply-config reconnect path
 * (mqtt_config_save() -> MQTT_CMD_TYPE_APPLY_CONFIG).  The callback is
 * invoked on the Mongoose poll thread, from the apply-config handler, before
 * the transport is disconnected/reconnected, with a snapshot of the exact
 * values the reconnect would use (see mqtt_config_snapshot_t).
 *
 * The gate is fail-closed: the apply-config handler reconnects only when a
 * validation callback is registered AND returns true.  Returning false (or
 * having no callback registered) rejects the update: the handler skips the
 * reconnect, notifies the failure observer with
 * MQTT_CONNECT_FAILURE_REASON_CONFIG_REJECTED and leaves the transport
 * disconnected.  A rejected update never reconnects with the mutated values.
 *
 * The platform is policy-neutral: it only provides the gate and the observer
 * hooks; the verified-TLS policy itself belongs to the registering owner.
 * Registration is idempotent and thread-safe, and may be performed before
 * mqtt_app_init().
 */
void mqtt_app_set_config_validation_callback(
	mqtt_config_validation_callback_t cb);

/* Must be called before mqtt_app_init() or while transport is fully stopped. */
void mqtt_app_set_connection_policy(const mqtt_connection_policy_t *policy);

#endif