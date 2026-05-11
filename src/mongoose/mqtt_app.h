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

void mqtt_app_init(void);
void mqtt_app_deinit(void);

bool mqtt_app_post_data(const char *topic, const char *message, int qos);
bool mqtt_app_is_connected(void);

bool mqtt_app_subscribe(const char *topic, int qos,
			mqtt_message_callback_t callback, uint32_t timeout_ms);
bool mqtt_app_unsubscribe(const char *topic, uint32_t timeout_ms);

#endif