#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "osal_mutex.h"
#include "osal_assert.h"
#include "osal_macro.h"

osal_status_t osal_mutex_create(osal_mutex_id_t *mutex_id, const char *name)
{
    SemaphoreHandle_t mutex;

    OSAL_CHECK_POINTER(mutex_id);

    if (name != NULL)
    {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }

    mutex = xSemaphoreCreateMutex();
    if (mutex == NULL)
    {
        return OSAL_ERROR;
    }

    *mutex_id = mutex;
    return OSAL_SUCCESS;
}

osal_status_t osal_mutex_delete(osal_mutex_id_t mutex_id)
{
    OSAL_CHECK_POINTER(mutex_id);

    vSemaphoreDelete(mutex_id);
    return OSAL_SUCCESS;
}

osal_status_t osal_mutex_take(osal_mutex_id_t mutex_id)
{
    OSAL_CHECK_POINTER(mutex_id);

    if (xSemaphoreTake(mutex_id, portMAX_DELAY) != pdTRUE)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_mutex_give(osal_mutex_id_t mutex_id)
{
    OSAL_CHECK_POINTER(mutex_id);

    if (xSemaphoreGive(mutex_id) != pdTRUE)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}
