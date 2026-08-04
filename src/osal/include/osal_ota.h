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

/**
 * @brief Initialize OTA subsystem for current boot.
 *
 * On ESP targets this confirms a newly booted OTA image when rollback is
 * enabled and the image is in pending-verify state.
 */
osal_status_t osal_ota_init(void);

osal_status_t osal_ota_begin(const osal_ota_descriptor_t *descriptor);
osal_status_t osal_ota_write(const uint8_t *data, size_t len);
osal_status_t osal_ota_finish(bool apply_update);
osal_status_t osal_ota_abort(void);
osal_status_t osal_ota_get_progress(size_t *written_size, size_t *total_size);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_OTA_H */