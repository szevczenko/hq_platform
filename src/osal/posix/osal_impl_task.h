#ifndef OSAL_IMPL_TASK_H
#define OSAL_IMPL_TASK_H

#include <pthread.h>
#include <limits.h>

typedef pthread_t osal_task_id_t;

/**
 * Minimum stack size accepted by POSIX pthreads (PTHREAD_STACK_MIN).
 * Callers should use this as the lower bound for osal_task_create() stack_size.
 */
#define OSAL_TASK_MIN_STACK_SIZE  ((size_t)PTHREAD_STACK_MIN)

#endif /* OSAL_IMPL_TASK_H */
