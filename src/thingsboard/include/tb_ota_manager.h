/**
 * @file tb_ota_manager.h
 * @brief OTA lifecycle supervisor for the ThingsBoard firmware updater.
 */

#ifndef TB_OTA_MANAGER_H
#define TB_OTA_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "tb_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_OTA_MANAGER_CHUNK_SIZE_DEFAULT       4096u
#define TB_OTA_MANAGER_REBOOT_DELAY_DEFAULT_MS  2000u
#define TB_OTA_MANAGER_BLINK_PERIOD_DEFAULT_MS  1000u
#define TB_OTA_MANAGER_INIT_RETRY_DEFAULT_MS    10000u
#define TB_OTA_MANAGER_CONFIRM_RETRY_DEFAULT_MS 1000u
#define TB_OTA_MANAGER_STR_MAX_LEN              127u

typedef enum tb_ota_manager_status {
    TB_OTA_MANAGER_OK = 0,
    TB_OTA_MANAGER_ERR_INVALID_ARGUMENT = -1,
    TB_OTA_MANAGER_ERR_ALREADY_INITIALIZED = -2
} tb_ota_manager_status_t;

typedef enum tb_ota_manager_event {
    /** An update entered the download/activation lifecycle. */
    TB_OTA_MANAGER_EVENT_STARTED = 0,
    /** The update failed or was interrupted. */
    TB_OTA_MANAGER_EVENT_FAILED,
    /** The configured restart callback is about to run. */
    TB_OTA_MANAGER_EVENT_RESTARTING
} tb_ota_manager_event_t;

typedef uint32_t (*tb_ota_manager_now_fn_t)(void);
typedef void (*tb_ota_manager_event_fn_t)(tb_ota_manager_event_t event,
                                          void *ctx);
typedef void (*tb_ota_manager_indicator_fn_t)(bool on, void *ctx);
typedef void (*tb_ota_manager_restart_fn_t)(void *ctx);
typedef int (*tb_ota_manager_confirm_fn_t)(tb_client_t *client, void *ctx);

typedef struct tb_ota_manager_config {
    const char *title;
    const char *version;
    uint32_t chunk_size;
    uint32_t chunk_timeout_ms;
    uint32_t chunk_retries;
    uint32_t check_period_ms;
    uint32_t reboot_delay_ms;
    uint32_t blink_period_ms;
    uint32_t init_retry_ms; /**< Updater initialization retry; 0 uses the default. */
    uint32_t confirm_retry_ms; /**< Health confirmation retry; 0 uses the default. */
    tb_ota_manager_now_fn_t now_ms;
    /** Lifecycle notifications, indicator changes, and restart run from poll. */
    tb_ota_manager_event_fn_t on_state;
    tb_ota_manager_indicator_fn_t set_indicator;
    tb_ota_manager_restart_fn_t restart;
    /** Optional health confirmation override; 0 succeeds, other values retry. */
    tb_ota_manager_confirm_fn_t confirm_health;
    void *ctx;
} tb_ota_manager_config_t;

tb_ota_manager_status_t tb_ota_manager_init(
    const tb_ota_manager_config_t *config);
void tb_ota_manager_deinit(void);

void tb_ota_manager_on_connected(tb_client_t *client);
/** These request/notification functions are safe to call from other contexts. */
void tb_ota_manager_on_disconnected(void);
void tb_ota_manager_request_check(void);
void tb_ota_manager_request_image_confirmation(void);
void tb_ota_manager_poll(void);

bool tb_ota_manager_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* TB_OTA_MANAGER_H */