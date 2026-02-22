#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "osal_queue.h"
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

osal_status_t osal_queue_create(osal_queue_id_t *queue_id,
                                const char *name,
                                uint32_t max_items,
                                uint32_t item_size)
{
    QueueHandle_t queue;

    OSAL_CHECK_POINTER(queue_id);

    if (name != NULL)
    {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }

    ARGCHECK(max_items > 0U, OSAL_QUEUE_INVALID_SIZE);
    ARGCHECK(item_size > 0U, OSAL_QUEUE_INVALID_SIZE);

    queue = xQueueCreate((UBaseType_t)max_items, (UBaseType_t)item_size);
    if (queue == NULL)
    {
        return OSAL_ERROR;
    }

#if defined(configQUEUE_REGISTRY_SIZE) && (configQUEUE_REGISTRY_SIZE > 0)
    if (name != NULL)
    {
        vQueueAddToRegistry(queue, name);
    }
#endif

    *queue_id = queue;
    return OSAL_SUCCESS;
}

osal_status_t osal_queue_send(osal_queue_id_t queue_id,
                              const void *item,
                              uint32_t timeout_ms)
{
    TickType_t ticks;

    OSAL_CHECK_POINTER(queue_id);
    OSAL_CHECK_POINTER(item);

    ticks = osal_timeout_to_ticks(timeout_ms);
    if (xQueueSend(queue_id, item, ticks) != pdTRUE)
    {
        if (timeout_ms == 0U)
        {
            return OSAL_QUEUE_FULL;
        }
        if (timeout_ms == OSAL_MAX_DELAY)
        {
            return OSAL_ERROR;
        }
        return OSAL_QUEUE_TIMEOUT;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_queue_receive(osal_queue_id_t queue_id,
                                 void *buffer,
                                 uint32_t timeout_ms)
{
    TickType_t ticks;

    OSAL_CHECK_POINTER(queue_id);
    OSAL_CHECK_POINTER(buffer);

    ticks = osal_timeout_to_ticks(timeout_ms);
    if (xQueueReceive(queue_id, buffer, ticks) != pdTRUE)
    {
        if (timeout_ms == 0U)
        {
            return OSAL_QUEUE_EMPTY;
        }
        if (timeout_ms == OSAL_MAX_DELAY)
        {
            return OSAL_ERROR;
        }
        return OSAL_QUEUE_TIMEOUT;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_queue_delete(osal_queue_id_t queue_id)
{
    OSAL_CHECK_POINTER(queue_id);

#if defined(configQUEUE_REGISTRY_SIZE) && (configQUEUE_REGISTRY_SIZE > 0)
    vQueueUnregisterQueue(queue_id);
#endif

    vQueueDelete(queue_id);
    return OSAL_SUCCESS;
}

uint32_t osal_queue_get_count(osal_queue_id_t queue_id)
{
    OSAL_CHECK_POINTER(queue_id);

    return (uint32_t)uxQueueMessagesWaiting(queue_id);
}

osal_status_t osal_queue_send_from_isr(osal_queue_id_t queue_id, const void *item)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    OSAL_CHECK_POINTER(queue_id);
    OSAL_CHECK_POINTER(item);

    if (xQueueSendFromISR(queue_id, item, &higher_priority_task_woken) != pdTRUE)
    {
        return OSAL_QUEUE_FULL;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
    return OSAL_SUCCESS;
}

osal_status_t osal_queue_receive_from_isr(osal_queue_id_t queue_id, void *buffer)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    OSAL_CHECK_POINTER(queue_id);
    OSAL_CHECK_POINTER(buffer);

    if (xQueueReceiveFromISR(queue_id, buffer, &higher_priority_task_woken) != pdTRUE)
    {
        return OSAL_QUEUE_EMPTY;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
    return OSAL_SUCCESS;
}
