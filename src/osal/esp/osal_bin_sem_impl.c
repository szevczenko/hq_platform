#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "osal_bin_sem.h"
#include "osal_assert.h"
#include "osal_macro.h"

static TickType_t osal_timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == OSAL_MAX_DELAY)
    {
        return portMAX_DELAY;
    }

    return pdMS_TO_TICKS(timeout_ms);
}

osal_status_t osal_bin_sem_create(osal_bin_sem_id_t *sem_id,
                                  const char *name,
                                  uint32_t initial_value)
{
    SemaphoreHandle_t sem;

    OSAL_CHECK_POINTER(sem_id);
    ARGCHECK(initial_value <= OSAL_SEM_FULL, OSAL_INVALID_SEM_VALUE);

    if (name != NULL)
    {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }

    sem = xSemaphoreCreateBinary();
    if (sem == NULL)
    {
        return OSAL_ERROR;
    }

    if (initial_value == OSAL_SEM_FULL)
    {
        (void)xSemaphoreGive(sem);
    }

    *sem_id = sem;
    return OSAL_SUCCESS;
}

osal_status_t osal_bin_sem_delete(osal_bin_sem_id_t sem_id)
{
    OSAL_CHECK_POINTER(sem_id);

    vSemaphoreDelete(sem_id);
    return OSAL_SUCCESS;
}

osal_status_t osal_bin_sem_give(osal_bin_sem_id_t sem_id)
{
    OSAL_CHECK_POINTER(sem_id);

    if (xSemaphoreGive(sem_id) != pdTRUE)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_bin_sem_take(osal_bin_sem_id_t sem_id)
{
    OSAL_CHECK_POINTER(sem_id);

    if (xSemaphoreTake(sem_id, portMAX_DELAY) != pdTRUE)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_bin_sem_timed_wait(osal_bin_sem_id_t sem_id, uint32_t timeout_ms)
{
    TickType_t ticks;

    OSAL_CHECK_POINTER(sem_id);

    ticks = osal_timeout_to_ticks(timeout_ms);
    if (xSemaphoreTake(sem_id, ticks) != pdTRUE)
    {
        if (timeout_ms == OSAL_MAX_DELAY)
        {
            return OSAL_SEM_FAILURE;
        }
        return OSAL_SEM_TIMEOUT;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_bin_sem_give_from_isr(osal_bin_sem_id_t sem_id)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    OSAL_CHECK_POINTER(sem_id);

    if (xSemaphoreGiveFromISR(sem_id, &higher_priority_task_woken) != pdTRUE)
    {
        return OSAL_SEM_FAILURE;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
    return OSAL_SUCCESS;
}

osal_status_t osal_bin_sem_take_from_isr(osal_bin_sem_id_t sem_id)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    OSAL_CHECK_POINTER(sem_id);

    if (xSemaphoreTakeFromISR(sem_id, &higher_priority_task_woken) != pdTRUE)
    {
        return OSAL_SEM_TIMEOUT;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
    return OSAL_SUCCESS;
}
