/**
 *******************************************************************************
 * @file    tb_provision.h
 * @brief   ThingsBoard client – device provisioning API
 *******************************************************************************
 */

#ifndef TB_PROVISION_H
#define TB_PROVISION_H

#include "tb_client.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_name;             /**< Device name (optional) */
    const char *provision_device_key;    /**< Provisioning device key */
    const char *provision_device_secret; /**< Provisioning device secret */
    const char *credentials_type;        /**< NULL, "ACCESS_TOKEN", "MQTT_BASIC", "X509_CERTIFICATE" */
    const char *token;                   /**< For ACCESS_TOKEN type */
    const char *username;                /**< For MQTT_BASIC type */
    const char *password;                /**< For MQTT_BASIC type */
    const char *client_id;               /**< For MQTT_BASIC type */
    const char *certificate_hash;        /**< For X509_CERTIFICATE type */
} tb_provision_request_t;

/**
 * @brief Callback for provisioning response
 * @param response_json  JSON response from server with credentials
 * @param user_data      User-supplied context
 */
typedef void (*tb_provision_cb_t)(const char *response_json, void *user_data);

/**
 * @brief Send a device provisioning request
 * @param client       Client handle (must be connected with "provision" token)
 * @param req          Provisioning request parameters
 * @param cb           Response callback
 * @param user_data    User data for callback
 * @param timeout_ms   Response timeout
 * @return 0 on success, negative on error
 */
int tb_provision_request(tb_client_t *client, const tb_provision_request_t *req,
                         tb_provision_cb_t cb, void *user_data,
                         uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* TB_PROVISION_H */
