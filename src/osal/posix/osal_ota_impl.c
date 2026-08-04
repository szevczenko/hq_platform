#include "osal_ota.h"

#include <string.h>

#include "osal_log.h"

typedef struct {
    bool active;
    size_t total_size;
    size_t written_size;
} osal_ota_ctx_t;

static osal_ota_ctx_t s_ota_ctx;

osal_status_t osal_ota_init(void)
{
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

    memset(&s_ota_ctx, 0, sizeof(s_ota_ctx));
    s_ota_ctx.active = true;
    s_ota_ctx.total_size = descriptor->total_size;

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
    if (s_ota_ctx.written_size + len > s_ota_ctx.total_size) {
        return OSAL_ERR_OUTPUT_TOO_LARGE;
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
        return OSAL_ERROR;
    }

    osal_log_info("[osal_ota] POSIX dummy finish: apply=%d written=%zu",
                  apply_update ? 1 : 0, s_ota_ctx.written_size);
    s_ota_ctx.active = false;
    return OSAL_SUCCESS;
}

osal_status_t osal_ota_abort(void)
{
    s_ota_ctx.active = false;
    s_ota_ctx.written_size = 0;
    s_ota_ctx.total_size = 0;
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