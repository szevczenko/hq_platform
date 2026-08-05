#include "osal_ota_state.h"

#include <string.h>
#include <stdint.h>

#include "osal_file.h"
#include "osal_littlefs_backend.h"
#include "osal_mount.h"

#define OSAL_OTA_STATE_FILE_PATH      "/ota_state.bin"
#define OSAL_OTA_STATE_IMAGE_PATH     "/tmp/osal_ota_state.img"
#define OSAL_OTA_STATE_MOUNT_POINT    "/"
#define OSAL_OTA_STATE_MAGIC          0x4f544131U
#define OSAL_OTA_STATE_FORMAT_VERSION 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t download_state;
    char title[OSAL_OTA_STATE_TITLE_LEN];
    char app_version[OSAL_OTA_STATE_VERSION_LEN];
    char checksum[OSAL_OTA_STATE_CHECKSUM_LEN];
    char last_error[OSAL_OTA_STATE_ERROR_LEN];
} osal_ota_state_record_t;

static osal_status_t osal_ota_state_ensure_storage(void)
{
    if (g_osal_lfs_mounted) {
        return OSAL_SUCCESS;
    }

    osal_status_t status = osal_initfs((char *)OSAL_OTA_STATE_IMAGE_PATH,
                                       OSAL_OTA_STATE_IMAGE_PATH,
                                       OSAL_OTA_STATE_MOUNT_POINT,
                                       0U, 0U);
    if (status != OSAL_SUCCESS) {
        return status;
    }

    status = osal_mount(OSAL_OTA_STATE_IMAGE_PATH, OSAL_OTA_STATE_MOUNT_POINT);
    if (status == OSAL_SUCCESS) {
        return OSAL_SUCCESS;
    }

    status = osal_mkfs((char *)OSAL_OTA_STATE_IMAGE_PATH,
                       OSAL_OTA_STATE_IMAGE_PATH,
                       OSAL_OTA_STATE_MOUNT_POINT,
                       0U, 0U);
    if (status != OSAL_SUCCESS) {
        return status;
    }

    return osal_mount(OSAL_OTA_STATE_IMAGE_PATH, OSAL_OTA_STATE_MOUNT_POINT);
}

static void osal_ota_state_copy_in(osal_ota_state_record_t *record,
                                   const osal_ota_state_t *state)
{
    memset(record, 0, sizeof(*record));
    record->magic = OSAL_OTA_STATE_MAGIC;
    record->version = OSAL_OTA_STATE_FORMAT_VERSION;
    record->download_state = (uint32_t)state->download_state;
    memcpy(record->title, state->title, sizeof(record->title));
    memcpy(record->app_version, state->version, sizeof(record->app_version));
    memcpy(record->checksum, state->checksum, sizeof(record->checksum));
    memcpy(record->last_error, state->last_error, sizeof(record->last_error));
}

static osal_status_t osal_ota_state_copy_out(osal_ota_state_t *state,
                                             const osal_ota_state_record_t *record)
{
    if (record->magic != OSAL_OTA_STATE_MAGIC ||
        record->version != OSAL_OTA_STATE_FORMAT_VERSION ||
        record->download_state > OSAL_OTA_DOWNLOAD_STATE_FAILED) {
        return OSAL_ERROR;
    }

    memset(state, 0, sizeof(*state));
    state->download_state = (osal_ota_download_state_t)record->download_state;
    memcpy(state->title, record->title, sizeof(state->title));
    memcpy(state->version, record->app_version, sizeof(state->version));
    memcpy(state->checksum, record->checksum, sizeof(state->checksum));
    memcpy(state->last_error, record->last_error, sizeof(state->last_error));
    return OSAL_SUCCESS;
}

osal_status_t osal_ota_state_save(const osal_ota_state_t *state)
{
    osal_ota_state_record_t record;
    osal_status_t status;
    osal_file_id_t file_id;
    int32_t written;

    if (state == NULL) {
        return OSAL_INVALID_POINTER;
    }

    status = osal_ota_state_ensure_storage();
    if (status != OSAL_SUCCESS) {
        return status;
    }

    osal_ota_state_copy_in(&record, state);
    file_id = osal_open_create(OSAL_OTA_STATE_FILE_PATH,
                               OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
                               OSAL_WRITE_ONLY);
    if (file_id < 0) {
        return (osal_status_t)file_id;
    }

    written = osal_write(file_id, &record, sizeof(record));
    (void)osal_close(file_id);
    return (written == (int32_t)sizeof(record)) ? OSAL_SUCCESS : OSAL_ERROR;
}

osal_status_t osal_ota_state_load(osal_ota_state_t *state)
{
    osal_ota_state_record_t record;
    osal_status_t status;
    osal_fstat_t file_stats;
    osal_file_id_t file_id;
    int32_t read_count;

    if (state == NULL) {
        return OSAL_INVALID_POINTER;
    }

    status = osal_ota_state_ensure_storage();
    if (status != OSAL_SUCCESS) {
        return status;
    }

    if (osal_stat(OSAL_OTA_STATE_FILE_PATH, &file_stats) != OSAL_SUCCESS) {
        return OSAL_ERR_EMPTY_SET;
    }
    if (file_stats.file_size != sizeof(record)) {
        return OSAL_ERROR;
    }

    file_id = osal_open_create(OSAL_OTA_STATE_FILE_PATH,
                               OSAL_FILE_FLAG_NONE,
                               OSAL_READ_ONLY);
    if (file_id < 0) {
        return (osal_status_t)file_id;
    }

    read_count = osal_read(file_id, &record, sizeof(record));
    (void)osal_close(file_id);
    if (read_count != (int32_t)sizeof(record)) {
        return OSAL_ERROR;
    }

    return osal_ota_state_copy_out(state, &record);
}

osal_status_t osal_ota_state_clear(void)
{
    osal_status_t status = osal_ota_state_ensure_storage();
    if (status != OSAL_SUCCESS) {
        return status;
    }

    status = (osal_status_t)osal_remove(OSAL_OTA_STATE_FILE_PATH);
    if (status == OSAL_SUCCESS || status == OSAL_FS_ERR_PATH_INVALID) {
        return OSAL_SUCCESS;
    }

    if (status == OSAL_ERROR) {
        osal_fstat_t file_stats;
        if (osal_stat(OSAL_OTA_STATE_FILE_PATH, &file_stats) ==
            OSAL_FS_ERR_PATH_INVALID) {
            return OSAL_SUCCESS;
        }
    }

    return status;
}