#ifndef OSAL_IMPL_TIMER_H
#define OSAL_IMPL_TIMER_H

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

typedef TimerHandle_t osal_timer_id_t;

#define OSAL_TIMER_STATIC_SIZE  (sizeof(StaticTimer_t))

#endif /* OSAL_IMPL_TIMER_H */
