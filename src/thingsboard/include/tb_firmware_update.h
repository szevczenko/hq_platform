/**
 *******************************************************************************
 * @file    tb_firmware_update.h
 * @brief   ThingsBoard client – firmware update API
 *******************************************************************************
 */

#ifndef TB_FIRMWARE_UPDATE_H
#define TB_FIRMWARE_UPDATE_H

#include "tb_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB_FIRMWARE_UPDATE_CHUNK_TIMEOUT_DEFAULT_MS 10000U
#define TB_FIRMWARE_UPDATE_CHUNK_RETRIES_DEFAULT    3U
#define TB_FIRMWARE_UPDATE_STR_LEN                  128U

/** @brief Firmware update states, reported as `fw_state` telemetry. */
typedef enum {
    TB_FIRMWARE_UPDATE_STATE_IDLE = 0,
    TB_FIRMWARE_UPDATE_STATE_DOWNLOADING,
    TB_FIRMWARE_UPDATE_STATE_DOWNLOADED,
    TB_FIRMWARE_UPDATE_STATE_VERIFIED,
    TB_FIRMWARE_UPDATE_STATE_UPDATING,
    TB_FIRMWARE_UPDATE_STATE_UPDATED,
    TB_FIRMWARE_UPDATE_STATE_FAILED
} tb_firmware_update_state_t;

typedef void (*tb_firmware_applied_cb_t)(const char *new_title,
                                         const char *new_version,
                                         void *user_data);

/**
 * @brief Verified image is activated for the next boot; the callee owns
 *        the restart (called outside the module lock).
 */
typedef void (*tb_firmware_reboot_required_cb_t)(const char *new_title,
                                                 const char *new_version,
                                                 void *user_data);

typedef struct {
    const char *current_title;
    const char *current_version;
    uint32_t chunk_size;
    tb_firmware_applied_cb_t on_applied;
    void *user_data;
    /** Chunk response timeout enforced by tb_firmware_update_poll(); 0 = default. */
    uint32_t chunk_timeout_ms;
    /** Re-requests of one chunk before the update fails; 0 = default. */
    uint32_t max_chunk_retries;
    /** When set, the restart is deferred to this callback instead of
     *  happening immediately inside the chunk handler. */
    tb_firmware_reboot_required_cb_t on_reboot_required;
} tb_firmware_update_config_t;

typedef struct {
    tb_firmware_update_state_t state;
    size_t downloaded_size;
    size_t total_size;
    char target_title[TB_FIRMWARE_UPDATE_STR_LEN];
    char target_version[TB_FIRMWARE_UPDATE_STR_LEN];
    char last_error[TB_FIRMWARE_UPDATE_STR_LEN];
} tb_firmware_update_status_t;

int tb_firmware_update_init(tb_client_t *client,
                            const tb_firmware_update_config_t *config);

int tb_firmware_update_request_check(tb_client_t *client);

int tb_firmware_update_confirm_health(tb_client_t *client);

bool tb_firmware_update_is_in_progress(void);

/**
 * @brief Drive the chunk timeout / bounded re-request policy.
 *
 * Call periodically from the application task while connected.
 */
void tb_firmware_update_poll(tb_client_t *client, uint32_t now_ms);

int tb_firmware_update_get_status(tb_firmware_update_status_t *status);

const char *tb_firmware_update_state_name(tb_firmware_update_state_t state);

void tb_firmware_update_deinit(tb_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* TB_FIRMWARE_UPDATE_H */