#ifndef OSAL_BIN_SEM_H
#define OSAL_BIN_SEM_H

#include <stdint.h>
#include "osal_common_type.h"
#include "osal_error.h"
#include "osal_impl_sem.h"

/**
 * @brief Create a binary semaphore
 * @param[out] sem_id        Returned semaphore ID
 * @param[in]  name          Semaphore name for debugging (optional, may be NULL)
 * @param[in]  initial_value Initial value: OSAL_SEM_EMPTY (0) or OSAL_SEM_FULL (1)
 * @return OSAL status code
 * @retval OSAL_SUCCESS           Semaphore created successfully
 * @retval OSAL_ERROR             Creation failed
 * @retval OSAL_INVALID_POINTER   sem_id is NULL
 * @retval OSAL_INVALID_SEM_VALUE Invalid initial_value (not 0 or 1)
 */
osal_status_t osal_bin_sem_create(osal_bin_sem_id_t *sem_id,
                                  const char *name,
                                  uint32_t initial_value);

/**
 * @brief Delete a binary semaphore
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS         Semaphore deleted successfully
 * @retval OSAL_ERR_INVALID_ID  Invalid semaphore ID
 */
osal_status_t osal_bin_sem_delete(osal_bin_sem_id_t sem_id);

/**
 * @brief Give/release a binary semaphore (task context)
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Do NOT use from ISR - use osal_bin_sem_give_from_isr() instead
 */
osal_status_t osal_bin_sem_give(osal_bin_sem_id_t sem_id);

/**
 * @brief Take/acquire a binary semaphore (blocking, infinite wait)
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Semaphore acquired
 * @retval OSAL_SEM_FAILURE   Operation failed
 */
osal_status_t osal_bin_sem_take(osal_bin_sem_id_t sem_id);

/**
 * @brief Take/acquire a binary semaphore with timeout
 * @param[in] sem_id        Semaphore ID
 * @param[in] timeout_ms    Timeout in milliseconds (OSAL_MAX_DELAY for infinite)
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Semaphore acquired
 * @retval OSAL_SEM_TIMEOUT   Timeout expired
 */
osal_status_t osal_bin_sem_timed_wait(osal_bin_sem_id_t sem_id, uint32_t timeout_ms);

/**
 * @brief Give/release a binary semaphore from ISR context
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_bin_sem_give_from_isr(osal_bin_sem_id_t sem_id);

/**
 * @brief Take/acquire a binary semaphore from ISR context
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_bin_sem_take_from_isr(osal_bin_sem_id_t sem_id);

#endif /* OSAL_BIN_SEM_H */
