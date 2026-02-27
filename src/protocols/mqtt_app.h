/**
 *******************************************************************************
 * @file    ota.h
 * @author  Dmytro Shevchenko
 * @brief   MQTT Application layer
 *******************************************************************************
 */

/* Define to prevent recursive inclusion ------------------------------------*/

#ifndef _MQTT_APP_H
#define _MQTT_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Public functions ----------------------------------------------------------*/

/**
 * @brief   Init mqtt app task.
 */
void MqttApp_Init( void );

/**
 * @brief   Deinit mqtt app task.
 */
void MqttApp_Deinit( void );

/**
 * @brief   Post data to MQTT topic.
 * @param   [in] topic - MQTT topic to publish to.
 * @param   [in] message - Message content to publish.
 * @param   [in] qos - Quality of Service level (0, 1, or 2).
 * @return  true - if message queued successfully, otherwise false
 */
bool MqttApp_PostData( const char* topic, const char* message, int qos );

/**
 * @brief   Check if MQTT client is connected to server.
 * @return  true - if connected to MQTT server, otherwise false
 */
bool MqttApp_IsConnected(void);

typedef void (*mqtt_message_callback_t)(const char* topic, const char* message, size_t message_len);

/**
 * @brief   Subscribe to MQTT topic with callback and timeout.
 * @param   [in] topic - MQTT topic to subscribe to.
 * @param   [in] qos - Quality of Service level (0, 1, or 2).
 * @param   [in] callback - Function to call when message arrives.
 * @param   [in] timeout_ms - Timeout in milliseconds for subscription.
 * @return  true - if subscription successful, otherwise false
 */
bool MqttApp_Subscribe(const char* topic, int qos, mqtt_message_callback_t callback, uint32_t timeout_ms);

/**
 * @brief   Unsubscribe from MQTT topic with timeout.
 * @param   [in] topic - MQTT topic to unsubscribe from.
 * @param   [in] timeout_ms - Timeout in milliseconds for unsubscription.
 * @return  true - if unsubscription successful, otherwise false
 */
bool MqttApp_Unsubscribe(const char* topic, uint32_t timeout_ms);

#endif