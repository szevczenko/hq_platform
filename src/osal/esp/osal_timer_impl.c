#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "osal_timer.h"
#include "osal_assert.h"
#include "osal_macro.h"

struct osal_timer_meta
{
    void (*callback)(osal_timer_id_t);
    void *context;
    bool free_meta;
};

static TickType_t osal_timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == OSAL_MAX_DELAY)
    {
        return portMAX_DELAY;
    }

    return pdMS_TO_TICKS(timeout_ms);
}

static TickType_t osal_period_to_ticks(uint32_t period_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(period_ms);

    if (ticks == 0)
    {
        ticks = 1;
    }

    return ticks;
}

static void osal_timer_callback(TimerHandle_t handle)
{
    struct osal_timer_meta *meta = (struct osal_timer_meta *)pvTimerGetTimerID(handle);

    if (meta != NULL && meta->callback != NULL)
    {
        meta->callback((osal_timer_id_t)handle);
    }
}

static struct osal_timer_meta *osal_timer_meta_create(osal_stackptr_t stack_pointer,
                                                      size_t stack_size)
{
    struct osal_timer_meta *meta = NULL;

    if (stack_pointer != NULL)
    {
        uint8_t *base = (uint8_t *)stack_pointer;
        uintptr_t addr = (uintptr_t)(base + OSAL_TIMER_STATIC_SIZE);
        uintptr_t align = sizeof(void *);
        uintptr_t end = (uintptr_t)base + stack_size;

        addr = (addr + (align - 1U)) & ~(align - 1U);
        if (addr + sizeof(*meta) <= end)
        {
            meta = (struct osal_timer_meta *)addr;
            memset(meta, 0, sizeof(*meta));
            meta->free_meta = false;
            return meta;
        }
    }

    meta = (struct osal_timer_meta *)malloc(sizeof(*meta));
    if (meta == NULL)
    {
        return NULL;
    }

    memset(meta, 0, sizeof(*meta));
    meta->free_meta = true;
    return meta;
}

osal_status_t osal_timer_create(osal_timer_id_t *timer_id,
                                const char *name,
                                uint32_t period_ms,
                                bool auto_reload,
                                void (*callback)(osal_timer_id_t),
                                void *callback_arg,
                                osal_stackptr_t stack_pointer,
                                size_t stack_size)
{
    TimerHandle_t timer;
    StaticTimer_t *static_timer = NULL;
    struct osal_timer_meta *meta;
    TickType_t period_ticks;

    OSAL_CHECK_POINTER(timer_id);
    OSAL_CHECK_POINTER(callback);

    if (name != NULL)
    {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }

    ARGCHECK(period_ms > 0U, OSAL_TIMER_ERR_INVALID_ARGS);

    if (stack_pointer != NULL)
    {
        ARGCHECK(stack_size >= OSAL_TIMER_STATIC_SIZE, OSAL_ERR_INVALID_SIZE);
        static_timer = (StaticTimer_t *)stack_pointer;
    }

    meta = osal_timer_meta_create(stack_pointer, stack_size);
    if (meta == NULL)
    {
        return OSAL_ERROR;
    }

    meta->callback = callback;
    meta->context = callback_arg;

    period_ticks = osal_period_to_ticks(period_ms);

    if (static_timer != NULL)
    {
        timer = xTimerCreateStatic((name != NULL) ? name : "",
                                   period_ticks,
                                   auto_reload ? pdTRUE : pdFALSE,
                                   meta,
                                   osal_timer_callback,
                                   static_timer);
    }
    else
    {
        timer = xTimerCreate((name != NULL) ? name : "",
                             period_ticks,
                             auto_reload ? pdTRUE : pdFALSE,
                             meta,
                             osal_timer_callback);
    }

    if (timer == NULL)
    {
        if (meta->free_meta)
        {
            free(meta);
        }
        return OSAL_ERROR;
    }

    *timer_id = (osal_timer_id_t)timer;
    return OSAL_SUCCESS;
}

osal_status_t osal_timer_start(osal_timer_id_t timer_id, uint32_t timeout_ms)
{
    TickType_t ticks;

    OSAL_CHECK_POINTER(timer_id);

    ticks = osal_timeout_to_ticks(timeout_ms);
    if (xTimerStart((TimerHandle_t)timer_id, ticks) != pdPASS)
    {
        return OSAL_TIMER_ERR_INTERNAL;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_timer_reset(osal_timer_id_t timer_id, uint32_t timeout_ms)
{
    TickType_t ticks;

    OSAL_CHECK_POINTER(timer_id);

    ticks = osal_timeout_to_ticks(timeout_ms);
    if (xTimerReset((TimerHandle_t)timer_id, ticks) != pdPASS)
    {
        return OSAL_TIMER_ERR_INTERNAL;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_timer_stop(osal_timer_id_t timer_id, uint32_t timeout_ms)
{
    TickType_t ticks;

    OSAL_CHECK_POINTER(timer_id);

    ticks = osal_timeout_to_ticks(timeout_ms);
    if (xTimerStop((TimerHandle_t)timer_id, ticks) != pdPASS)
    {
        return OSAL_TIMER_ERR_INTERNAL;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_timer_delete(osal_timer_id_t timer_id, uint32_t timeout_ms)
{
    TickType_t ticks;
    struct osal_timer_meta *meta;

    OSAL_CHECK_POINTER(timer_id);

    meta = (struct osal_timer_meta *)pvTimerGetTimerID((TimerHandle_t)timer_id);
    ticks = osal_timeout_to_ticks(timeout_ms);

    if (xTimerDelete((TimerHandle_t)timer_id, ticks) != pdPASS)
    {
        return OSAL_TIMER_ERR_INTERNAL;
    }

    if (meta != NULL && meta->free_meta)
    {
        free(meta);
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_timer_change_period(osal_timer_id_t timer_id,
                                       uint32_t new_period_ms,
                                       uint32_t timeout_ms)
{
    TickType_t ticks;
    TickType_t new_ticks;

    OSAL_CHECK_POINTER(timer_id);
    ARGCHECK(new_period_ms > 0U, OSAL_TIMER_ERR_INVALID_ARGS);

    new_ticks = osal_period_to_ticks(new_period_ms);
    ticks = osal_timeout_to_ticks(timeout_ms);

    if (xTimerChangePeriod((TimerHandle_t)timer_id, new_ticks, ticks) != pdPASS)
    {
        return OSAL_TIMER_ERR_INTERNAL;
    }

    return OSAL_SUCCESS;
}

bool osal_timer_is_active(osal_timer_id_t timer_id)
{
    if (timer_id == NULL)
    {
        return false;
    }

    return (xTimerIsTimerActive((TimerHandle_t)timer_id) != pdFALSE);
}

osal_status_t osal_timer_start_from_isr(osal_timer_id_t timer_id)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    OSAL_CHECK_POINTER(timer_id);

    if (xTimerStartFromISR((TimerHandle_t)timer_id, &higher_priority_task_woken) != pdPASS)
    {
        return OSAL_TIMER_ERR_INTERNAL;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
    return OSAL_SUCCESS;
}

osal_status_t osal_timer_stop_from_isr(osal_timer_id_t timer_id)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    OSAL_CHECK_POINTER(timer_id);

    if (xTimerStopFromISR((TimerHandle_t)timer_id, &higher_priority_task_woken) != pdPASS)
    {
        return OSAL_TIMER_ERR_INTERNAL;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
    return OSAL_SUCCESS;
}

osal_status_t osal_timer_reset_from_isr(osal_timer_id_t timer_id)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    OSAL_CHECK_POINTER(timer_id);

    if (xTimerResetFromISR((TimerHandle_t)timer_id, &higher_priority_task_woken) != pdPASS)
    {
        return OSAL_TIMER_ERR_INTERNAL;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
    return OSAL_SUCCESS;
}

void *osal_timer_get_context(osal_timer_id_t timer_id)
{
    struct osal_timer_meta *meta;

    if (timer_id == NULL)
    {
        return NULL;
    }

    meta = (struct osal_timer_meta *)pvTimerGetTimerID((TimerHandle_t)timer_id);
    if (meta == NULL)
    {
        return NULL;
    }

    return meta->context;
}

osal_status_t osal_timer_set_context(osal_timer_id_t timer_id, void *context)
{
    struct osal_timer_meta *meta;

    OSAL_CHECK_POINTER(timer_id);

    meta = (struct osal_timer_meta *)pvTimerGetTimerID((TimerHandle_t)timer_id);
    if (meta == NULL)
    {
        return OSAL_ERR_INVALID_ID;
    }

    meta->context = context;
    return OSAL_SUCCESS;
}
