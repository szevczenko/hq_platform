#include "hq_json_doc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "osal_error.h"
#include "osal_file.h"

#define HQ_JSON_TMP_SUFFIX ".tmp"
#define HQ_JSON_GOOD_SUFFIX ".good"
#define HQ_JSON_GOOD_TMP_SUFFIX ".good.tmp"
#define HQ_JSON_GOOD_OLD_SUFFIX ".good.old"

static bool hq_json_is_missing(int32_t rc)
{
    return rc == OSAL_ERR_NAME_NOT_FOUND || rc == OSAL_FS_ERR_PATH_INVALID;
}

static void hq_json_wipe(char *buffer, size_t length)
{
    volatile char *cursor = (volatile char *)buffer;
    while (length-- > 0U) {
        *cursor++ = 0;
    }
}

static bool hq_json_has_nul_escape(const char *json, size_t length)
{
    bool in_string = false;
    for (size_t i = 0U; i < length; ++i) {
        if (!in_string) {
            in_string = json[i] == '"';
            continue;
        }
        if (json[i] == '"') {
            in_string = false;
        } else if (json[i] == '\\') {
            if (i + 5U < length && json[i + 1U] == 'u' &&
                json[i + 2U] == '0' && json[i + 3U] == '0' &&
                json[i + 4U] == '0' && json[i + 5U] == '0') {
                return true;
            }
            ++i; /* Skip the escaped character. */
        }
    }
    return false;
}

static bool hq_json_unique_keys(const cJSON *node)
{
    if (cJSON_IsObject(node)) {
        for (const cJSON *child = node->child; child != NULL;
             child = child->next) {
            if (child->string == NULL) {
                return false;
            }
            for (const cJSON *other = child->next; other != NULL;
                 other = other->next) {
                if (other->string != NULL &&
                    strcmp(child->string, other->string) == 0) {
                    return false;
                }
            }
            if (!hq_json_unique_keys(child)) {
                return false;
            }
        }
    } else if (cJSON_IsArray(node)) {
        for (const cJSON *child = node->child; child != NULL;
             child = child->next) {
            if (!hq_json_unique_keys(child)) {
                return false;
            }
        }
    }
    return true;
}

hq_json_doc_status_t hq_json_doc_validate(
    const char *json, size_t json_len, hq_json_doc_validator_t validator,
    void *context)
{
    if (json == NULL || json_len == 0U) {
        return HQ_JSON_DOC_ERR_ARGUMENT;
    }
    if (memchr(json, '\0', json_len) != NULL ||
        hq_json_has_nul_escape(json, json_len)) {
        return HQ_JSON_DOC_ERR_MALFORMED;
    }

    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len + 1U, NULL, 1);
    if (root == NULL || !hq_json_unique_keys(root)) {
        cJSON_Delete(root);
        return HQ_JSON_DOC_ERR_MALFORMED;
    }
    cJSON_Delete(root);
    return validator == NULL ? HQ_JSON_DOC_OK
                             : validator(json, json_len, context);
}

hq_json_doc_status_t hq_json_doc_read(const char *path, size_t max_bytes,
                                      char *out_json, size_t out_capacity,
                                      size_t *out_len)
{
    osal_fstat_t stat = { 0 };
    size_t total = 0U;
    if (path == NULL || out_json == NULL || out_len == NULL ||
        out_capacity < max_bytes + 1U) {
        return HQ_JSON_DOC_ERR_ARGUMENT;
    }
    *out_len = 0U;
    out_json[0] = '\0';
    int32_t rc = osal_stat(path, &stat);
    if (rc != OSAL_SUCCESS) {
        return hq_json_is_missing(rc) ? HQ_JSON_DOC_ERR_NOT_FOUND
                                     : HQ_JSON_DOC_ERR_IO;
    }
    if ((size_t)OSAL_FILESTAT_SIZE(stat) > max_bytes) {
        return HQ_JSON_DOC_ERR_BOUNDS;
    }
    osal_file_id_t fd = osal_open_create(path, OSAL_FILE_FLAG_NONE,
                                         OSAL_READ_ONLY);
    if (fd < 0) {
        return HQ_JSON_DOC_ERR_IO;
    }
    while (total <= max_bytes) {
        size_t wanted = total < max_bytes ? max_bytes - total : 1U;
        int32_t count = osal_read(fd, out_json + total, wanted);
        if (count < 0) {
            (void)osal_close(fd);
            return HQ_JSON_DOC_ERR_IO;
        }
        if (count == 0) {
            break;
        }
        total += (size_t)count;
    }
    rc = osal_close(fd);
    if (rc != OSAL_SUCCESS) {
        return HQ_JSON_DOC_ERR_IO;
    }
    if (total > max_bytes) {
        return HQ_JSON_DOC_ERR_BOUNDS;
    }
    out_json[total] = '\0';
    *out_len = total;
    return HQ_JSON_DOC_OK;
}

static bool hq_json_path(const char *path, const char *suffix, char *out,
                         size_t capacity)
{
    int written = snprintf(out, capacity, "%s%s", path, suffix);
    return written > 0 && (size_t)written < capacity;
}

static void hq_json_remove(const char *path)
{
    (void)osal_remove(path);
}

static hq_json_doc_status_t hq_json_write(const char *path, const char *json,
                                          size_t length)
{
    osal_file_id_t fd = osal_open_create(path, OSAL_FILE_FLAG_CREATE |
                                         OSAL_FILE_FLAG_TRUNCATE,
                                         OSAL_WRITE_ONLY);
    if (fd < 0) {
        return HQ_JSON_DOC_ERR_IO;
    }
    size_t written = 0U;
    while (written < length) {
        int32_t count = osal_write(fd, json + written, length - written);
        if (count <= 0) {
            (void)osal_close(fd);
            return HQ_JSON_DOC_ERR_IO;
        }
        written += (size_t)count;
    }
    return osal_close(fd) == OSAL_SUCCESS ? HQ_JSON_DOC_OK
                                          : HQ_JSON_DOC_ERR_IO;
}

static hq_json_doc_status_t hq_json_rename(int32_t rc)
{
    return rc == OSAL_ERR_OPERATION_NOT_SUPPORTED ||
                   rc == OSAL_ERR_NOT_IMPLEMENTED
               ? HQ_JSON_DOC_ERR_UNSUPPORTED
               : HQ_JSON_DOC_ERR_IO;
}

hq_json_doc_status_t hq_json_doc_commit(
    const char *path, const char *json, size_t json_len, size_t max_bytes,
    hq_json_doc_validator_t validator, void *context)
{
    char tmp[OSAL_MAX_PATH_LEN];
    char good[OSAL_MAX_PATH_LEN];
    char good_tmp[OSAL_MAX_PATH_LEN];
    char good_old[OSAL_MAX_PATH_LEN];
    char *verify = NULL;
    size_t verify_len = 0U;
    bool good_preserved = false;
    bool good_promoted = false;
    hq_json_doc_status_t status;

    if (path == NULL || json == NULL || validator == NULL || json_len == 0U) {
        return HQ_JSON_DOC_ERR_ARGUMENT;
    }
    if (json_len > max_bytes) {
        return HQ_JSON_DOC_ERR_BOUNDS;
    }
    status = hq_json_doc_validate(json, json_len, validator, context);
    if (status != HQ_JSON_DOC_OK) {
        return status;
    }
    if (!hq_json_path(path, HQ_JSON_TMP_SUFFIX, tmp, sizeof(tmp)) ||
        !hq_json_path(path, HQ_JSON_GOOD_SUFFIX, good, sizeof(good)) ||
        !hq_json_path(path, HQ_JSON_GOOD_TMP_SUFFIX, good_tmp,
                      sizeof(good_tmp)) ||
        !hq_json_path(path, HQ_JSON_GOOD_OLD_SUFFIX, good_old,
                      sizeof(good_old))) {
        return HQ_JSON_DOC_ERR_ARGUMENT;
    }
    verify = malloc(max_bytes + 1U);
    if (verify == NULL) {
        return HQ_JSON_DOC_ERR_IO;
    }
    status = hq_json_write(tmp, json, json_len);
    if (status != HQ_JSON_DOC_OK) {
        hq_json_remove(tmp);
        goto done;
    }
    status = hq_json_doc_read(tmp, max_bytes, verify, max_bytes + 1U,
                              &verify_len);
    if (status == HQ_JSON_DOC_OK) {
        status = hq_json_doc_validate(verify, verify_len, validator, context);
    }
    if (status != HQ_JSON_DOC_OK) {
        hq_json_remove(tmp);
        goto done;
    }

    osal_fstat_t stat = { 0 };
    int32_t stat_rc = osal_stat(path, &stat);
    if (stat_rc == OSAL_SUCCESS) {
        status = hq_json_doc_read(path, max_bytes, verify, max_bytes + 1U,
                                  &verify_len);
        /* Existence was confirmed by stat: never treat it as missing now. */
        if (status != HQ_JSON_DOC_OK && status != HQ_JSON_DOC_ERR_BOUNDS) {
            hq_json_remove(tmp);
            status = HQ_JSON_DOC_ERR_IO;
            goto done;
        }
        if (status == HQ_JSON_DOC_OK &&
            hq_json_doc_validate(verify, verify_len, validator, context) ==
                HQ_JSON_DOC_OK) {
            int32_t cp_rc = osal_cp(path, good_tmp);
            if (cp_rc != OSAL_SUCCESS) {
                hq_json_remove(good_tmp);
                hq_json_remove(tmp);
                status = HQ_JSON_DOC_ERR_IO;
                goto done;
            }
            status = hq_json_doc_read(good_tmp, max_bytes, verify,
                                      max_bytes + 1U, &verify_len);
            if (status == HQ_JSON_DOC_OK) {
                status = hq_json_doc_validate(verify, verify_len, validator,
                                              context);
            }
            if (status != HQ_JSON_DOC_OK) {
                hq_json_remove(good_tmp);
                hq_json_remove(tmp);
                status = HQ_JSON_DOC_ERR_IO;
                goto done;
            }
            int32_t preserve_rc = osal_rename(good, good_old);
            if (preserve_rc == OSAL_SUCCESS) {
                good_preserved = true;
            } else if (!hq_json_is_missing(preserve_rc)) {
                hq_json_remove(good_tmp);
                hq_json_remove(tmp);
                status = hq_json_rename(preserve_rc);
                goto done;
            }
            int32_t good_rc = osal_rename(good_tmp, good);
            if (good_rc != OSAL_SUCCESS) {
                if (good_preserved) {
                    (void)osal_rename(good_old, good);
                }
                hq_json_remove(good_tmp);
                hq_json_remove(tmp);
                status = hq_json_rename(good_rc);
                goto done;
            }
            good_promoted = true;
        }
    } else if (!hq_json_is_missing(stat_rc)) {
        hq_json_remove(tmp);
        status = HQ_JSON_DOC_ERR_IO;
        goto done;
    }

    int32_t rename_rc = osal_rename(tmp, path);
    if (rename_rc != OSAL_SUCCESS) {
        if (good_preserved) {
            (void)osal_rename(good_old, good);
        } else if (good_promoted) {
            hq_json_remove(good);
        }
        hq_json_remove(tmp);
        status = hq_json_rename(rename_rc);
        goto done;
    }
    if (good_preserved) {
        hq_json_remove(good_old);
    }
    status = HQ_JSON_DOC_OK;

done:
    hq_json_wipe(verify, max_bytes + 1U);
    free(verify);
    return status;
}

static bool hq_json_case_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left++) !=
            tolower((unsigned char)*right++)) {
            return false;
        }
    }
    return *left == *right;
}

static bool hq_json_secret_key(const char *key)
{
    static const char *const names[] = {
        "token", "secret", "password", "psk", "key", "provisioning"
    };
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (hq_json_case_equal(key, names[i])) {
            return true;
        }
    }
    return false;
}

static bool hq_json_redact_tree(cJSON *node)
{
    cJSON *child = node->child;
    while (child != NULL) {
        cJSON *next = child->next;
        if (child->string != NULL && hq_json_secret_key(child->string)) {
            cJSON *replacement = cJSON_CreateString("[redacted]");
            if (replacement == NULL) {
                return false;
            }
            if (!cJSON_ReplaceItemViaPointer(node, child, replacement)) {
                cJSON_Delete(replacement);
                return false;
            }
        } else if (!hq_json_redact_tree(child)) {
            return false;
        }
        child = next;
    }
    return true;
}

size_t hq_json_doc_redact(const char *json, char *out, size_t out_capacity)
{
    if (json == NULL || out == NULL || out_capacity == 0U) {
        return 0U;
    }
    cJSON *root = cJSON_Parse(json);
    if (root == NULL || !hq_json_redact_tree(root)) {
        cJSON_Delete(root);
        const char *fallback = "[unparsable document]";
        size_t length = strlen(fallback);
        if (length >= out_capacity) {
            return 0U;
        }
        memcpy(out, fallback, length + 1U);
        return length;
    }
    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rendered == NULL) {
        return 0U;
    }
    size_t length = strlen(rendered);
    if (length >= out_capacity) {
        cJSON_free(rendered);
        return 0U;
    }
    memcpy(out, rendered, length + 1U);
    cJSON_free(rendered);
    return length;
}