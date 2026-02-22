#ifndef OSAL_IMPL_SEM_H
#define OSAL_IMPL_SEM_H

#include <semaphore.h>
#include <pthread.h>

typedef sem_t *osal_bin_sem_id_t;
typedef sem_t *osal_count_sem_id_t;
typedef pthread_mutex_t *osal_mutex_id_t;

#endif /* OSAL_IMPL_SEM_H */
