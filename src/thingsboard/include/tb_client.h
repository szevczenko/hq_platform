/**
 *******************************************************************************
 * @file    tb_client.h
 * @brief   ThingsBoard client – core connection management
 *******************************************************************************
 */

#ifndef TB_CLIENT_H
#define TB_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum concurrent pending requests (attribute requests, client RPCs) */
#define TB_MAX_PENDING_REQUESTS 8
#define TB_CLIENT_CONFIG_STR_SIZE 128

typedef struct tb_client tb_client_t;

typedef enum {
	TB_CLIENT_DISCONNECT_REASON_REMOTE_CLOSE = 0,
	TB_CLIENT_DISCONNECT_REASON_ERROR,
	TB_CLIENT_DISCONNECT_REASON_EXPLICIT
} tb_client_disconnect_reason_t;

typedef enum {
	TB_CLIENT_CONNECT_FAILURE_REASON_CONNECT_CREATE_FAILED = 0,
	TB_CLIENT_CONNECT_FAILURE_REASON_CONNACK_REJECTED,
	TB_CLIENT_CONNECT_FAILURE_REASON_TRANSPORT_ERROR
} tb_client_connect_failure_reason_t;

typedef enum {
    TB_REQUEST_RESULT_SUCCESS = 0,
    TB_REQUEST_RESULT_TIMEOUT,
    TB_REQUEST_RESULT_CANCELLED,
    TB_REQUEST_RESULT_ERROR
} tb_request_result_t;

typedef void (*tb_client_connect_callback_t)(tb_client_t *client,
					     void *user_data);
typedef void (*tb_client_disconnect_callback_t)(
	tb_client_t *client, tb_client_disconnect_reason_t reason,
	void *user_data);
typedef void (*tb_client_connect_failure_callback_t)(
	tb_client_t *client, tb_client_connect_failure_reason_t reason,
	void *user_data);

typedef struct {
    char server_url[TB_CLIENT_CONFIG_STR_SIZE];    /**< ThingsBoard MQTT URL e.g. "mqtt://host:1883" */
    char access_token[TB_CLIENT_CONFIG_STR_SIZE];  /**< Device access token (used as MQTT username) */
    char client_id[TB_CLIENT_CONFIG_STR_SIZE];     /**< MQTT client ID (empty = use access_token) */
    char device_name[TB_CLIENT_CONFIG_STR_SIZE];   /**< Device name for logging */
    /*
     * Connection-state callbacks run in MQTT event-loop context and must not
     * block. They are invoked without MQTT transport internal locks held.
     */
    tb_client_connect_callback_t on_connect;
    tb_client_disconnect_callback_t on_disconnect;
    tb_client_connect_failure_callback_t on_connect_failure;
    void *connection_user_data;

    /* Optional QoS defaults. Set use_custom_qos_defaults=true to apply. */
    bool use_custom_qos_defaults;
    uint8_t default_publish_qos;   /* 0 or 1 */
    uint8_t default_subscribe_qos; /* 0 or 1 */

    /* Optional MQTT transport policy. Zero values use bounded defaults. */
    uint16_t keepalive_sec;
    uint32_t reconnect_initial_delay_ms;
    uint32_t reconnect_max_delay_ms;
    bool reconnect_exponential_backoff;

    /* Optional ThingsBoard session-limits enforcement (P1 reliability feature). */
    bool enable_session_limits;
    uint16_t defer_queue_capacity; /* 0 = default */
} tb_client_config_t;

/**
 * @brief Initialize the ThingsBoard client
 * @param[out] client  Pointer to receive allocated client handle
 * @param[in]  config  Client configuration
 * @return 0 on success, negative on error
 */
int tb_client_init(tb_client_t **client, const tb_client_config_t *config);

/**
 * @brief Deinitialize and free the ThingsBoard client
 * @param client  Client handle
 */
void tb_client_deinit(tb_client_t *client);

/**
 * @brief Connect to ThingsBoard server
 * @param client  Client handle
 * @return 0 on success, negative on error
 */
int tb_client_connect(tb_client_t *client);

/**
 * @brief Disconnect from ThingsBoard server
 * @param client  Client handle
 */
void tb_client_disconnect(tb_client_t *client);

/**
 * @brief Check if client is connected
 * @param client  Client handle
 * @return true if connected
 */
bool tb_client_is_connected(tb_client_t *client);

/**
 * @brief Get the internal request ID counter (incremented per use)
 * @param client  Client handle
 * @return Next request ID
 */
uint32_t tb_client_get_next_request_id(tb_client_t *client);

/**
 * @brief Publish raw JSON string to a topic
 * @param client  Client handle
 * @param topic   MQTT topic
 * @param json    JSON payload (null-terminated)
 * @return 0 on success, negative on error
 */
int tb_client_publish(tb_client_t *client, const char *topic, const char *json);

/**
 * @brief Publish raw JSON string to a topic with explicit QoS
 * @param client  Client handle
 * @param topic   MQTT topic
 * @param json    JSON payload (null-terminated)
 * @param qos     MQTT QoS level (0 or 1)
 * @return 0 on success, negative on error
 */
int tb_client_publish_with_qos(tb_client_t *client, const char *topic,
                               const char *json, int qos);

/**
 * @brief Subscribe to a topic with message callback
 * @param client      Client handle
 * @param topic       MQTT topic
 * @param callback    Message callback
 * @param timeout_ms  Subscription timeout
 * @return 0 on success, negative on error
 */
int tb_client_subscribe(tb_client_t *client, const char *topic,
                        void (*callback)(const char *topic, const char *payload,
                                         size_t payload_len),
                        uint32_t timeout_ms);

/**
 * @brief Subscribe to a topic with explicit QoS
 * @param client      Client handle
 * @param topic       MQTT topic
 * @param qos         MQTT QoS level (0 or 1)
 * @param callback    Message callback
 * @param timeout_ms  Subscription timeout
 * @return 0 on success, negative on error
 */
int tb_client_subscribe_with_qos(tb_client_t *client, const char *topic,
                                 int qos,
                                 void (*callback)(const char *topic,
                                                  const char *payload,
                                                  size_t payload_len),
                                 uint32_t timeout_ms);

/**
 * @brief Unsubscribe from a topic
 * @param client      Client handle
 * @param topic       MQTT topic
 * @param timeout_ms  Unsubscription timeout
 * @return 0 on success, negative on error
 */
int tb_client_unsubscribe(tb_client_t *client, const char *topic,
                          uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* TB_CLIENT_H */
