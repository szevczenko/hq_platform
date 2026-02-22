#ifndef OSAL_IMPL_SEM_H
#define OSAL_IMPL_SEM_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef SemaphoreHandle_t osal_bin_sem_id_t;
typedef SemaphoreHandle_t osal_count_sem_id_t;
typedef SemaphoreHandle_t osal_mutex_id_t;

#endif /* OSAL_IMPL_SEM_H */
