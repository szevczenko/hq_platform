#ifndef OSAL_IMPL_TIMER_H
#define OSAL_IMPL_TIMER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "osal_common_type.h"

struct osal_timer_internal
{
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_t thread;
	clockid_t clock_id;
	uint32_t period_ms;
	bool auto_reload;
	bool active;
	bool stop_requested;
	bool use_static;
	void (*callback)(osal_timer_id_t);
	void *context;
	char name[OSAL_MAX_NAME_LEN];
};

typedef struct osal_timer_internal *osal_timer_id_t;

#define OSAL_TIMER_STATIC_SIZE  (sizeof(struct osal_timer_internal))

#endif /* OSAL_IMPL_TIMER_H */
