/**
 *******************************************************************************
 * @file    mqtt_app_mock.h
 * @brief   Mock of mqtt_app for ThingsBoard unit tests
 *******************************************************************************
 */

#ifndef MQTT_APP_MOCK_H
#define MQTT_APP_MOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Track mock state for assertions */
#define MOCK_MAX_PUBLISHES 32
#define MOCK_MAX_TOPIC_LEN 256
#define MOCK_MAX_MSG_LEN   1024

typedef struct {
    char topic[MOCK_MAX_TOPIC_LEN];
    char message[MOCK_MAX_MSG_LEN];
    int  qos;
} mock_publish_record_t;

typedef struct {
    char topic[MOCK_MAX_TOPIC_LEN];
} mock_subscribe_record_t;

/* Globals for test inspection */
extern mock_publish_record_t mock_publishes[];
extern int mock_publish_count;
extern mock_subscribe_record_t mock_subscribes[];
extern int mock_subscribe_count;
extern bool mock_connected;

/* Reset all mock state */
void mqtt_app_mock_reset(void);

/* Simulate receiving a message on a subscribed topic */
void mqtt_app_mock_deliver_message(const char *topic, const char *payload,
                                   size_t payload_len);

#endif /* MQTT_APP_MOCK_H */
