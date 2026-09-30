#ifndef TB_IDENTITY_H
#define TB_IDENTITY_H

#include <stdbool.h>

#include "hq_json_doc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_IDENTITY_CLIENT_ID_MAX_LEN 64U
#define TB_IDENTITY_TOKEN_MAX_LEN 128U
#define TB_IDENTITY_MAX_FILE_BYTES 2048U

typedef struct tb_identity_credentials {
    char client_id[TB_IDENTITY_CLIENT_ID_MAX_LEN + 1U];
    char access_token[TB_IDENTITY_TOKEN_MAX_LEN + 1U];
} tb_identity_credentials_t;

typedef enum tb_identity_status {
    TB_IDENTITY_OK = 0,
    TB_IDENTITY_ERR_ARGUMENT = -1,
    TB_IDENTITY_ERR_IO = -2,
    TB_IDENTITY_ERR_NOT_FOUND = -3,
    TB_IDENTITY_ERR_MALFORMED = -4,
    TB_IDENTITY_ERR_BOUNDS = -5,
    TB_IDENTITY_ERR_UNKNOWN_SCHEMA = -6,
    TB_IDENTITY_ERR_EMPTY_CLIENT_ID = -7,
    TB_IDENTITY_ERR_EMPTY_TOKEN = -8
} tb_identity_status_t;

/* The caller chooses the path and must enforce any provisioning policy. */
tb_identity_status_t tb_identity_load(const char *path);
bool tb_identity_is_loaded(void);
const tb_identity_credentials_t *tb_identity_get(void);
void tb_identity_clear(void);

#ifdef __cplusplus
}
#endif

#endif