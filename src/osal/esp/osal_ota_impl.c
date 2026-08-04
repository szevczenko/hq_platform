#include "osal_ota.h"

#include <inttypes.h>
#include <string.h>

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "osal_log.h"

typedef struct {
    bool active;
    esp_ota_handle_t handle;
    const esp_partition_t *update_partition;
    size_t total_size;
    size_t written_size;
} osal_ota_ctx_t;

static osal_ota_ctx_t s_ota_ctx;

static const char *osal_ota_state_to_string(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:
        return "VALID";
    case ESP_OTA_IMG_INVALID:
        return "INVALID";
    case ESP_OTA_IMG_ABORTED:
        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:
    default:
        return "UNDEFINED";
    }
}

static osal_status_t osal_ota_from_esp_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return OSAL_SUCCESS;
    case ESP_ERR_INVALID_ARG:
        return OSAL_ERR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_SIZE:
        return OSAL_ERR_INVALID_SIZE;
    case ESP_ERR_NO_MEM:
        return OSAL_ERR_OBJECT_IN_USE;
    case ESP_ERR_NOT_FOUND:
        return OSAL_ERR_EMPTY_SET;
    case ESP_ERR_OTA_VALIDATE_FAILED:
        return OSAL_ERROR;
    case ESP_ERR_OTA_ROLLBACK_INVALID_STATE:
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    default:
        return OSAL_ERROR;
    }
}

static void osal_ota_reset_ctx(void)
{
    memset(&s_ota_ctx, 0, sizeof(s_ota_ctx));
}

osal_status_t osal_ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        osal_log_warning("[osal_ota] Cannot get running partition");
        return OSAL_ERROR;
    }

    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED) {
        osal_log_info("[osal_ota] No OTA state for running partition subtype=0x%02x (%s)",
                      running->subtype, esp_err_to_name(err));
        /* Not an OTA slot state (for example factory app) or rollback not used. */
        return OSAL_SUCCESS;
    }
    if (err != ESP_OK) {
        osal_log_warning("[osal_ota] esp_ota_get_state_partition failed: %s",
                         esp_err_to_name(err));
        return osal_ota_from_esp_err(err);
    }

    osal_log_info("[osal_ota] Running partition=0x%08" PRIx32 " subtype=0x%02x state=%s",
                  running->address,
                  running->subtype,
                  osal_ota_state_to_string(ota_state));

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            osal_log_error("[osal_ota] Failed to confirm running image: %s",
                           esp_err_to_name(err));
            return osal_ota_from_esp_err(err);
        }
        osal_log_info("[osal_ota] Running OTA image confirmed");
    } else {
        osal_log_info("[osal_ota] Confirmation not required for state=%s",
                      osal_ota_state_to_string(ota_state));
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_ota_begin(const osal_ota_descriptor_t *descriptor)
{
    if (descriptor == NULL) {
        return OSAL_INVALID_POINTER;
    }

    if (s_ota_ctx.active) {
        return OSAL_ERR_OBJECT_IN_USE;
    }
    if (descriptor->total_size == 0) {
        return OSAL_ERR_INVALID_SIZE;
    }

    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        osal_log_error("[osal_ota] No OTA partition available");
        return OSAL_ERR_EMPTY_SET;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, descriptor->total_size,
                                  &handle);
    if (err != ESP_OK) {
        osal_log_error("[osal_ota] esp_ota_begin failed: %s",
                       esp_err_to_name(err));
        return osal_ota_from_esp_err(err);
    }

    osal_ota_reset_ctx();
    s_ota_ctx.active = true;
    s_ota_ctx.handle = handle;
    s_ota_ctx.update_partition = update_partition;
    s_ota_ctx.total_size = descriptor->total_size;

    osal_log_info("[osal_ota] OTA begin: title=%s version=%s size=%zu partition=0x%08" PRIx32,
                  descriptor->title ? descriptor->title : "",
                  descriptor->version ? descriptor->version : "",
                  descriptor->total_size,
                  update_partition->address);
    return OSAL_SUCCESS;
}

osal_status_t osal_ota_write(const uint8_t *data, size_t len)
{
    if (!s_ota_ctx.active) {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }
    if (len > 0 && data == NULL) {
        return OSAL_INVALID_POINTER;
    }
    if (len == 0) {
        return OSAL_SUCCESS;
    }
    if (s_ota_ctx.written_size + len > s_ota_ctx.total_size) {
        return OSAL_ERR_OUTPUT_TOO_LARGE;
    }

    esp_err_t err = esp_ota_write(s_ota_ctx.handle, data, len);
    if (err != ESP_OK) {
        osal_log_error("[osal_ota] esp_ota_write failed: %s",
                       esp_err_to_name(err));
        return osal_ota_from_esp_err(err);
    }

    s_ota_ctx.written_size += len;
    return OSAL_SUCCESS;
}

osal_status_t osal_ota_finish(bool apply_update)
{
    if (!s_ota_ctx.active) {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }
    if (s_ota_ctx.written_size != s_ota_ctx.total_size) {
        osal_log_error("[osal_ota] Incomplete image: written=%zu total=%zu",
                       s_ota_ctx.written_size, s_ota_ctx.total_size);
        return OSAL_ERROR;
    }

    esp_err_t err = esp_ota_end(s_ota_ctx.handle);
    if (err != ESP_OK) {
        osal_log_error("[osal_ota] esp_ota_end failed: %s", esp_err_to_name(err));
        osal_ota_reset_ctx();
        return osal_ota_from_esp_err(err);
    }

    if (apply_update) {
        err = esp_ota_set_boot_partition(s_ota_ctx.update_partition);
        if (err != ESP_OK) {
            osal_log_error("[osal_ota] esp_ota_set_boot_partition failed: %s",
                           esp_err_to_name(err));
            osal_ota_reset_ctx();
            return osal_ota_from_esp_err(err);
        }
    }

    osal_log_info("[osal_ota] OTA finish: apply=%d written=%zu",
                  apply_update ? 1 : 0, s_ota_ctx.written_size);
    osal_ota_reset_ctx();

    if (apply_update) {
        osal_log_info("[osal_ota] Rebooting into updated partition");
        esp_restart();
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_ota_abort(void)
{
    if (s_ota_ctx.active) {
        esp_err_t err = esp_ota_abort(s_ota_ctx.handle);
        if (err != ESP_OK) {
            osal_log_warning("[osal_ota] esp_ota_abort failed: %s",
                             esp_err_to_name(err));
        }
    }

    osal_ota_reset_ctx();
    osal_log_warning("[osal_ota] OTA aborted");
    return OSAL_SUCCESS;
}

osal_status_t osal_ota_get_progress(size_t *written_size, size_t *total_size)
{
    if (written_size == NULL || total_size == NULL) {
        return OSAL_INVALID_POINTER;
    }

    *written_size = s_ota_ctx.written_size;
    *total_size = s_ota_ctx.total_size;
    return OSAL_SUCCESS;
}