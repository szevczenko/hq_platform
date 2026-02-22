#include <stdio.h>

#include "osal_assert.h"

void osal_assert(const char *file, const char *function, int line, const char *message)
{
    fprintf(stderr, "OSAL Assertion Failed:\n");
    fprintf(stderr, "  File:     %s\n", file);
    fprintf(stderr, "  Function: %s\n", function);
    fprintf(stderr, "  Line:     %d\n", line);
    fprintf(stderr, "  Message:  %s\n", message);
    fflush(stderr);
}
