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
#define MQTT_CERT_FILE_PATH		"mqtt_cert.pem"
#define MQTT_CLIENT_CERT_FILE_PATH	"mqtt_client_cert.pem"
#define MQTT_CLIENT_KEY_FILE_PATH	"mqtt_client_key.pem"

/* Public types --------------------------------------------------------------*/

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

/* Public functions ----------------------------------------------------------*/

void mqtt_config_init(void);

bool mqtt_config_set_int(int value, mqtt_config_value_t key);
bool mqtt_config_set_bool(bool value, mqtt_config_value_t key);
bool mqtt_config_set_string(const char *string, mqtt_config_value_t key);
bool mqtt_config_set_cert(const char *cert, size_t cert_len, size_t offset,
			  mqtt_config_value_t key);
bool mqtt_config_get_int(int *value, mqtt_config_value_t key);
bool mqtt_config_get_bool(bool *value, mqtt_config_value_t key);
const char *mqtt_config_get_string(mqtt_config_value_t key);
const char *mqtt_config_get_cert(mqtt_config_value_t key);
bool mqtt_config_save(void);
void mqtt_config_set_callback(mqtt_apply_config_cb cb);

#endif