#ifndef HQ_CMD_INTERNAL_H
#define HQ_CMD_INTERNAL_H

#include <stdint.h>

#include "embedded_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

void hq_cmd_platform_write_char(char c);
void hq_cmd_register_builtin_commands(void);
int32_t hq_cmd_register_internal(const CliCommandBinding *binding);

#ifdef __cplusplus
}
#endif

#endif /* HQ_CMD_INTERNAL_H */
