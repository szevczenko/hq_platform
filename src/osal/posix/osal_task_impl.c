#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "osal_task.h"
#include "osal_assert.h"
#include "osal_macro.h"

struct osal_task_start
{
    void (*routine)(void *);
    void *arg;
};

static void *osal_task_entry(void *param)
{
    struct osal_task_start *start = (struct osal_task_start *)param;
    void (*routine)(void *) = start->routine;
    void *arg = start->arg;

    free(start);
    routine(arg);

    return NULL;
}

osal_status_t osal_task_attributes_init(osal_task_attr_t *attr)
{
    OSAL_CHECK_POINTER(attr);

    attr->core_affinity = OSAL_TASK_NO_AFFINITY;
    memset(attr->reserved, 0, sizeof(attr->reserved));

    return OSAL_SUCCESS;
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

static osal_status_t osal_task_validate_priority(osal_priority_t priority)
{
    int min_pri = sched_get_priority_min(SCHED_RR);
    int max_pri = sched_get_priority_max(SCHED_RR);

    if (min_pri == -1 || max_pri == -1)
    {
        return OSAL_SUCCESS;
    }

    if ((int)priority < min_pri || (int)priority > max_pri)
    {
        return OSAL_ERR_INVALID_PRIORITY;
    }

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
    pthread_attr_t thread_attr;
    struct sched_param sched_param;
    struct osal_task_start *start;
    osal_status_t status;
    int ret;

    OSAL_CHECK_POINTER(task_id);
    OSAL_CHECK_POINTER(routine);
    OSAL_CHECK_STRING(task_name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    ARGCHECK(stack_size > 0, OSAL_ERR_INVALID_SIZE);

    status = osal_task_validate_attr(attr);
    if (status != OSAL_SUCCESS)
    {
        return status;
    }

    status = osal_task_validate_priority(priority);
    if (status != OSAL_SUCCESS)
    {
        return status;
    }

#if defined(_SC_NPROCESSORS_ONLN)
    if (attr != NULL && attr->core_affinity != OSAL_TASK_NO_AFFINITY)
    {
        long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpu_count > 0 && attr->core_affinity >= cpu_count)
        {
            return OSAL_ERR_INVALID_ARGUMENT;
        }
    }
#endif

    start = (struct osal_task_start *)malloc(sizeof(*start));
    if (start == NULL)
    {
        return OSAL_ERROR;
    }

    start->routine = routine;
    start->arg = arg;

    ret = pthread_attr_init(&thread_attr);
    if (ret != 0)
    {
        free(start);
        return OSAL_ERROR;
    }

    if (stack_pointer != NULL)
    {
        ret = pthread_attr_setstack(&thread_attr, stack_pointer, stack_size);
        if (ret != 0)
        {
            pthread_attr_destroy(&thread_attr);
            free(start);
            return OSAL_ERR_INVALID_SIZE;
        }
    }
    else
    {
        ret = pthread_attr_setstacksize(&thread_attr, stack_size);
        if (ret != 0)
        {
            pthread_attr_destroy(&thread_attr);
            free(start);
            return OSAL_ERR_INVALID_SIZE;
        }
    }

    memset(&sched_param, 0, sizeof(sched_param));
    sched_param.sched_priority = (int)priority;
    (void)pthread_attr_setschedpolicy(&thread_attr, SCHED_RR);
    (void)pthread_attr_setschedparam(&thread_attr, &sched_param);

    ret = pthread_create(task_id, &thread_attr, osal_task_entry, start);
    pthread_attr_destroy(&thread_attr);

    if (ret != 0)
    {
        free(start);
        return OSAL_ERROR;
    }

#if defined(__linux__)
    if (task_name[0] != '\0')
    {
        (void)pthread_setname_np(*task_id, task_name);
    }
#elif defined(__APPLE__)
    if (task_name[0] != '\0')
    {
        (void)pthread_setname_np(task_name);
    }
#endif

#if defined(__linux__)
    if (attr != NULL && attr->core_affinity != OSAL_TASK_NO_AFFINITY)
    {
        cpu_set_t cpuset;

        CPU_ZERO(&cpuset);
        CPU_SET(attr->core_affinity, &cpuset);
        (void)pthread_setaffinity_np(*task_id, sizeof(cpuset), &cpuset);
    }
#endif

    return OSAL_SUCCESS;
}

osal_status_t osal_task_delete(osal_task_id_t task_id)
{
    int ret_cancel;
    int ret_join;

    ret_cancel = pthread_cancel(task_id);
    ret_join = pthread_join(task_id, NULL);

    if (ret_cancel != 0 || ret_join != 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_task_delay_ms(uint32_t milliseconds)
{
    struct timespec ts;
    int ret;

    ts.tv_sec = (time_t)(milliseconds / 1000U);
    ts.tv_nsec = (long)((milliseconds % 1000U) * 1000000U);

    ret = nanosleep(&ts, NULL);
    if (ret != 0)
    {
        return OSAL_ERROR;
    }

    return OSAL_SUCCESS;
}

uint32_t osal_task_get_time_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0U;
    }

    return (uint32_t)((ts.tv_sec * 1000U) + (ts.tv_nsec / 1000000U));
}
