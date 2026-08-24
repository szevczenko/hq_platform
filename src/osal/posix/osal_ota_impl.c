#include "osal_ota.h"

#include <string.h>

#include "mongoose.h"
#include "osal_log.h"

#define OSAL_OTA_SHA256_DIGEST_LEN 32U
#define OSAL_OTA_SHA256_HEX_LEN    (OSAL_OTA_SHA256_DIGEST_LEN * 2U)

typedef struct {
    bool active;
    bool verified;
    size_t total_size;
    size_t written_size;
    uint8_t expected_digest[OSAL_OTA_SHA256_DIGEST_LEN];
    mg_sha256_ctx sha256_ctx;
} osal_ota_ctx_t;

static osal_ota_ctx_t s_ota_ctx;
static bool s_running_image_pending_confirmation;

static void osal_ota_reset_ctx(void)
{
    memset(&s_ota_ctx, 0, sizeof(s_ota_ctx));
}

static bool osal_ota_is_sha256_algorithm(const char *algorithm)
{
    return algorithm != NULL && strcmp(algorithm, "SHA256") == 0;
}

static int osal_ota_hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

static osal_status_t osal_ota_parse_sha256_checksum(const char *checksum,
                                                    uint8_t *digest)
{
    size_t checksum_len = 0;

    if (checksum == NULL || digest == NULL) {
        return OSAL_INVALID_POINTER;
    }

    checksum_len = strlen(checksum);
    if (checksum_len != OSAL_OTA_SHA256_HEX_LEN) {
        return OSAL_ERR_INVALID_ARGUMENT;
    }

    for (size_t index = 0; index < OSAL_OTA_SHA256_DIGEST_LEN; index++) {
        int high = osal_ota_hex_nibble(checksum[index * 2U]);
        int low = osal_ota_hex_nibble(checksum[index * 2U + 1U]);

        if (high < 0 || low < 0) {
            return OSAL_ERR_INVALID_ARGUMENT;
        }

        digest[index] = (uint8_t)((high << 4) | low);
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_ota_init(void)
{
    s_running_image_pending_confirmation = false;
    return OSAL_SUCCESS;
}

osal_status_t osal_ota_get_security_info(osal_ota_security_info_t *info)
{
    if (info == NULL) {
        return OSAL_INVALID_POINTER;
    }

    memset(info, 0, sizeof(*info));
    info->boot_state = s_running_image_pending_confirmation
                           ? OSAL_OTA_BOOT_STATE_PENDING_CONFIRMATION
                           : OSAL_OTA_BOOT_STATE_VALID;
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
    if (!osal_ota_is_sha256_algorithm(descriptor->checksum_algorithm)) {
        return OSAL_ERR_INVALID_ARGUMENT;
    }

    memset(&s_ota_ctx, 0, sizeof(s_ota_ctx));
    osal_status_t status = osal_ota_parse_sha256_checksum(
        descriptor->checksum, s_ota_ctx.expected_digest);
    if (status != OSAL_SUCCESS) {
        osal_ota_reset_ctx();
        return status;
    }

    s_ota_ctx.active = true;
    s_ota_ctx.total_size = descriptor->total_size;
    mg_sha256_init(&s_ota_ctx.sha256_ctx);

    osal_log_info("[osal_ota] POSIX dummy begin: title=%s version=%s size=%zu",
                  descriptor->title ? descriptor->title : "",
                  descriptor->version ? descriptor->version : "",
                  descriptor->total_size);
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

    mg_sha256_update(&s_ota_ctx.sha256_ctx, data, len);
    s_ota_ctx.written_size += len;
    return OSAL_SUCCESS;
}

osal_status_t osal_ota_verify(void)
{
    uint8_t actual_digest[OSAL_OTA_SHA256_DIGEST_LEN];

    if (!s_ota_ctx.active) {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }
    if (s_ota_ctx.written_size != s_ota_ctx.total_size) {
        return OSAL_ERROR;
    }
    if (s_ota_ctx.verified) {
        return OSAL_SUCCESS;
    }

    mg_sha256_final(actual_digest, &s_ota_ctx.sha256_ctx);
    if (memcmp(actual_digest, s_ota_ctx.expected_digest,
               sizeof(actual_digest)) != 0) {
        return OSAL_ERROR;
    }

    s_ota_ctx.verified = true;
    return OSAL_SUCCESS;
}

bool osal_ota_needs_confirmation(void)
{
    return s_running_image_pending_confirmation;
}

osal_status_t osal_ota_confirm_running_image(void)
{
    return s_running_image_pending_confirmation ? OSAL_ERR_INCORRECT_OBJ_STATE
                                                : OSAL_SUCCESS;
}

osal_status_t osal_ota_finish(bool apply_update)
{
    if (!s_ota_ctx.active) {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }
    if (s_ota_ctx.written_size != s_ota_ctx.total_size) {
        return OSAL_ERROR;
    }
    if (!s_ota_ctx.verified) {
        return OSAL_ERR_INCORRECT_OBJ_STATE;
    }

    osal_log_info("[osal_ota] POSIX dummy finish: apply=%d written=%zu",
                  apply_update ? 1 : 0, s_ota_ctx.written_size);
    osal_ota_reset_ctx();
    return OSAL_SUCCESS;
}

osal_status_t osal_ota_abort(void)
{
    osal_ota_reset_ctx();
    osal_log_warning("[osal_ota] POSIX dummy abort");
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