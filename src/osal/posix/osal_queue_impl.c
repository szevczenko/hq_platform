#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "osal_queue.h"
#include "osal_assert.h"
#include "osal_macro.h"

struct osal_queue_internal
{
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t *buffer;
    size_t item_size;
    uint32_t max_items;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    char name[OSAL_MAX_NAME_LEN];
};

static void osal_timespec_add_ms(struct timespec *ts, uint32_t timeout_ms)
{
    ts->tv_sec += (time_t)(timeout_ms / 1000U);
    ts->tv_nsec += (long)((timeout_ms % 1000U) * 1000000U);
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static int osal_get_deadline(struct timespec *ts, uint32_t timeout_ms)
{
#if defined(CLOCK_REALTIME)
    if (clock_gettime(CLOCK_REALTIME, ts) != 0)
    {
        return -1;
    }
#else
    ts->tv_sec = time(NULL);
    ts->tv_nsec = 0;
#endif

    osal_timespec_add_ms(ts, timeout_ms);
    return 0;
}

osal_status_t osal_queue_create(osal_queue_id_t *queue_id,
                                const char *name,
                                uint32_t max_items,
                                uint32_t item_size)
{
    struct osal_queue_internal *queue;
    size_t buffer_size;

    OSAL_CHECK_POINTER(queue_id);

    if (name != NULL)
    {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }

    ARGCHECK(max_items > 0U, OSAL_QUEUE_INVALID_SIZE);
    ARGCHECK(item_size > 0U, OSAL_QUEUE_INVALID_SIZE);

    if ((size_t)max_items > (SIZE_MAX / (size_t)item_size))
    {
        return OSAL_QUEUE_INVALID_SIZE;
    }

    buffer_size = (size_t)max_items * (size_t)item_size;

    queue = (struct osal_queue_internal *)malloc(sizeof(*queue));
    if (queue == NULL)
    {
        return OSAL_ERROR;
    }

    memset(queue, 0, sizeof(*queue));
    queue->item_size = (size_t)item_size;
    queue->max_items = max_items;

    if (name != NULL)
    {
        strncpy(queue->name, name, OSAL_MAX_NAME_LEN - 1U);
    }

    queue->buffer = (uint8_t *)malloc(buffer_size);
    if (queue->buffer == NULL)
    {
        free(queue);
        return OSAL_ERROR;
    }

    if (pthread_mutex_init(&queue->mutex, NULL) != 0)
    {
        free(queue->buffer);
        free(queue);
        return OSAL_ERROR;
    }

    if (pthread_cond_init(&queue->not_empty, NULL) != 0)
    {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->buffer);
        free(queue);
        return OSAL_ERROR;
    }

    if (pthread_cond_init(&queue->not_full, NULL) != 0)
    {
        pthread_cond_destroy(&queue->not_empty);
        pthread_mutex_destroy(&queue->mutex);
        free(queue->buffer);
        free(queue);
        return OSAL_ERROR;
    }

    *queue_id = queue;
    return OSAL_SUCCESS;
}

osal_status_t osal_queue_send(osal_queue_id_t queue_id,
                              const void *item,
                              uint32_t timeout_ms)
{
    struct osal_queue_internal *queue = queue_id;
    struct timespec deadline;
    int ret;

    OSAL_CHECK_POINTER(queue);
    OSAL_CHECK_POINTER(item);

    if (timeout_ms != 0U && timeout_ms != OSAL_MAX_DELAY)
    {
        if (osal_get_deadline(&deadline, timeout_ms) != 0)
        {
            return OSAL_ERROR;
        }
    }

    if (pthread_mutex_lock(&queue->mutex) != 0)
    {
        return OSAL_ERROR;
    }

    while (queue->count == queue->max_items)
    {
        if (timeout_ms == 0U)
        {
            pthread_mutex_unlock(&queue->mutex);
            return OSAL_QUEUE_FULL;
        }

        if (timeout_ms == OSAL_MAX_DELAY)
        {
            ret = pthread_cond_wait(&queue->not_full, &queue->mutex);
        }
        else
        {
            ret = pthread_cond_timedwait(&queue->not_full, &queue->mutex, &deadline);
        }

        if (ret == ETIMEDOUT)
        {
            pthread_mutex_unlock(&queue->mutex);
            return OSAL_QUEUE_TIMEOUT;
        }

        if (ret != 0)
        {
            pthread_mutex_unlock(&queue->mutex);
            return OSAL_ERROR;
        }
    }

    memcpy(queue->buffer + (queue->tail * queue->item_size), item, queue->item_size);
    queue->tail = (queue->tail + 1U) % queue->max_items;
    queue->count++;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);

    return OSAL_SUCCESS;
}

osal_status_t osal_queue_receive(osal_queue_id_t queue_id,
                                 void *buffer,
                                 uint32_t timeout_ms)
{
    struct osal_queue_internal *queue = queue_id;
    struct timespec deadline;
    int ret;

    OSAL_CHECK_POINTER(queue);
    OSAL_CHECK_POINTER(buffer);

    if (timeout_ms != 0U && timeout_ms != OSAL_MAX_DELAY)
    {
        if (osal_get_deadline(&deadline, timeout_ms) != 0)
        {
            return OSAL_ERROR;
        }
    }

    if (pthread_mutex_lock(&queue->mutex) != 0)
    {
        return OSAL_ERROR;
    }

    while (queue->count == 0U)
    {
        if (timeout_ms == 0U)
        {
            pthread_mutex_unlock(&queue->mutex);
            return OSAL_QUEUE_EMPTY;
        }

        if (timeout_ms == OSAL_MAX_DELAY)
        {
            ret = pthread_cond_wait(&queue->not_empty, &queue->mutex);
        }
        else
        {
            ret = pthread_cond_timedwait(&queue->not_empty, &queue->mutex, &deadline);
        }

        if (ret == ETIMEDOUT)
        {
            pthread_mutex_unlock(&queue->mutex);
            return OSAL_QUEUE_TIMEOUT;
        }

        if (ret != 0)
        {
            pthread_mutex_unlock(&queue->mutex);
            return OSAL_ERROR;
        }
    }

    memcpy(buffer, queue->buffer + (queue->head * queue->item_size), queue->item_size);
    queue->head = (queue->head + 1U) % queue->max_items;
    queue->count--;

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

    return OSAL_SUCCESS;
}

osal_status_t osal_queue_delete(osal_queue_id_t queue_id)
{
    struct osal_queue_internal *queue = queue_id;

    OSAL_CHECK_POINTER(queue);

    if (pthread_cond_destroy(&queue->not_empty) != 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    if (pthread_cond_destroy(&queue->not_full) != 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    if (pthread_mutex_destroy(&queue->mutex) != 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    free(queue->buffer);
    free(queue);

    return OSAL_SUCCESS;
}

uint32_t osal_queue_get_count(osal_queue_id_t queue_id)
{
    struct osal_queue_internal *queue = queue_id;
    uint32_t count;

    OSAL_CHECK_POINTER(queue);

    if (pthread_mutex_lock(&queue->mutex) != 0)
    {
        return 0U;
    }

    count = queue->count;
    pthread_mutex_unlock(&queue->mutex);

    return count;
}

osal_status_t osal_queue_send_from_isr(osal_queue_id_t queue_id, const void *item)
{
    (void)queue_id;
    (void)item;
    return OSAL_ERR_NOT_IMPLEMENTED;
}

osal_status_t osal_queue_receive_from_isr(osal_queue_id_t queue_id, void *buffer)
{
    (void)queue_id;
    (void)buffer;
    return OSAL_ERR_NOT_IMPLEMENTED;
}
