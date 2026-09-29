#include "tb_identity.h"

#include <string.h>

#include "cJSON.h"

static tb_identity_credentials_t s_credentials;
static bool s_loaded;

static void tb_identity_wipe(void *data, size_t length)
{
    volatile unsigned char *cursor = data;
    while (length-- > 0U) {
        *cursor++ = 0U;
    }
}

static bool tb_identity_ascii(const char *value, size_t maximum,
                              bool forbid_space)
{
    size_t length = 0U;
    while (length <= maximum && value[length] != '\0') {
        ++length;
    }
    if (length == 0U || length > maximum) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (ch < (forbid_space ? 0x21U : 0x20U) || ch > 0x7eU) {
            return false;
        }
    }
    return true;
}

static tb_identity_status_t tb_identity_validate(const char *json,
                                                 size_t length)
{
    static const char *const keys[] = {
        "schema_version", "client_id", "access_token"
    };
    cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1U, NULL, 1);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return TB_IDENTITY_ERR_MALFORMED;
    }
    size_t count = 0U;
    bool valid = true;
    for (const cJSON *child = root->child; child != NULL;
         child = child->next) {
        bool known = false;
        for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (child->string != NULL && strcmp(child->string, keys[i]) == 0) {
                known = true;
                break;
            }
        }
        valid = valid && known;
        ++count;
    }
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    cJSON *client = cJSON_GetObjectItemCaseSensitive(root, "client_id");
    cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "access_token");
    if (valid && count == 3U && cJSON_IsNumber(version) &&
        version->valuedouble > 1.0) {
        cJSON_Delete(root);
        return TB_IDENTITY_ERR_UNKNOWN_SCHEMA;
    }
    if (valid && count == 3U && cJSON_IsNumber(version) &&
        version->valuedouble == 1.0 && cJSON_IsString(client) &&
        client->valuestring != NULL && client->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return TB_IDENTITY_ERR_EMPTY_CLIENT_ID;
    }
    if (valid && count == 3U && cJSON_IsNumber(version) &&
        version->valuedouble == 1.0 && cJSON_IsString(token) &&
        token->valuestring != NULL && token->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return TB_IDENTITY_ERR_EMPTY_TOKEN;
    }
    if (cJSON_IsString(client) && client->valuestring != NULL &&
        strlen(client->valuestring) > TB_IDENTITY_CLIENT_ID_MAX_LEN) {
        cJSON_Delete(root);
        return TB_IDENTITY_ERR_BOUNDS;
    }
    if (cJSON_IsString(token) && token->valuestring != NULL &&
        strlen(token->valuestring) > TB_IDENTITY_TOKEN_MAX_LEN) {
        cJSON_Delete(root);
        return TB_IDENTITY_ERR_BOUNDS;
    }
    valid = valid && count == 3U && cJSON_IsNumber(version) &&
            version->valuedouble == 1.0 && cJSON_IsString(client) &&
            client->valuestring != NULL && cJSON_IsString(token) &&
            token->valuestring != NULL &&
            tb_identity_ascii(client->valuestring,
                              TB_IDENTITY_CLIENT_ID_MAX_LEN, false) &&
            tb_identity_ascii(token->valuestring,
                              TB_IDENTITY_TOKEN_MAX_LEN, true);
    cJSON_Delete(root);
    return valid ? TB_IDENTITY_OK : TB_IDENTITY_ERR_MALFORMED;
}

tb_identity_status_t tb_identity_load(const char *path)
{
    char json[TB_IDENTITY_MAX_FILE_BYTES + 1U];
    size_t length = 0U;
    cJSON *root = NULL;
    tb_identity_clear();
    if (path == NULL) {
        return TB_IDENTITY_ERR_ARGUMENT;
    }
    hq_json_doc_status_t read_status = hq_json_doc_read(
        path, TB_IDENTITY_MAX_FILE_BYTES, json, sizeof(json), &length);
    tb_identity_status_t status = TB_IDENTITY_ERR_IO;
    if (read_status == HQ_JSON_DOC_OK) {
        if (hq_json_doc_validate(json, length, NULL, NULL) != HQ_JSON_DOC_OK) {
            status = TB_IDENTITY_ERR_MALFORMED;
        } else {
            status = tb_identity_validate(json, length);
        }
    } else if (read_status == HQ_JSON_DOC_ERR_NOT_FOUND) {
        status = TB_IDENTITY_ERR_NOT_FOUND;
    } else if (read_status == HQ_JSON_DOC_ERR_BOUNDS) {
        status = TB_IDENTITY_ERR_BOUNDS;
    } else if (read_status == HQ_JSON_DOC_ERR_ARGUMENT) {
        status = TB_IDENTITY_ERR_ARGUMENT;
    }
    if (status == TB_IDENTITY_OK) {
        root = cJSON_ParseWithLengthOpts(json, length + 1U, NULL, 1);
        cJSON *client = cJSON_GetObjectItemCaseSensitive(root, "client_id");
        cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "access_token");
        strcpy(s_credentials.client_id, client->valuestring);
        strcpy(s_credentials.access_token, token->valuestring);
        tb_identity_wipe(token->valuestring, strlen(token->valuestring) + 1U);
        s_loaded = true;
    }
    cJSON_Delete(root);
    tb_identity_wipe(json, sizeof(json));
    return status;
}

bool tb_identity_is_loaded(void)
{
    return s_loaded;
}

const tb_identity_credentials_t *tb_identity_get(void)
{
    return s_loaded ? &s_credentials : NULL;
}

void tb_identity_clear(void)
{
    tb_identity_wipe(&s_credentials, sizeof(s_credentials));
    s_loaded = false;
}