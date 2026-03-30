#ifndef HQ_CMD_H
#define HQ_CMD_H

#include <stdbool.h>
#include <stdint.h>

typedef struct EmbeddedCli EmbeddedCli;

typedef void (*hq_cmd_handler_t)(EmbeddedCli *cli, char *args, void *context);

typedef struct
{
	const char *name;
	const char *help;
	bool tokenize_args;
	void *context;
	hq_cmd_handler_t handler;
} hq_cmd_binding_t;

#ifdef __cplusplus
extern "C" {
#endif

int32_t hq_cmd_init(void);
void hq_cmd_deinit(void);
void hq_cmd_process(void);
void hq_cmd_receive_char(char c);
void hq_cmd_print(const char *text);
int32_t hq_cmd_register(const hq_cmd_binding_t *binding);
EmbeddedCli *hq_cmd_get_cli(void);

#ifdef __cplusplus
}
#endif

#endif /* HQ_CMD_H */
