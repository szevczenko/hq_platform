#include <errno.h>
#include <semaphore.h>
#include <stdlib.h>
#include <time.h>

#include "osal_count_sem.h"
#include "osal_assert.h"
#include "osal_macro.h"

static osal_status_t osal_sem_wait_timed(sem_t *sem, uint32_t timeout_ms)
{
    if (timeout_ms == 0U)
    {
        if (sem_trywait(sem) == 0)
        {
            return OSAL_SUCCESS;
        }

        if (errno == EAGAIN)
        {
            return OSAL_SEM_TIMEOUT;
        }

        return OSAL_SEM_FAILURE;
    }

    if (timeout_ms == OSAL_MAX_DELAY)
    {
        if (sem_wait(sem) == 0)
        {
            return OSAL_SUCCESS;
        }

        return OSAL_SEM_FAILURE;
    }

#if defined(_POSIX_TIMERS) && (_POSIX_TIMERS > 0)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(timeout_ms / 1000U);
        ts.tv_nsec += (long)((timeout_ms % 1000U) * 1000000U);
        if (ts.tv_nsec >= 1000000000L)
        {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        while (sem_timedwait(sem, &ts) != 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == ETIMEDOUT)
            {
                return OSAL_SEM_TIMEOUT;
            }
            return OSAL_SEM_FAILURE;
        }

        return OSAL_SUCCESS;
    }
#else
    (void)timeout_ms;
    if (sem_wait(sem) == 0)
    {
        return OSAL_SUCCESS;
    }

    return OSAL_SEM_FAILURE;
#endif
}

osal_status_t osal_count_sem_create(osal_count_sem_id_t *sem_id,
                                    const char *name,
                                    uint32_t initial_value,
                                    uint32_t max_value)
{
    sem_t *sem;

    OSAL_CHECK_POINTER(sem_id);

    if (name != NULL)
    {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }

    if (max_value != 0U && initial_value > max_value)
    {
        return OSAL_INVALID_SEM_VALUE;
    }

    sem = (sem_t *)malloc(sizeof(*sem));
    if (sem == NULL)
    {
        return OSAL_ERROR;
    }

    if (sem_init(sem, 0, initial_value) != 0)
    {
        free(sem);
        return OSAL_ERROR;
    }

    *sem_id = sem;
    return OSAL_SUCCESS;
}

osal_status_t osal_count_sem_delete(osal_count_sem_id_t sem_id)
{
    OSAL_CHECK_POINTER(sem_id);

    if (sem_destroy(sem_id) != 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    free(sem_id);
    return OSAL_SUCCESS;
}

osal_status_t osal_count_sem_give(osal_count_sem_id_t sem_id)
{
    OSAL_CHECK_POINTER(sem_id);

    if (sem_post(sem_id) != 0)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_count_sem_take(osal_count_sem_id_t sem_id)
{
    OSAL_CHECK_POINTER(sem_id);

    if (sem_wait(sem_id) != 0)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_count_sem_timed_wait(osal_count_sem_id_t sem_id, uint32_t timeout_ms)
{
    OSAL_CHECK_POINTER(sem_id);

    return osal_sem_wait_timed(sem_id, timeout_ms);
}

osal_status_t osal_count_sem_give_from_isr(osal_count_sem_id_t sem_id)
{
    (void)sem_id;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_count_sem_take_from_isr(osal_count_sem_id_t sem_id)
{
    (void)sem_id;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

uint32_t osal_count_sem_get_count(osal_count_sem_id_t sem_id)
{
    int value;

    OSAL_CHECK_POINTER(sem_id);

    if (sem_getvalue(sem_id, &value) != 0)
    {
        return 0U;
    }

    if (value < 0)
    {
        return 0U;
    }

    return (uint32_t)value;
}
