#include "hq_device_info.h"

#include <string.h>

#include "cJSON.h"

typedef struct hq_device_validate_context {
    bool manufacturing;
} hq_device_validate_context_t;

static bool hq_device_ascii(const char *value, size_t maximum)
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
        if (ch < 0x21U || ch > 0x7eU) {
            return false;
        }
    }
    return true;
}

static bool hq_device_keys(const cJSON *root, const char *const *keys,
                           size_t expected)
{
    size_t count = 0U;
    if (!cJSON_IsObject(root)) {
        return false;
    }
    for (const cJSON *child = root->child; child != NULL;
         child = child->next) {
        bool known = false;
        for (size_t i = 0U; i < expected; ++i) {
            if (child->string != NULL && strcmp(child->string, keys[i]) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            return false;
        }
        ++count;
    }
    return count == expected;
}

static hq_json_doc_status_t hq_device_validate(
    const char *json, size_t length, void *context)
{
    static const char *const device_keys[] = {
        "schema_version", "product", "hardware_revision", "serial",
        "thingsboard_name"
    };
    static const char *const manufacturing_keys[] = {
        "schema_version", "manufacturing_state", "credential_mode"
    };
    const hq_device_validate_context_t *validation = context;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1U, NULL, 1);
    if (root == NULL) {
        return HQ_JSON_DOC_ERR_MALFORMED;
    }
    const char *const *keys = validation->manufacturing
                                  ? manufacturing_keys
                                  : device_keys;
    size_t key_count = validation->manufacturing
                           ? sizeof(manufacturing_keys) /
                                 sizeof(manufacturing_keys[0])
                           : sizeof(device_keys) / sizeof(device_keys[0]);
    bool valid = hq_device_keys(root, keys, key_count);
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    valid = valid && cJSON_IsNumber(version) &&
            version->valuedouble == 1.0;
    if (!validation->manufacturing && valid) {
        cJSON *product = cJSON_GetObjectItemCaseSensitive(root, "product");
        cJSON *revision = cJSON_GetObjectItemCaseSensitive(root,
                                                            "hardware_revision");
        cJSON *serial = cJSON_GetObjectItemCaseSensitive(root, "serial");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(root,
                                                        "thingsboard_name");
        valid = cJSON_IsString(product) && product->valuestring != NULL &&
                hq_device_ascii(product->valuestring,
                                HQ_DEVICE_INFO_PRODUCT_MAX_LEN) &&
                cJSON_IsString(revision) && revision->valuestring != NULL &&
                hq_device_ascii(revision->valuestring,
                                HQ_DEVICE_INFO_HARDWARE_REVISION_MAX_LEN) &&
                cJSON_IsString(serial) && serial->valuestring != NULL &&
                hq_device_ascii(serial->valuestring,
                                HQ_DEVICE_INFO_SERIAL_MAX_LEN) &&
                cJSON_IsString(name) && name->valuestring != NULL &&
                hq_device_ascii(name->valuestring,
                                HQ_DEVICE_INFO_NAME_MAX_LEN);
    } else if (validation->manufacturing && valid) {
        cJSON *state = cJSON_GetObjectItemCaseSensitive(
            root, "manufacturing_state");
        cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "credential_mode");
        valid = cJSON_IsNumber(state) && state->valuedouble >= 0.0 &&
                state->valuedouble <= 2.0 &&
                state->valuedouble == (double)(int)state->valuedouble &&
                cJSON_IsNumber(mode) && mode->valuedouble >= 0.0 &&
                mode->valuedouble <= 2.0 &&
                mode->valuedouble == (double)(int)mode->valuedouble;
    }
    cJSON_Delete(root);
    return valid ? HQ_JSON_DOC_OK : HQ_JSON_DOC_ERR_MALFORMED;
}

hq_json_doc_status_t hq_device_info_load(const char *path,
                                         hq_device_info_t *out_info)
{
    char json[HQ_DEVICE_INFO_MAX_FILE_BYTES + 1U];
    size_t length = 0U;
    hq_device_validate_context_t context = { .manufacturing = false };
    hq_json_doc_status_t status;
    if (path == NULL || out_info == NULL) {
        return HQ_JSON_DOC_ERR_ARGUMENT;
    }
    status = hq_json_doc_read(path, HQ_DEVICE_INFO_MAX_FILE_BYTES, json,
                              sizeof(json), &length);
    if (status == HQ_JSON_DOC_OK) {
        status = hq_json_doc_validate(json, length, hq_device_validate,
                                      &context);
    }
    if (status == HQ_JSON_DOC_OK) {
        cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1U, NULL, 1);
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
        out_info->schema_version = (uint32_t)item->valuedouble;
        item = cJSON_GetObjectItemCaseSensitive(root, "product");
        strcpy(out_info->product, item->valuestring);
        item = cJSON_GetObjectItemCaseSensitive(root, "hardware_revision");
        strcpy(out_info->hardware_revision, item->valuestring);
        item = cJSON_GetObjectItemCaseSensitive(root, "serial");
        strcpy(out_info->serial, item->valuestring);
        item = cJSON_GetObjectItemCaseSensitive(root, "thingsboard_name");
        strcpy(out_info->thingsboard_name, item->valuestring);
        cJSON_Delete(root);
    }
    return status;
}

hq_json_doc_status_t hq_device_manufacturing_load(
    const char *path, hq_device_manufacturing_info_t *out_info)
{
    char json[HQ_DEVICE_INFO_MAX_FILE_BYTES + 1U];
    size_t length = 0U;
    hq_device_validate_context_t context = { .manufacturing = true };
    hq_json_doc_status_t status;
    if (path == NULL || out_info == NULL) {
        return HQ_JSON_DOC_ERR_ARGUMENT;
    }
    status = hq_json_doc_read(path, HQ_DEVICE_INFO_MAX_FILE_BYTES, json,
                              sizeof(json), &length);
    if (status == HQ_JSON_DOC_OK) {
        status = hq_json_doc_validate(json, length, hq_device_validate,
                                      &context);
    }
    if (status == HQ_JSON_DOC_OK) {
        cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1U, NULL, 1);
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
        out_info->schema_version = (uint32_t)item->valuedouble;
        item = cJSON_GetObjectItemCaseSensitive(root, "manufacturing_state");
        out_info->manufacturing_state =
            (hq_device_manufacturing_state_t)(int)item->valuedouble;
        item = cJSON_GetObjectItemCaseSensitive(root, "credential_mode");
        out_info->credential_mode =
            (hq_device_credential_mode_t)(int)item->valuedouble;
        cJSON_Delete(root);
    }
    return status;
}

bool hq_device_info_is_provisioned(
    const hq_device_manufacturing_info_t *manufacturing)
{
    return manufacturing != NULL &&
           manufacturing->manufacturing_state == HQ_DEVICE_PROVISIONED &&
           manufacturing->credential_mode != HQ_DEVICE_CREDENTIAL_NONE;
}