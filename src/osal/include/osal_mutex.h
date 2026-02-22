#ifndef OSAL_MUTEX_H
#define OSAL_MUTEX_H

#include "osal_common_type.h"
#include "osal_error.h"
#include "osal_impl_sem.h"

/**
 * @brief Create a mutex
 * @param[out] mutex_id  Returned mutex ID
 * @param[in]  name      Mutex name for debugging (optional, may be NULL)
 * @return OSAL status code
 * @retval OSAL_SUCCESS           Mutex created successfully
 * @retval OSAL_ERROR             Creation failed
 * @retval OSAL_INVALID_POINTER   mutex_id is NULL
 */
osal_status_t osal_mutex_create(osal_mutex_id_t *mutex_id,
                                const char *name);

/**
 * @brief Delete a mutex
 * @param[in] mutex_id  Mutex ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS         Mutex deleted successfully
 * @retval OSAL_ERR_INVALID_ID  Invalid mutex ID
 */
osal_status_t osal_mutex_delete(osal_mutex_id_t mutex_id);

/**
 * @brief Take/lock a mutex (blocking, infinite wait)
 * @param[in] mutex_id  Mutex ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Mutex acquired
 * @retval OSAL_SEM_FAILURE   Operation failed
 * @note Must NOT be called from ISR context
 * @note Only the task that took the mutex may give it back
 */
osal_status_t osal_mutex_take(osal_mutex_id_t mutex_id);

/**
 * @brief Give/unlock a mutex
 * @param[in] mutex_id  Mutex ID
 * @return OSAL status code
 * @note Must NOT be called from ISR context
 * @note Must be called by the same task that took the mutex
 */
osal_status_t osal_mutex_give(osal_mutex_id_t mutex_id);

#endif /* OSAL_MUTEX_H */
