#ifndef HQ_DEVICE_INFO_H
#define HQ_DEVICE_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hq_json_doc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HQ_DEVICE_INFO_PRODUCT_MAX_LEN 32U
#define HQ_DEVICE_INFO_HARDWARE_REVISION_MAX_LEN 16U
#define HQ_DEVICE_INFO_SERIAL_MAX_LEN 32U
#define HQ_DEVICE_INFO_NAME_MAX_LEN 64U
#define HQ_DEVICE_INFO_MAX_FILE_BYTES 2048U

typedef enum hq_device_manufacturing_state {
    HQ_DEVICE_UNPROVISIONED = 0,
    HQ_DEVICE_PROVISIONED = 1,
    HQ_DEVICE_QUARANTINED = 2
} hq_device_manufacturing_state_t;

typedef enum hq_device_credential_mode {
    HQ_DEVICE_CREDENTIAL_NONE = 0,
    HQ_DEVICE_CREDENTIAL_PSK = 1,
    HQ_DEVICE_CREDENTIAL_MTLS = 2
} hq_device_credential_mode_t;

typedef struct hq_device_info {
    uint32_t schema_version;
    char product[HQ_DEVICE_INFO_PRODUCT_MAX_LEN + 1U];
    char hardware_revision[HQ_DEVICE_INFO_HARDWARE_REVISION_MAX_LEN + 1U];
    char serial[HQ_DEVICE_INFO_SERIAL_MAX_LEN + 1U];
    char thingsboard_name[HQ_DEVICE_INFO_NAME_MAX_LEN + 1U];
} hq_device_info_t;

typedef struct hq_device_manufacturing_info {
    uint32_t schema_version;
    hq_device_manufacturing_state_t manufacturing_state;
    hq_device_credential_mode_t credential_mode;
} hq_device_manufacturing_info_t;

/* Paths are supplied by the caller; this service has no product mount policy. */
hq_json_doc_status_t hq_device_info_load(
    const char *path, hq_device_info_t *out_info);
hq_json_doc_status_t hq_device_manufacturing_load(
    const char *path, hq_device_manufacturing_info_t *out_info);
bool hq_device_info_is_provisioned(
    const hq_device_manufacturing_info_t *manufacturing);

#ifdef __cplusplus
}
#endif

#endif