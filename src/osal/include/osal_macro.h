#ifndef OSAL_MACRO_H
#define OSAL_MACRO_H

#include <string.h>

/**
 * @brief Generic argument checking macro for non-critical values
 *
 * Returns errcode if cond evaluates false.
 */
#define ARGCHECK(cond, errcode) \
    if (!(cond))                \
    {                           \
        return errcode;         \
    }

/**
 * @brief String length limit check macro
 *
 * Ensures a null terminator is found within len bytes.
 */
#define LENGTHCHECK(str, len, errcode) ARGCHECK(memchr((str), '\0', (len)), (errcode))

#endif /* OSAL_MACRO_H */
