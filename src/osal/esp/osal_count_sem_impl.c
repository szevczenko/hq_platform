#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "osal_count_sem.h"
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

osal_status_t osal_count_sem_create(osal_count_sem_id_t *sem_id,
                                    const char *name,
                                    uint32_t initial_value,
                                    uint32_t max_value)
{
    SemaphoreHandle_t sem;
    uint32_t effective_max;

    OSAL_CHECK_POINTER(sem_id);

    if (name != NULL)
    {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }

    effective_max = max_value;
    if (effective_max == 0U)
    {
        effective_max = (initial_value == 0U) ? 1U : initial_value;
    }

    if (initial_value > effective_max)
    {
        return OSAL_INVALID_SEM_VALUE;
    }

    sem = xSemaphoreCreateCounting((UBaseType_t)effective_max,
                                   (UBaseType_t)initial_value);
    if (sem == NULL)
    {
        return OSAL_ERROR;
    }

    *sem_id = sem;
    return OSAL_SUCCESS;
}

osal_status_t osal_count_sem_delete(osal_count_sem_id_t sem_id)
{
    OSAL_CHECK_POINTER(sem_id);

    vSemaphoreDelete(sem_id);
    return OSAL_SUCCESS;
}

osal_status_t osal_count_sem_give(osal_count_sem_id_t sem_id)
{
    OSAL_CHECK_POINTER(sem_id);

    if (xSemaphoreGive(sem_id) != pdTRUE)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_count_sem_take(osal_count_sem_id_t sem_id)
{
    OSAL_CHECK_POINTER(sem_id);

    if (xSemaphoreTake(sem_id, portMAX_DELAY) != pdTRUE)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_count_sem_timed_wait(osal_count_sem_id_t sem_id, uint32_t timeout_ms)
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

osal_status_t osal_count_sem_give_from_isr(osal_count_sem_id_t sem_id)
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

osal_status_t osal_count_sem_take_from_isr(osal_count_sem_id_t sem_id)
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

uint32_t osal_count_sem_get_count(osal_count_sem_id_t sem_id)
{
    if (sem_id == NULL)
    {
        return 0U;
    }

    return (uint32_t)uxSemaphoreGetCount(sem_id);
}
