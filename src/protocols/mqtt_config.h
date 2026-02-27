/**
 *******************************************************************************
 * @file    mqtt_config.h
 * @author  Dmytro Shevchenko
 * @brief   MQTT modules configuration header file
 *******************************************************************************
 */

/* Define to prevent recursive inclusion ------------------------------------*/

#ifndef _MQTT_CONFIG_H
#define _MQTT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* Public macros -------------------------------------------------------------*/

#define MQTT_CONFIG_STR_SIZE 64
#define MQTT_CERT_MAX_SIZE   5120

/* Public types --------------------------------------------------------------*/

typedef enum
{
  MQTT_CONFIG_VALUE_ADDRESS,
  MQTT_CONFIG_VALUE_SSL,
  MQTT_CONFIG_VALUE_TOPIC_PREFIX,
  MQTT_CONFIG_VALUE_POST_DATA_TOPIC,
  MQTT_CONFIG_VALUE_USERNAME,
  MQTT_CONFIG_VALUE_PASSWORD,
  MQTT_CONFIG_VALUE_CLIENT_ID,
  MQTT_CONFIG_VALUE_CERT,
  MQTT_CONFIG_VALUE_LAST
} mqtt_config_value_t;

typedef void ( *mqtt_apply_config_cb )( void );

/* Public functions ----------------------------------------------------------*/

/**
 * @brief   Init mqtt config.
 */
void MQTTConfig_Init( void );

bool MQTTConfig_SetInt( int value, mqtt_config_value_t config_value );
bool MQTTConfig_SetBool( bool value, mqtt_config_value_t config_value );
bool MQTTConfig_SetString( const char* string, mqtt_config_value_t config_value );
bool MQTTConfig_SetCert( const char* cert, size_t cert_len, size_t offset, mqtt_config_value_t config_value );
bool MQTTConfig_GetInt( int* value, mqtt_config_value_t config_value );
bool MQTTConfig_GetBool( bool* value, mqtt_config_value_t config_value );
const char* MQTTConfig_GetString( mqtt_config_value_t config_value );
const char* MQTTConfig_GetCert( mqtt_config_value_t config_value );
bool MQTTConfig_Save( void );
void MQTTConfig_SetCallback( mqtt_apply_config_cb cb );

#endif