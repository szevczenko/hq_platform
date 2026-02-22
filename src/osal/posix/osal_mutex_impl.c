#define _GNU_SOURCE

#include <pthread.h>
#include <stdlib.h>

#include "osal_mutex.h"
#include "osal_assert.h"
#include "osal_macro.h"

osal_status_t osal_mutex_create(osal_mutex_id_t *mutex_id, const char *name)
{
    pthread_mutex_t *mutex;

    OSAL_CHECK_POINTER(mutex_id);

    if (name != NULL)
    {
        OSAL_CHECK_STRING(name, OSAL_MAX_NAME_LEN, OSAL_ERR_NAME_TOO_LONG);
    }

    mutex = (pthread_mutex_t *)malloc(sizeof(*mutex));
    if (mutex == NULL)
    {
        return OSAL_ERROR;
    }

    if (pthread_mutex_init(mutex, NULL) != 0)
    {
        free(mutex);
        return OSAL_ERROR;
    }

    *mutex_id = mutex;
    return OSAL_SUCCESS;
}

osal_status_t osal_mutex_delete(osal_mutex_id_t mutex_id)
{
    OSAL_CHECK_POINTER(mutex_id);

    if (pthread_mutex_destroy(mutex_id) != 0)
    {
        return OSAL_ERR_INVALID_ID;
    }

    free(mutex_id);
    return OSAL_SUCCESS;
}

osal_status_t osal_mutex_take(osal_mutex_id_t mutex_id)
{
    OSAL_CHECK_POINTER(mutex_id);

    if (pthread_mutex_lock(mutex_id) != 0)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}

osal_status_t osal_mutex_give(osal_mutex_id_t mutex_id)
{
    OSAL_CHECK_POINTER(mutex_id);

    if (pthread_mutex_unlock(mutex_id) != 0)
    {
        return OSAL_SEM_FAILURE;
    }

    return OSAL_SUCCESS;
}
