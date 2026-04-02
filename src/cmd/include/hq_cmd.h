#ifndef HQ_CMD_H
#define HQ_CMD_H

#include <stdbool.h>
#include <stdint.h>

/** Opaque CLI instance type (hides the underlying EmbeddedCli library). */
typedef struct EmbeddedCli hq_cmd_cli_t;

/**
 * Command handler function type.
 *
 * @param cli      Opaque CLI instance (pass through; not normally used directly).
 * @param args     Raw argument string (tokenize with hq_cmd_tokenize_args() first).
 * @param context  User-supplied context pointer from hq_cmd_binding_t.
 */
typedef void (*hq_cmd_handler_t)(hq_cmd_cli_t *cli, char *args, void *context);

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

/* ── Lifecycle ─────────────────────────────────────────────────────── */

int32_t hq_cmd_init(void);
void hq_cmd_deinit(void);
void hq_cmd_process(void);
void hq_cmd_receive_char(char c);
void hq_cmd_stop(void);
void hq_cmd_wait(void);

/* ── Command registration ──────────────────────────────────────────── */

int32_t hq_cmd_register(const hq_cmd_binding_t *binding);
void hq_cmd_register_builtin_commands(void);

/* ── Output ────────────────────────────────────────────────────────── */

void hq_cmd_print(const char *text);

/* ── Argument helpers ──────────────────────────────────────────────── */

/**
 * Tokenize a raw argument string in-place.
 * Replaces spaces with '\\0'; the result is double-null-terminated.
 * Call this before using hq_cmd_get_token / hq_cmd_get_token_count.
 */
void hq_cmd_tokenize_args(char *args);

/**
 * Return the number of tokens in a tokenized argument string.
 */
uint16_t hq_cmd_get_token_count(const char *args);

/**
 * Return a read-only pointer to the token at 1-based position @p pos,
 * or NULL if @p pos is out of range.
 */
const char *hq_cmd_get_token(const char *args, uint16_t pos);

/**
 * Same as hq_cmd_get_token() but returns a mutable pointer.
 */
char *hq_cmd_get_token_variable(char *args, uint16_t pos);

/**
 * Find @p token in the tokenized string. Returns 1-based position, or 0
 * if not found.
 */
uint16_t hq_cmd_find_token(const char *args, const char *token);

/* ── Advanced / low-level ──────────────────────────────────────────── */

hq_cmd_cli_t *hq_cmd_get_cli(void);

#ifdef __cplusplus
}
#endif

#endif /* HQ_CMD_H */
