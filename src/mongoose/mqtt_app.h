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

typedef void ( *mqtt_message_callback_t )( const char* topic, const char* message, size_t message_len );

void MqttApp_Init( void );
void MqttApp_Deinit( void );

bool MqttApp_PostData( const char* topic, const char* message, int qos );
bool MqttApp_IsConnected( void );

bool MqttApp_Subscribe( const char* topic, int qos, mqtt_message_callback_t callback, uint32_t timeout_ms );
bool MqttApp_Unsubscribe( const char* topic, uint32_t timeout_ms );

#endif