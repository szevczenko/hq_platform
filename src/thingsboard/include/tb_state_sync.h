/**
 *******************************************************************************
 * @file    tb_state_sync.h
 * @brief   Generic ThingsBoard desired-state synchronization engine
 *******************************************************************************
 */

#ifndef TB_STATE_SYNC_H
#define TB_STATE_SYNC_H

#include "tb_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB_STATE_SYNC_MAX_STATE_BYTES 128u
#define TB_STATE_SYNC_MAX_RPC_HANDLERS 12u
#define TB_STATE_SYNC_MAX_RPC_METHOD_LEN 32u
#define TB_STATE_SYNC_MAX_RPC_PAYLOAD_BYTES 512u

#define TB_STATE_SYNC_TIMEOUT_DEFAULT_MS 10000u
#define TB_STATE_SYNC_TIMEOUT_MIN_MS 100u
#define TB_STATE_SYNC_TIMEOUT_MAX_MS 60000u
#define TB_STATE_SYNC_RETRY_INITIAL_DEFAULT_MS 2000u
#define TB_STATE_SYNC_RETRY_MAX_DEFAULT_MS 30000u
#define TB_STATE_SYNC_RETRY_DELAY_MIN_MS 100u
#define TB_STATE_SYNC_RETRY_DELAY_MAX_MS 600000u
#define TB_STATE_SYNC_MAX_RETRIES_DEFAULT 5u
#define TB_STATE_SYNC_TELEMETRY_PERIOD_DEFAULT_MS 30000u
#define TB_STATE_SYNC_TELEMETRY_PERIOD_MIN_MS 1000u
#define TB_STATE_SYNC_TELEMETRY_PERIOD_MAX_MS 3600000u

typedef enum tb_state_sync_source {
    TB_STATE_SYNC_SOURCE_CLIENT = 0,
    TB_STATE_SYNC_SOURCE_SHARED,
    TB_STATE_SYNC_SOURCE_UPDATE,
    TB_STATE_SYNC_SOURCE_DEFAULT
} tb_state_sync_source_t;

typedef enum tb_state_sync_parse_result {
    TB_STATE_SYNC_PARSE_INVALID = 0,
    TB_STATE_SYNC_PARSE_VALID,
    TB_STATE_SYNC_PARSE_IGNORED
} tb_state_sync_parse_result_t;

typedef tb_state_sync_parse_result_t (*tb_state_sync_parse_fn_t)(
    const char *json, tb_state_sync_source_t source, void *state,
    void *user_data);
typedef bool (*tb_state_sync_apply_fn_t)(const void *state,
                                         bool output_suspended,
                                         void *user_data);
typedef bool (*tb_state_sync_equal_fn_t)(const void *left, const void *right,
                                         void *user_data);
typedef void (*tb_state_sync_state_fn_t)(const void *state, void *user_data);
typedef void (*tb_state_sync_void_fn_t)(void *user_data);
typedef uint32_t (*tb_state_sync_now_fn_t)(void);
typedef void (*tb_state_sync_rpc_fn_t)(const char *method,
                                       const char *params_json,
                                       uint32_t request_id,
                                       void *user_data);

typedef struct tb_state_sync_config {
    tb_client_t *client;
    const char *const *client_keys;
    size_t client_key_count;
    const char *const *shared_keys;
    size_t shared_key_count;
    size_t max_payload_bytes;
    size_t state_size;
    uint32_t sync_timeout_ms;
    uint32_t retry_initial_delay_ms;
    uint32_t retry_max_delay_ms;
    uint32_t max_retries;
    uint32_t telemetry_period_ms;
    tb_state_sync_now_fn_t now_ms;
    tb_state_sync_parse_fn_t parse_state;
    tb_state_sync_apply_fn_t apply_state;
    tb_state_sync_equal_fn_t states_equal;
    tb_state_sync_void_fn_t force_inactive;
    tb_state_sync_state_fn_t publish_applied_attributes;
    tb_state_sync_void_fn_t publish_telemetry;
    void *user_data;
} tb_state_sync_config_t;

typedef enum tb_state_sync_status {
    TB_STATE_SYNC_OK = 0,
    TB_STATE_SYNC_ERR_INVALID_ARGUMENT = -1,
    TB_STATE_SYNC_ERR_NOT_INITIALIZED = -2,
    TB_STATE_SYNC_ERR_ALREADY_INITIALIZED = -3,
    TB_STATE_SYNC_ERR_CLIENT_MISMATCH = -4,
    TB_STATE_SYNC_ERR_NO_RESOURCE = -5
} tb_state_sync_status_t;

tb_state_sync_status_t tb_state_sync_init(const tb_state_sync_config_t *config);
void tb_state_sync_deinit(void);
void tb_state_sync_on_connected(tb_client_t *client);
void tb_state_sync_on_disconnected(tb_client_t *client);
void tb_state_sync_poll(tb_client_t *client);
bool tb_state_sync_is_synchronized(tb_client_t *client);
bool tb_state_sync_is_connected(void);
bool tb_state_sync_is_output_suspended(void);
tb_state_sync_status_t tb_state_sync_get_state(tb_client_t *client,
                                                bool *has_state,
                                                void *state_out);
tb_state_sync_status_t tb_state_sync_record_state(const void *state);
void tb_state_sync_publish_applied_state(void);
void tb_state_sync_note_state_change(void);
void tb_state_sync_set_output_suspended(bool suspended);
int tb_state_sync_register_rpc_handler(const char *method,
                                       tb_state_sync_rpc_fn_t handler,
                                       void *user_data);
int tb_state_sync_respond_rpc_error(tb_client_t *client, uint32_t request_id,
                                    const char *error, const char *reason,
                                    const char *method);

#ifdef __cplusplus
}
#endif

#endif /* TB_STATE_SYNC_H */