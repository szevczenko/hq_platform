/**
 *******************************************************************************
 * @file    tb_attributes.h
 * @brief   ThingsBoard client – attributes API
 *******************************************************************************
 */

#ifndef TB_ATTRIBUTES_H
#define TB_ATTRIBUTES_H

#include "tb_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Callback for attribute request response */
typedef void (*tb_attribute_response_cb_t)(const char *json_response,
                                           void *user_data);

/** Callback for shared attribute updates */
typedef void (*tb_shared_attribute_cb_t)(const char *json_payload,
                                         void *user_data);

/*
 * Note: Stateful APIs in this module (attribute requests and shared-attribute
 * subscribe/unsubscribe) are singleton-scoped and must use one tb_client_t
 * instance for the process lifetime.
 */

/**
 * @brief Send client attribute (integer)
 */
int tb_attributes_send_int(tb_client_t *client, const char *key, int64_t value);

/**
 * @brief Send client attribute (double)
 */
int tb_attributes_send_double(tb_client_t *client, const char *key, double value);

/**
 * @brief Send client attribute (boolean)
 */
int tb_attributes_send_bool(tb_client_t *client, const char *key, bool value);

/**
 * @brief Send client attribute (string)
 */
int tb_attributes_send_string(tb_client_t *client, const char *key, const char *value);

/**
 * @brief Send raw JSON attributes
 */
int tb_attributes_send_json(tb_client_t *client, const char *json);

/**
 * @brief Request client-side attribute values from the server
 * @param client      Client handle
 * @param keys        Array of key names to request
 * @param num_keys    Number of keys
 * @param cb          Response callback
 * @param user_data   User data passed to callback
 * @param timeout_ms  Request timeout in ms
 * @return 0 on success, negative on error
 */
int tb_attributes_request_client(tb_client_t *client, const char *keys[],
                                 size_t num_keys, tb_attribute_response_cb_t cb,
                                 void *user_data, uint32_t timeout_ms);

/**
 * @brief Request shared attribute values from the server
 */
int tb_attributes_request_shared(tb_client_t *client, const char *keys[],
                                 size_t num_keys, tb_attribute_response_cb_t cb,
                                 void *user_data, uint32_t timeout_ms);

/**
 * @brief Subscribe to shared attribute updates
 */
int tb_attributes_subscribe(tb_client_t *client, tb_shared_attribute_cb_t cb,
                            void *user_data);

/**
 * @brief Unsubscribe from shared attribute updates
 */
int tb_attributes_unsubscribe(tb_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* TB_ATTRIBUTES_H */
