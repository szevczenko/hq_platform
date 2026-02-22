#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "osal_timer.h"
#include "osal_assert.h"
#include "osal_macro.h"

static void osal_timespec_add_ms(struct timespec *ts, uint32_t ms)
{
    ts->tv_sec += (time_t)(ms / 1000U);
    ts->tv_nsec += (long)((ms % 1000U) * 1000000U);
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static int osal_timer_get_deadline(struct osal_timer_internal *timer, struct timespec *ts)
{
    if (clock_gettime(timer->clock_id, ts) != 0)
    {
        return -1;
    }

    osal_timespec_add_ms(ts, timer->period_ms);
    return 0;
}

static void *osal_timer_thread(void *arg)
{
    struct osal_timer_internal *timer = (struct osal_timer_internal *)arg;

    if (pthread_mutex_lock(&timer->mutex) != 0)
    {
        return NULL;
    }

    while (!timer->stop_requested)
    {
        struct timespec deadline;

        while (!timer->active && !timer->stop_requested)
        {
            pthread_cond_wait(&timer->cond, &timer->mutex);
        }

        if (timer->stop_requested)
        {
            break;
        }

        if (osal_timer_get_deadline(timer, &deadline) != 0)
        {
            timer->active = false;
            break;
        }

        while (timer->active && !timer->stop_requested)
        {
            int ret = pthread_cond_timedwait(&timer->cond, &timer->mutex, &deadline);

            if (ret == ETIMEDOUT)
            {
                void (*callback)(osal_timer_id_t) = timer->callback;
                osal_timer_id_t id = timer;

                if (!timer->auto_reload)
                {
                    timer->active = false;
                }
                else
                {
                    if (osal_timer_get_deadline(timer, &deadline) != 0)
                    {
                        timer->active = false;
                    }
                }

                pthread_mutex_unlock(&timer->mutex);
                callback(id);
                pthread_mutex_lock(&timer->mutex);

                if (!timer->auto_reload)
                {
                    break;
                }
            }
            else if (ret != 0)
            {
                timer->active = false;
                break;
            }
            else
            {
                if (!timer->active || timer->stop_requested)
                {
                    break;
                }
                if (osal_timer_get_deadline(timer, &deadline) != 0)
                {
                    timer->active = false;
                    break;
                }
            }
        }
    }

    pthread_mutex_unlock(&timer->mutex);
    return NULL;
}

static osal_status_t osal_timer_init(struct osal_timer_internal *timer)
{
    pthread_condattr_t cond_attr;
    int ret;

    memset(timer, 0, sizeof(*timer));

    ret = pthread_mutex_init(&timer->mutex, NULL);
    if (ret != 0)
    {
        return OSAL_ERROR;
    }

#if defined(CLOCK_MONOTONIC) && defined(_POSIX_CLOCK_SELECTION) && (_POSIX_CLOCK_SELECTION > 0)
    if (pthread_condattr_init(&cond_attr) == 0)
    {
        if (pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC) == 0)
        {
            timer->clock_id = CLOCK_MONOTONIC;
            ret = pthread_cond_init(&timer->cond, &cond_attr);
        }
        else
        {
            timer->clock_id = CLOCK_REALTIME;
            ret = pthread_cond_init(&timer->cond, NULL);
        }
        pthread_condattr_destroy(&cond_attr);
    }
    else
    {
        timer->clock_id = CLOCK_REALTIME;
        ret = pthread_cond_init(&timer->cond, NULL);
    }
#else
    timer->clock_id = CLOCK_REALTIME;
    ret = pthread_cond_init(&timer->cond, NULL);
#endif

    if (ret != 0)
    {
        pthread_mutex_destroy(&timer->mutex);
        return OSAL_ERROR;
    }

    return OSAL_SUCCESS;
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
    struct osal_timer_internal *timer;
    osal_status_t status;
    int ret;

    OSAL_CHECK_POINTER(timer_id);
    OSAL_CHECK_POINTER(callback);

    if (name != NULL)
    {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }

    ARGCHECK(period_ms > 0U, OSAL_TIMER_ERR_INVALID_ARGS);

    if (stack_pointer != NULL)
    {
        ARGCHECK(stack_size >= sizeof(struct osal_timer_internal), OSAL_ERR_INVALID_SIZE);
        timer = (struct osal_timer_internal *)stack_pointer;
    }
    else
    {
        timer = (struct osal_timer_internal *)malloc(sizeof(*timer));
        if (timer == NULL)
        {
            return OSAL_ERROR;
        }
    }

    status = osal_timer_init(timer);
    if (status != OSAL_SUCCESS)
    {
        if (stack_pointer == NULL)
        {
            free(timer);
        }
        return status;
    }

    timer->period_ms = period_ms;
    timer->auto_reload = auto_reload;
    timer->callback = callback;
    timer->context = callback_arg;
    timer->use_static = (stack_pointer != NULL);

    if (name != NULL)
    {
        strncpy(timer->name, name, OSAL_MAX_NAME_LEN - 1U);
    }

    ret = pthread_create(&timer->thread, NULL, osal_timer_thread, timer);
    if (ret != 0)
    {
        pthread_cond_destroy(&timer->cond);
        pthread_mutex_destroy(&timer->mutex);
        if (stack_pointer == NULL)
        {
            free(timer);
        }
        return OSAL_ERROR;
    }

    *timer_id = timer;
    return OSAL_SUCCESS;
}

osal_status_t osal_timer_start(osal_timer_id_t timer_id, uint32_t timeout_ms)
{
    struct osal_timer_internal *timer = timer_id;

    (void)timeout_ms;
    OSAL_CHECK_POINTER(timer);

    if (pthread_mutex_lock(&timer->mutex) != 0)
    {
        return OSAL_ERROR;
    }

    timer->active = true;
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->mutex);

    return OSAL_SUCCESS;
}

osal_status_t osal_timer_reset(osal_timer_id_t timer_id, uint32_t timeout_ms)
{
    struct osal_timer_internal *timer = timer_id;

    (void)timeout_ms;
    OSAL_CHECK_POINTER(timer);

    if (pthread_mutex_lock(&timer->mutex) != 0)
    {
        return OSAL_ERROR;
    }

    timer->active = true;
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->mutex);

    return OSAL_SUCCESS;
}

osal_status_t osal_timer_stop(osal_timer_id_t timer_id, uint32_t timeout_ms)
{
    struct osal_timer_internal *timer = timer_id;

    (void)timeout_ms;
    OSAL_CHECK_POINTER(timer);

    if (pthread_mutex_lock(&timer->mutex) != 0)
    {
        return OSAL_ERROR;
    }

    timer->active = false;
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->mutex);

    return OSAL_SUCCESS;
}

osal_status_t osal_timer_delete(osal_timer_id_t timer_id, uint32_t timeout_ms)
{
    struct osal_timer_internal *timer = timer_id;

    (void)timeout_ms;
    OSAL_CHECK_POINTER(timer);

    if (pthread_mutex_lock(&timer->mutex) != 0)
    {
        return OSAL_ERROR;
    }

    timer->stop_requested = true;
    timer->active = false;
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->mutex);

    if (pthread_join(timer->thread, NULL) != 0)
    {
        return OSAL_ERROR;
    }

    pthread_cond_destroy(&timer->cond);
    pthread_mutex_destroy(&timer->mutex);

    if (!timer->use_static)
    {
        free(timer);
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_timer_change_period(osal_timer_id_t timer_id,
                                       uint32_t new_period_ms,
                                       uint32_t timeout_ms)
{
    struct osal_timer_internal *timer = timer_id;

    (void)timeout_ms;
    OSAL_CHECK_POINTER(timer);
    ARGCHECK(new_period_ms > 0U, OSAL_TIMER_ERR_INVALID_ARGS);

    if (pthread_mutex_lock(&timer->mutex) != 0)
    {
        return OSAL_ERROR;
    }

    timer->period_ms = new_period_ms;
    timer->active = true;
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->mutex);

    return OSAL_SUCCESS;
}

bool osal_timer_is_active(osal_timer_id_t timer_id)
{
    struct osal_timer_internal *timer = timer_id;
    bool active = false;

    if (timer == NULL)
    {
        return false;
    }

    if (pthread_mutex_lock(&timer->mutex) != 0)
    {
        return false;
    }

    active = timer->active;
    pthread_mutex_unlock(&timer->mutex);

    return active;
}

osal_status_t osal_timer_start_from_isr(osal_timer_id_t timer_id)
{
    (void)timer_id;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_timer_stop_from_isr(osal_timer_id_t timer_id)
{
    (void)timer_id;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_timer_reset_from_isr(osal_timer_id_t timer_id)
{
    (void)timer_id;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

void *osal_timer_get_context(osal_timer_id_t timer_id)
{
    struct osal_timer_internal *timer = timer_id;
    void *context;

    if (timer == NULL)
    {
        return NULL;
    }

    if (pthread_mutex_lock(&timer->mutex) != 0)
    {
        return NULL;
    }

    context = timer->context;
    pthread_mutex_unlock(&timer->mutex);

    return context;
}

osal_status_t osal_timer_set_context(osal_timer_id_t timer_id, void *context)
{
    struct osal_timer_internal *timer = timer_id;

    OSAL_CHECK_POINTER(timer);

    if (pthread_mutex_lock(&timer->mutex) != 0)
    {
        return OSAL_ERROR;
    }

    timer->context = context;
    pthread_mutex_unlock(&timer->mutex);

    return OSAL_SUCCESS;
}
