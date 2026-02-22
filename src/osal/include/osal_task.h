#ifndef OSAL_TASK_H
#define OSAL_TASK_H

#include <stddef.h>
#include "osal_common_type.h"
#include "osal_error.h"
#include "osal_impl_task.h"

#define OSAL_TASK_NO_AFFINITY  (-1)

typedef void *osal_stackptr_t;
typedef uint32_t osal_priority_t;

typedef struct {
    int32_t core_affinity;    /**< Core to pin task to (0, 1, ...) or OSAL_TASK_NO_AFFINITY. */
    uint32_t reserved[4];     /**< Reserved for future use. Must be zero. */
} osal_task_attr_t;

/**
 * @brief Initialize task attributes with default values
 *
 * @param[out] attr  Pointer to task attributes structure
 * @return OSAL status code
 * @retval OSAL_SUCCESS           Attributes initialized
 * @retval OSAL_INVALID_POINTER   attr is NULL
 */
osal_status_t osal_task_attributes_init(osal_task_attr_t *attr);

/**
 * @brief Create and start a new task
 *
 * @param[out] task_id         Returned task ID for future operations
 * @param[in]  task_name       Human-readable task name for debugging
 * @param[in]  routine         Function pointer to task entry point
 * @param[in]  arg             Argument passed to task function (may be NULL)
 * @param[in]  stack_pointer   Pointer to pre-allocated stack memory, or NULL for dynamic allocation
 * @param[in]  stack_size      Stack size in bytes
 * @param[in]  priority        Task execution priority (higher values = higher priority)
 * @param[in]  attr            Optional task attributes, or NULL for defaults
 *
 * @return OSAL status code
 * @retval OSAL_SUCCESS            Task created successfully
 * @retval OSAL_ERROR              Task creation failed
 * @retval OSAL_INVALID_POINTER    task_id, task_name, or routine is NULL
 * @retval OSAL_ERR_NAME_TOO_LONG  task_name exceeds OSAL_MAX_NAME_LEN length
 * @retval OSAL_ERR_NO_FREE_IDS    Task table is full
 * @retval OSAL_ERR_NAME_TAKEN     Task name already in use
 * @retval OSAL_ERR_INVALID_PRIORITY Priority value out of valid range
 */
osal_status_t osal_task_create(osal_task_id_t *task_id,
                               const char *task_name,
                               void (*routine)(void *),
                               void *arg,
                               osal_stackptr_t stack_pointer,
                               size_t stack_size,
                               osal_priority_t priority,
                               const osal_task_attr_t *attr);

/**
 * @brief Delete/terminate a task
 * @param[in] task_id  ID of task to delete
 * @return OSAL status code
 * @retval OSAL_SUCCESS         Task deleted successfully
 * @retval OSAL_ERR_INVALID_ID  Invalid task ID
 */
osal_status_t osal_task_delete(osal_task_id_t task_id);

/**
 * @brief Delay/sleep the calling task for specified milliseconds
 * @param[in] milliseconds  Time to sleep in milliseconds
 * @return OSAL status code
 */
osal_status_t osal_task_delay_ms(uint32_t milliseconds);

/**
 * @brief Get current system time in milliseconds since boot
 * @return Time in milliseconds (32-bit unsigned integer)
 */
uint32_t osal_task_get_time_ms(void);

#endif /* OSAL_TASK_H */
