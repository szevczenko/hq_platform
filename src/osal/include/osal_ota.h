#ifndef OSAL_OTA_H
#define OSAL_OTA_H

#include "osal_common_type.h"
#include "osal_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title;
    const char *version;
    const char *checksum;
    const char *checksum_algorithm;
    size_t total_size;
} osal_ota_descriptor_t;

typedef enum {
    OSAL_OTA_BOOT_STATE_UNKNOWN = 0,
    OSAL_OTA_BOOT_STATE_VALID,
    OSAL_OTA_BOOT_STATE_PENDING_CONFIRMATION,
    OSAL_OTA_BOOT_STATE_INVALID
} osal_ota_boot_state_t;

typedef struct {
    bool secure_boot_enforced;
    bool flash_encryption_enabled;
    bool rollback_enabled;
    bool anti_rollback_enabled;
    osal_ota_boot_state_t boot_state;
} osal_ota_security_info_t;

/**
 * @brief Initialize OTA subsystem for current boot.
 *
 * On ESP targets this checks whether the running OTA image is pending
 * verification. Confirmation is deferred until
 * osal_ota_confirm_running_image() is called.
 */
osal_status_t osal_ota_init(void);
osal_status_t osal_ota_get_security_info(osal_ota_security_info_t *info);

osal_status_t osal_ota_begin(const osal_ota_descriptor_t *descriptor);
osal_status_t osal_ota_write(const uint8_t *data, size_t len);
osal_status_t osal_ota_verify(void);
bool osal_ota_needs_confirmation(void);
osal_status_t osal_ota_confirm_running_image(void);
osal_status_t osal_ota_finish(bool apply_update);
osal_status_t osal_ota_abort(void);
osal_status_t osal_ota_get_progress(size_t *written_size, size_t *total_size);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_OTA_H */