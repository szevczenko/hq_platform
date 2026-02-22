#ifndef OSAL_ASSERT_H
#define OSAL_ASSERT_H

#include "osal_common_type.h"
#include "osal_error.h"
#include "osal_macro.h"

/**
 * @brief Check if pointer is non-NULL
 *
 * Triggers osal_assert and returns OSAL_INVALID_POINTER if ptr is NULL.
 */
#define OSAL_CHECK_POINTER(ptr) \
    if ((ptr) == NULL)          \
    {                           \
        osal_assert(__FILE__, __func__, __LINE__, #ptr " is NULL"); \
        return OSAL_INVALID_POINTER; \
    }

/**
 * @brief Check string pointer and length
 *
 * Validates pointer and ensures null terminator within maxlen.
 */
#define OSAL_CHECK_STRING(str, maxlen, errcode) \
    do                                          \
    {                                           \
        OSAL_CHECK_POINTER(str);                \
        LENGTHCHECK(str, maxlen, errcode);      \
    } while (0)

/**
 * @brief Platform-specific assertion handler
 */
void osal_assert(const char *file, const char *function, int line, const char *message);

#endif /* OSAL_ASSERT_H */
