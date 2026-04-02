#ifndef HQ_CMD_INTERNAL_H
#define HQ_CMD_INTERNAL_H

#include <stdint.h>

#include "embedded_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Platform HAL — implemented per target in platforms/<target>/ */
void hq_cmd_platform_output_init(void);
void hq_cmd_platform_write_char(char c);
int  hq_cmd_platform_read_char(void);

/* Internal registration using raw EmbeddedCli binding (core use only). */
int32_t hq_cmd_register_internal(const CliCommandBinding *binding);

#ifdef __cplusplus
}
#endif

#endif /* HQ_CMD_INTERNAL_H */
