#ifndef OSAL_IMPL_TASK_H
#define OSAL_IMPL_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef TaskHandle_t osal_task_id_t;

/**
 * Minimum usable stack size on ESP-IDF (FreeRTOS configMINIMAL_STACK_SIZE * word).
 * Callers should use this as the lower bound for osal_task_create() stack_size.
 */
#define OSAL_TASK_MIN_STACK_SIZE  ((size_t)(configMINIMAL_STACK_SIZE * sizeof(StackType_t)))

#endif /* OSAL_IMPL_TASK_H */
