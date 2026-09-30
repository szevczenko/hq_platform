/**
 *******************************************************************************
 * @file    mqtt_config.h
 * @author  Dmytro Shevchenko
 * @brief   MQTT module configuration header file
 *******************************************************************************
 */

#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* Public macros -------------------------------------------------------------*/

#define MQTT_CONFIG_STR_SIZE		128
#define MQTT_CERT_MAX_SIZE		5120
#define MQTT_CONFIG_FILE_PATH		"mqtt.json"

/* Public types --------------------------------------------------------------*/

typedef enum {
	MQTT_CERT_SOURCE_NONE = 0,
	MQTT_CERT_SOURCE_FILE_PATH,
	MQTT_CERT_SOURCE_RAW,
	MQTT_CERT_SOURCE_LAST
} mqtt_cert_source_t;

typedef enum {
	MQTT_CONFIG_VALUE_ADDRESS,
	MQTT_CONFIG_VALUE_SSL,
	MQTT_CONFIG_VALUE_SKIP_VERIFY,
	MQTT_CONFIG_VALUE_TOPIC_PREFIX,
	MQTT_CONFIG_VALUE_POST_DATA_TOPIC,
	MQTT_CONFIG_VALUE_USERNAME,
	MQTT_CONFIG_VALUE_PASSWORD,
	MQTT_CONFIG_VALUE_CLIENT_ID,
	MQTT_CONFIG_VALUE_CERT,
	MQTT_CONFIG_VALUE_CLIENT_CERT,
	MQTT_CONFIG_VALUE_CLIENT_KEY,
	MQTT_CONFIG_VALUE_LAST
} mqtt_config_value_t;

typedef void (*mqtt_apply_config_cb)(void);

typedef struct {
	const char *address;
	const char *client_id;
	const char *username;
	const char *password;
	const char *ca_path;
	const char *client_cert_path;
	const char *client_key_path;
} mqtt_verified_config_t;

typedef enum {
	MQTT_VERIFIED_CONFIG_OK = 0,
	MQTT_VERIFIED_CONFIG_INVALID,
	MQTT_VERIFIED_CONFIG_CA_ERROR,
	MQTT_VERIFIED_CONFIG_CLIENT_CERT_ERROR,
	MQTT_VERIFIED_CONFIG_APPLY_ERROR
} mqtt_verified_config_status_t;

/* Public functions ----------------------------------------------------------*/

void mqtt_config_init(void);

bool mqtt_config_set_bool(bool value, mqtt_config_value_t key);
bool mqtt_config_set_string(const char *string, mqtt_config_value_t key);
bool mqtt_config_set_cert_source(mqtt_cert_source_t source, const char *value,
				 mqtt_config_value_t key);
bool mqtt_config_get_bool(bool *value, mqtt_config_value_t key);
const char *mqtt_config_get_string(mqtt_config_value_t key);
const char *mqtt_config_get_cert(mqtt_config_value_t key);
bool mqtt_config_get_cert_source(mqtt_cert_source_t *source, const char **value,
				 mqtt_config_value_t key);
bool mqtt_config_save(void);
void mqtt_config_set_callback(mqtt_apply_config_cb cb);

/* Apply and retain a verified-TLS transport snapshot. Credentials may be
 * omitted to preserve the current runtime token/provider values. */
mqtt_verified_config_status_t mqtt_config_apply_verified(
	const mqtt_verified_config_t *verified);

/* Close the secure apply gate and unregister its reconnect validator. */
void mqtt_config_invalidate_verified(void);

/* Check the live mutable config against the applied transport snapshot.
 * Username/password are excluded by design: token rotation must not
 * invalidate a verified TLS transport. */
bool mqtt_config_verified_is_current(void);

/* Copy the server URL only while the verified snapshot is still current. */
bool mqtt_config_get_verified_server_url(char *out, size_t out_size);

#endif