#ifndef OSAL_OTA_STATE_H
#define OSAL_OTA_STATE_H

#include "osal_error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OSAL_OTA_STATE_TITLE_LEN    128U
#define OSAL_OTA_STATE_VERSION_LEN  128U
#define OSAL_OTA_STATE_CHECKSUM_LEN 128U
#define OSAL_OTA_STATE_ERROR_LEN    128U

typedef enum {
    OSAL_OTA_DOWNLOAD_STATE_IDLE = 0,
    OSAL_OTA_DOWNLOAD_STATE_DOWNLOADING,
    OSAL_OTA_DOWNLOAD_STATE_DOWNLOADED,
    OSAL_OTA_DOWNLOAD_STATE_VERIFIED,
    OSAL_OTA_DOWNLOAD_STATE_UPDATING,
    OSAL_OTA_DOWNLOAD_STATE_UPDATED,
    OSAL_OTA_DOWNLOAD_STATE_FAILED
} osal_ota_download_state_t;

typedef struct {
    char title[OSAL_OTA_STATE_TITLE_LEN];
    char version[OSAL_OTA_STATE_VERSION_LEN];
    char checksum[OSAL_OTA_STATE_CHECKSUM_LEN];
    osal_ota_download_state_t download_state;
    char last_error[OSAL_OTA_STATE_ERROR_LEN];
} osal_ota_state_t;

osal_status_t osal_ota_state_save(const osal_ota_state_t *state);
osal_status_t osal_ota_state_load(osal_ota_state_t *state);
osal_status_t osal_ota_state_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* OSAL_OTA_STATE_H */