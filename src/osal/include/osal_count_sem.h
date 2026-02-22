#ifndef OSAL_COUNT_SEM_H
#define OSAL_COUNT_SEM_H

#include <stdint.h>
#include "osal_common_type.h"
#include "osal_error.h"
#include "osal_impl_sem.h"

/**
 * @brief Create a counting semaphore
 * @param[out] sem_id         Returned semaphore ID
 * @param[in]  name           Semaphore name for debugging (optional, may be NULL)
 * @param[in]  initial_value  Initial counter value
 * @param[in]  max_value      Maximum counter value (0 = no limit / platform default)
 * @return OSAL status code
 * @retval OSAL_SUCCESS           Semaphore created successfully
 * @retval OSAL_ERROR             Creation failed
 * @retval OSAL_INVALID_POINTER   sem_id is NULL
 */
osal_status_t osal_count_sem_create(osal_count_sem_id_t *sem_id,
                                    const char *name,
                                    uint32_t initial_value,
                                    uint32_t max_value);

/**
 * @brief Delete a counting semaphore
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS         Semaphore deleted successfully
 * @retval OSAL_ERR_INVALID_ID  Invalid semaphore ID
 */
osal_status_t osal_count_sem_delete(osal_count_sem_id_t sem_id);

/**
 * @brief Give/increment a counting semaphore (task context)
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Do NOT use from ISR - use osal_count_sem_give_from_isr() instead
 */
osal_status_t osal_count_sem_give(osal_count_sem_id_t sem_id);

/**
 * @brief Take/decrement a counting semaphore (blocking, infinite wait)
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Semaphore acquired
 * @retval OSAL_SEM_FAILURE   Operation failed
 */
osal_status_t osal_count_sem_take(osal_count_sem_id_t sem_id);

/**
 * @brief Take/decrement a counting semaphore with timeout
 * @param[in] sem_id        Semaphore ID
 * @param[in] timeout_ms    Timeout in milliseconds (OSAL_MAX_DELAY for infinite)
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Semaphore acquired
 * @retval OSAL_SEM_TIMEOUT   Timeout expired
 */
osal_status_t osal_count_sem_timed_wait(osal_count_sem_id_t sem_id, uint32_t timeout_ms);

/**
 * @brief Give/increment a counting semaphore from ISR context
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_count_sem_give_from_isr(osal_count_sem_id_t sem_id);

/**
 * @brief Take/decrement a counting semaphore from ISR context
 * @param[in] sem_id  Semaphore ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_count_sem_take_from_isr(osal_count_sem_id_t sem_id);

/**
 * @brief Get the current count value of a counting semaphore
 * @param[in] sem_id  Semaphore ID
 * @return Current count value
 */
uint32_t osal_count_sem_get_count(osal_count_sem_id_t sem_id);

#endif /* OSAL_COUNT_SEM_H */
