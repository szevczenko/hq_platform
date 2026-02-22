#ifndef OSAL_COMMON_TYPE_H
#define OSAL_COMMON_TYPE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/** @brief Infinite timeout for blocking operations */
#define OSAL_MAX_DELAY  0xFFFFFFFF

/** @brief Maximum name length for all OSAL objects */
#define OSAL_MAX_NAME_LEN  32

/** @brief Semaphore initial value: empty/unavailable (0) */
#define OSAL_SEM_EMPTY  0

/** @brief Semaphore initial value: full/available (1) */
#define OSAL_SEM_FULL   1

#endif /* OSAL_COMMON_TYPE_H */
