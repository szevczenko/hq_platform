#ifndef OSAL_TIMER_H
#define OSAL_TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "osal_common_type.h"
#include "osal_error.h"
#include "osal_impl_timer.h"
#include "osal_task.h"

/**
 * @brief Create a software timer
 * @param[out] timer_id       Returned timer ID
 * @param[in]  name           Timer name for debugging (optional)
 * @param[in]  period_ms      Timer period in milliseconds
 * @param[in]  auto_reload    true for auto-reload, false for one-shot
 * @param[in]  callback       Function to call when timer expires
 * @param[in]  callback_arg   Argument passed to callback function
 * @param[in]  stack_pointer  Pointer to pre-allocated timer buffer, or NULL for dynamic allocation
 * @param[in]  stack_size     Size of the pre-allocated buffer in bytes (ignored when stack_pointer is NULL)
 * @return OSAL status code
 *
 * @note If stack_pointer is NULL, the system allocates the timer control block dynamically.
 * @note If stack_pointer is non-NULL, the buffer must be large enough to hold the
 *       platform-specific timer structure (e.g., StaticTimer_t on FreeRTOS).
 */
osal_status_t osal_timer_create(osal_timer_id_t *timer_id,
                                const char *name,
                                uint32_t period_ms,
                                bool auto_reload,
                                void (*callback)(osal_timer_id_t),
                                void *callback_arg,
                                osal_stackptr_t stack_pointer,
                                size_t stack_size);

/**
 * @brief Start a timer (task context)
 * @param[in] timer_id     Timer ID
 * @param[in] timeout_ms   Max time to wait if command queue is full
 * @return OSAL status code
 * @note Transitions a dormant timer to the active state.
 *       To restart an already-active timer, use osal_timer_reset().
 */
osal_status_t osal_timer_start(osal_timer_id_t timer_id, uint32_t timeout_ms);

/**
 * @brief Reset a running timer (task context)
 *
 * Resets the timer's expiry time so it is recalculated relative to
 * when osal_timer_reset() is called, not when the timer was originally
 * started. If the timer is dormant (not running), this function starts it.
 *
 * @param[in] timer_id     Timer ID
 * @param[in] timeout_ms   Max time to wait if command queue is full
 * @return OSAL status code
 *
 * @note FreeRTOS: Maps to xTimerReset()
 */
osal_status_t osal_timer_reset(osal_timer_id_t timer_id, uint32_t timeout_ms);

/**
 * @brief Stop a timer (task context)
 * @param[in] timer_id     Timer ID
 * @param[in] timeout_ms   Max time to wait if command queue is full
 * @return OSAL status code
 */
osal_status_t osal_timer_stop(osal_timer_id_t timer_id, uint32_t timeout_ms);

/**
 * @brief Delete/destroy a timer
 * @param[in] timer_id     Timer ID
 * @param[in] timeout_ms   Max time to wait if command queue is full
 * @return OSAL status code
 */
osal_status_t osal_timer_delete(osal_timer_id_t timer_id, uint32_t timeout_ms);

/**
 * @brief Change timer period (task context)
 * @param[in] timer_id      Timer ID
 * @param[in] new_period_ms New period in milliseconds
 * @param[in] timeout_ms    Max time to wait if command queue is full
 * @return OSAL status code
 * @note This also starts the timer if it's dormant
 */
osal_status_t osal_timer_change_period(osal_timer_id_t timer_id,
                                       uint32_t new_period_ms,
                                       uint32_t timeout_ms);

/**
 * @brief Check if timer is active
 * @param[in] timer_id  Timer ID
 * @return true if timer is active, false if dormant
 */
bool osal_timer_is_active(osal_timer_id_t timer_id);

/**
 * @brief Start a timer from ISR context
 * @param[in] timer_id  Timer ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_timer_start_from_isr(osal_timer_id_t timer_id);

/**
 * @brief Stop a timer from ISR context
 * @param[in] timer_id  Timer ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_timer_stop_from_isr(osal_timer_id_t timer_id);

/**
 * @brief Reset a running timer from ISR context
 *
 * ISR-safe variant of osal_timer_reset(). Recalculates the timer's
 * expiry time relative to now. If the timer is dormant, starts it.
 *
 * @param[in] timer_id  Timer ID
 * @return OSAL status code
 * @note Platform-specific ISR context switching is handled internally
 * @note FreeRTOS: Maps to xTimerResetFromISR()
 */
osal_status_t osal_timer_reset_from_isr(osal_timer_id_t timer_id);

/**
 * @brief Get the user context associated with a timer
 *
 * @param[in] timer_id  Timer ID
 * @return Pointer to user context, or NULL if none was set
 */
void *osal_timer_get_context(osal_timer_id_t timer_id);

/**
 * @brief Set or update the user context associated with a timer
 *
 * @param[in] timer_id  Timer ID
 * @param[in] context   Pointer to user data (may be NULL to clear)
 * @return OSAL status code
 * @retval OSAL_SUCCESS         Context set successfully
 * @retval OSAL_ERR_INVALID_ID  Invalid timer ID
 */
osal_status_t osal_timer_set_context(osal_timer_id_t timer_id, void *context);

#endif /* OSAL_TIMER_H */
