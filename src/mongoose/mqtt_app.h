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
	MQTT_CONNECT_FAILURE_REASON_TRANSPORT_ERROR
} mqtt_connect_failure_reason_t;

typedef void (*mqtt_connect_callback_t)(void);
typedef void (*mqtt_disconnect_callback_t)(mqtt_disconnect_reason_t reason);
typedef void (*mqtt_connect_failure_callback_t)(
	mqtt_connect_failure_reason_t reason);

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

void mqtt_app_set_connect_callback(mqtt_connect_callback_t cb);
void mqtt_app_set_disconnect_callback(mqtt_disconnect_callback_t cb);
void mqtt_app_set_connect_failure_callback(mqtt_connect_failure_callback_t cb);
void mqtt_app_set_connection_policy(const mqtt_connection_policy_t *policy);

#endif