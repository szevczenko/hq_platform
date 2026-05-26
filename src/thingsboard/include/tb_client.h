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

typedef struct tb_client tb_client_t;

typedef struct {
    const char *server_url;      /**< ThingsBoard MQTT URL e.g. "mqtt://host:1883" */
    const char *access_token;    /**< Device access token (used as MQTT username) */
    const char *client_id;       /**< MQTT client ID (NULL = use access_token) */
    const char *device_name;     /**< Device name for logging */
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
