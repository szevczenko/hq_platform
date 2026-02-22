#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "osal_task.h"
#include "osal_assert.h"
#include "osal_macro.h"

struct osal_task_tcb_entry
{
    TaskHandle_t handle;
    StaticTask_t *tcb;
    struct osal_task_tcb_entry *next;
};

static SemaphoreHandle_t osal_task_registry_mutex;
static StaticSemaphore_t osal_task_registry_mutex_buf;
static struct osal_task_tcb_entry *osal_task_registry_head;

static void osal_task_registry_lock(void)
{
    if (osal_task_registry_mutex == NULL)
    {
        osal_task_registry_mutex = xSemaphoreCreateMutexStatic(&osal_task_registry_mutex_buf);
    }

    if (osal_task_registry_mutex != NULL)
    {
        (void)xSemaphoreTake(osal_task_registry_mutex, portMAX_DELAY);
    }
}

static void osal_task_registry_unlock(void)
{
    if (osal_task_registry_mutex != NULL)
    {
        (void)xSemaphoreGive(osal_task_registry_mutex);
    }
}

static struct osal_task_tcb_entry *osal_task_registry_detach(TaskHandle_t handle)
{
    struct osal_task_tcb_entry *prev = NULL;
    struct osal_task_tcb_entry *cur;

    osal_task_registry_lock();
    cur = osal_task_registry_head;
    while (cur != NULL)
    {
        if (cur->handle == handle)
        {
            if (prev != NULL)
            {
                prev->next = cur->next;
            }
            else
            {
                osal_task_registry_head = cur->next;
            }
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    osal_task_registry_unlock();

    return cur;
}

static osal_status_t osal_task_validate_attr(const osal_task_attr_t *attr)
{
    if (attr == NULL)
    {
        return OSAL_SUCCESS;
    }

    if (attr->core_affinity != OSAL_TASK_NO_AFFINITY && attr->core_affinity < 0)
    {
        return OSAL_ERR_INVALID_ARGUMENT;
    }

    if (attr->reserved[0] != 0 || attr->reserved[1] != 0 ||
        attr->reserved[2] != 0 || attr->reserved[3] != 0)
    {
        return OSAL_ERR_INVALID_ARGUMENT;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_task_attributes_init(osal_task_attr_t *attr)
{
    OSAL_CHECK_POINTER(attr);

    attr->core_affinity = OSAL_TASK_NO_AFFINITY;
    memset(attr->reserved, 0, sizeof(attr->reserved));

    return OSAL_SUCCESS;
}

osal_status_t osal_task_create(osal_task_id_t *task_id,
                               const char *task_name,
                               void (*routine)(void *),
                               void *arg,
                               osal_stackptr_t stack_pointer,
                               size_t stack_size,
                               osal_priority_t priority,
                               const osal_task_attr_t *attr)
{
    BaseType_t result;
    BaseType_t core;
    UBaseType_t stack_depth;
    TaskHandle_t handle;
    osal_status_t status;

    OSAL_CHECK_POINTER(task_id);
    OSAL_CHECK_POINTER(routine);
    OSAL_CHECK_STRING(task_name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    ARGCHECK(stack_size > 0, OSAL_ERR_INVALID_SIZE);

    status = osal_task_validate_attr(attr);
    if (status != OSAL_SUCCESS)
    {
        return status;
    }

    if (priority >= (osal_priority_t)configMAX_PRIORITIES)
    {
        return OSAL_ERR_INVALID_PRIORITY;
    }

    stack_depth = (UBaseType_t)(stack_size / sizeof(StackType_t));
    if (stack_depth == 0U)
    {
        return OSAL_ERR_INVALID_SIZE;
    }

    if (attr != NULL && attr->core_affinity != OSAL_TASK_NO_AFFINITY)
    {
        core = (BaseType_t)attr->core_affinity;
    }
    else
    {
        core = tskNO_AFFINITY;
    }

#if defined(portNUM_PROCESSORS)
    if (attr != NULL && attr->core_affinity != OSAL_TASK_NO_AFFINITY)
    {
        if (attr->core_affinity >= (int32_t)portNUM_PROCESSORS)
        {
            return OSAL_ERR_INVALID_ARGUMENT;
        }
    }
#endif

    if (stack_pointer != NULL)
    {
        struct osal_task_tcb_entry *entry;
        StaticTask_t *tcb;
        StackType_t *stack_buf;

        stack_depth = (UBaseType_t)(stack_size / sizeof(StackType_t));
        if (stack_depth == 0U)
        {
            return OSAL_ERR_INVALID_SIZE;
        }

        entry = (struct osal_task_tcb_entry *)pvPortMalloc(sizeof(*entry));
        if (entry == NULL)
        {
            return OSAL_ERROR;
        }

        tcb = (StaticTask_t *)pvPortMalloc(sizeof(StaticTask_t));
        if (tcb == NULL)
        {
            vPortFree(entry);
            return OSAL_ERROR;
        }

        stack_buf = (StackType_t *)stack_pointer;

        handle = xTaskCreateStaticPinnedToCore(routine,
                                               task_name,
                                               stack_depth,
                                               arg,
                                               (UBaseType_t)priority,
                                               stack_buf,
                                               tcb,
                                               core);
        if (handle == NULL)
        {
            vPortFree(tcb);
            vPortFree(entry);
            return OSAL_ERROR;
        }

        entry->handle = handle;
        entry->tcb = tcb;
        osal_task_registry_lock();
        entry->next = osal_task_registry_head;
        osal_task_registry_head = entry;
        osal_task_registry_unlock();
    }
    else
    {
        stack_depth = (UBaseType_t)(stack_size / sizeof(StackType_t));
        if (stack_depth == 0U)
        {
            return OSAL_ERR_INVALID_SIZE;
        }

        result = xTaskCreatePinnedToCore(routine,
                                         task_name,
                                         stack_depth,
                                         arg,
                                         (UBaseType_t)priority,
                                         &handle,
                                         core);
        if (result != pdPASS)
        {
            return OSAL_ERROR;
        }
    }

    *task_id = handle;
    return OSAL_SUCCESS;
}

osal_status_t osal_task_delete(osal_task_id_t task_id)
{
    struct osal_task_tcb_entry *entry;

    if (task_id == NULL)
    {
        return OSAL_ERR_INVALID_ID;
    }

    if (task_id == xTaskGetCurrentTaskHandle())
    {
        vTaskDelete(task_id);
        return OSAL_SUCCESS;
    }

    entry = osal_task_registry_detach(task_id);
    vTaskDelete(task_id);

    if (entry != NULL)
    {
        vPortFree(entry->tcb);
        vPortFree(entry);
    }
    return OSAL_SUCCESS;
}

osal_status_t osal_task_delay_ms(uint32_t milliseconds)
{
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
    return OSAL_SUCCESS;
}

uint32_t osal_task_get_time_ms(void)
{
    TickType_t ticks = xTaskGetTickCount();

    return (uint32_t)(ticks * portTICK_PERIOD_MS);
}
