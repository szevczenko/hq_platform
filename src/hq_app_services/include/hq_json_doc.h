#ifndef HQ_JSON_DOC_H
#define HQ_JSON_DOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hq_json_doc_status {
    HQ_JSON_DOC_OK = 0,
    HQ_JSON_DOC_ERR_ARGUMENT = -1,
    HQ_JSON_DOC_ERR_IO = -2,
    HQ_JSON_DOC_ERR_NOT_FOUND = -3,
    HQ_JSON_DOC_ERR_MALFORMED = -4,
    HQ_JSON_DOC_ERR_BOUNDS = -5,
    HQ_JSON_DOC_ERR_UNSUPPORTED = -6
} hq_json_doc_status_t;

typedef hq_json_doc_status_t (*hq_json_doc_validator_t)(
    const char *json, size_t json_len, void *context);

/* Strictly parses one JSON document and rejects duplicate keys at any depth. */
hq_json_doc_status_t hq_json_doc_validate(
    const char *json, size_t json_len, hq_json_doc_validator_t validator,
    void *context);

/* Reads one bounded file. The caller supplies max_bytes + 1 bytes. */
hq_json_doc_status_t hq_json_doc_read(const char *path, size_t max_bytes,
                                      char *out_json, size_t out_capacity,
                                      size_t *out_len);

/* Validates, stages, verifies, then atomically replaces path; retains .good. */
hq_json_doc_status_t hq_json_doc_commit(
    const char *path, const char *json, size_t json_len, size_t max_bytes,
    hq_json_doc_validator_t validator, void *context);

/* Redacts secret-bearing JSON string members without logging source text. */
size_t hq_json_doc_redact(const char *json, char *out, size_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif