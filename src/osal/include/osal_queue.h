#ifndef OSAL_QUEUE_H
#define OSAL_QUEUE_H

#include <stdint.h>
#include "osal_common_type.h"
#include "osal_error.h"
#include "osal_impl_queue.h"

/**
 * @brief Create a message queue
 * @param[out] queue_id    Returned queue ID
 * @param[in]  name        Queue name for debugging (optional, may be NULL)
 * @param[in]  max_items   Maximum number of items queue can hold
 * @param[in]  item_size   Size of each item in bytes
 * @return OSAL status code
 * @retval OSAL_SUCCESS            Queue created successfully
 * @retval OSAL_ERROR              Creation failed
 * @retval OSAL_INVALID_POINTER    queue_id is NULL
 * @retval OSAL_ERR_NAME_TOO_LONG  name exceeds OSAL_MAX_NAME_LEN
 * @retval OSAL_QUEUE_INVALID_SIZE Invalid max_items or item_size
 */
osal_status_t osal_queue_create(osal_queue_id_t *queue_id,
                                const char *name,
                                uint32_t max_items,
                                uint32_t item_size);

/**
 * @brief Send/post an item to queue (task context)
 * @param[in] queue_id     Queue ID
 * @param[in] item         Pointer to item to copy into queue
 * @param[in] timeout_ms   Timeout in milliseconds
 * @return OSAL status code
 * @retval OSAL_SUCCESS       Item sent successfully
 * @retval OSAL_QUEUE_FULL    Queue is full
 * @retval OSAL_QUEUE_TIMEOUT Timeout expired
 */
osal_status_t osal_queue_send(osal_queue_id_t queue_id,
                              const void *item,
                              uint32_t timeout_ms);

/**
 * @brief Receive an item from queue (task context)
 * @param[in]  queue_id    Queue ID
 * @param[out] buffer      Pointer to buffer to copy received item into
 * @param[in]  timeout_ms  Timeout in milliseconds
 * @return OSAL status code
 * @retval OSAL_SUCCESS        Item received successfully
 * @retval OSAL_QUEUE_EMPTY    Queue is empty
 * @retval OSAL_QUEUE_TIMEOUT  Timeout expired
 */
osal_status_t osal_queue_receive(osal_queue_id_t queue_id,
                                 void *buffer,
                                 uint32_t timeout_ms);

/**
 * @brief Delete/destroy a queue
 * @param[in] queue_id  Queue ID
 * @return OSAL status code
 */
osal_status_t osal_queue_delete(osal_queue_id_t queue_id);

/**
 * @brief Get number of items currently in queue
 * @param[in] queue_id  Queue ID
 * @return Number of items in queue
 */
uint32_t osal_queue_get_count(osal_queue_id_t queue_id);

/**
 * @brief Send an item to queue from ISR context
 * @param[in] queue_id  Queue ID
 * @param[in] item      Pointer to item to send
 * @return OSAL status code
 * @note No timeout parameter — ISR functions must never block.
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_queue_send_from_isr(osal_queue_id_t queue_id,
                                       const void *item);

/**
 * @brief Receive an item from queue from ISR context
 * @param[in]  queue_id  Queue ID
 * @param[out] buffer    Pointer to buffer for received item
 * @return OSAL status code
 * @note No timeout parameter — ISR functions must never block.
 * @note Platform-specific ISR context switching is handled internally
 */
osal_status_t osal_queue_receive_from_isr(osal_queue_id_t queue_id,
                                          void *buffer);

#endif /* OSAL_QUEUE_H */
