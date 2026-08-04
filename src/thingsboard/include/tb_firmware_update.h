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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tb_firmware_applied_cb_t)(const char *new_title,
                                         const char *new_version,
                                         void *user_data);

typedef struct {
    const char *current_title;
    const char *current_version;
    uint32_t chunk_size;
    tb_firmware_applied_cb_t on_applied;
    void *user_data;
} tb_firmware_update_config_t;

int tb_firmware_update_init(tb_client_t *client,
                            const tb_firmware_update_config_t *config);

int tb_firmware_update_request_check(tb_client_t *client);

bool tb_firmware_update_is_in_progress(void);

void tb_firmware_update_deinit(tb_client_t *client);

#ifdef __cplusplus
}
#endif

#endif /* TB_FIRMWARE_UPDATE_H */