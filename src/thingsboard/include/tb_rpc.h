/**
 *******************************************************************************
 * @file    tb_rpc.h
 * @brief   ThingsBoard client – RPC API (server-side and client-side)
 *******************************************************************************
 */

#ifndef TB_RPC_H
#define TB_RPC_H

#include "tb_client.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback for server-side RPC requests
 * @param method       RPC method name
 * @param params_json  JSON string of parameters (may be "{}" if empty)
 * @param request_id   Request ID for responding
 * @param user_data    User-supplied context
 */
typedef void (*tb_server_rpc_cb_t)(const char *method, const char *params_json,
                                   uint32_t request_id, void *user_data);

/**
 * @brief Callback for client-side RPC responses
 * @param result         Request result status
 * @param response_json  JSON response from server
 * @param user_data      User-supplied context
 */
typedef void (*tb_client_rpc_cb_t)(tb_request_result_t result,
                                   const char *response_json,
                                   void *user_data);

/**
 * @brief Subscribe to server-side RPC requests
 */
int tb_rpc_subscribe_server(tb_client_t *client, tb_server_rpc_cb_t cb,
                            void *user_data);

/**
 * @brief Unsubscribe from server-side RPC requests
 */
int tb_rpc_unsubscribe_server(tb_client_t *client);

/**
 * @brief Send response to a server-side RPC request
 * @param client         Client handle
 * @param request_id     Request ID from the callback
 * @param response_json  JSON response payload
 * @return 0 on success, negative on error
 */
int tb_rpc_respond(tb_client_t *client, uint32_t request_id,
                   const char *response_json);

/**
 * @brief Send a client-side RPC request to the server
 * @param client       Client handle
 * @param method       RPC method name
 * @param params_json  JSON parameters (NULL or "{}" for no params)
 * @param cb           Response callback
 * @param user_data    User data for callback
 * @param timeout_ms   Response timeout in ms
 * @return 0 on success, negative on error
 */
int tb_rpc_request(tb_client_t *client, const char *method,
                   const char *params_json, tb_client_rpc_cb_t cb,
                   void *user_data, uint32_t timeout_ms);

/* Internal lifecycle hook used by tb_client on transport disconnect. */
void tb_rpc_handle_disconnect(tb_client_t *client);

/* Internal lifecycle hook used by tb_client on deinit. */
void tb_rpc_deinit(tb_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* TB_RPC_H */
