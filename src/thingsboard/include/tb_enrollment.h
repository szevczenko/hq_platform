#ifndef TB_ENROLLMENT_H
#define TB_ENROLLMENT_H

#include <stdbool.h>
#include <stdint.h>

#include "tb_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_ENROLLMENT_MAX_STRING_LEN 64U
#define TB_ENROLLMENT_CREDENTIAL_TYPE_MAX_LEN 31U
#define TB_ENROLLMENT_CREDENTIAL_VALUE_MAX_LEN 128U

typedef enum {
	TB_ENROLLMENT_OK = 0,
	TB_ENROLLMENT_ERR_REQUEST = -1,
	TB_ENROLLMENT_ERR_RESPONSE = -2,
	TB_ENROLLMENT_ERR_PERSIST = -3,
} tb_enrollment_status_t;

typedef struct {
	const char *device_name;
	const char *provision_device_key;
	const char *provision_device_secret;
} tb_enrollment_config_t;

typedef struct {
	char credentials_type[TB_ENROLLMENT_CREDENTIAL_TYPE_MAX_LEN + 1U];
	char credentials_value[TB_ENROLLMENT_CREDENTIAL_VALUE_MAX_LEN + 1U];
} tb_enrollment_credentials_t;

typedef bool (*tb_enrollment_persist_cb_t)(
	const tb_enrollment_credentials_t *credentials, void *user_data);
typedef void (*tb_enrollment_complete_cb_t)(
	tb_enrollment_status_t status,
	const tb_enrollment_credentials_t *credentials, void *user_data);

/**
 * Perform enrollment synchronously, waiting at most timeout_ms for a response.
 * Credential data is provided to the persistence callback and, on success,
 * the completion callback; it is cleared when this function returns. Neither
 * callback is required. The completion callback receives NULL credentials on
 * failure.
 */
tb_enrollment_status_t tb_enrollment_enroll(
	tb_client_t *client, const tb_enrollment_config_t *config,
	uint32_t timeout_ms, tb_enrollment_persist_cb_t persist_cb,
	void *persist_user_data, tb_enrollment_complete_cb_t complete_cb,
	void *complete_user_data);

#ifdef __cplusplus
}
#endif

#endif