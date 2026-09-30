#include "tb_enrollment.h"

#include <stddef.h>
#include <string.h>

#include "cJSON.h"
#include "osal_task.h"
#include "osal_bin_sem.h"
#include "tb_provision.h"

typedef struct {
	bool valid;
	osal_bin_sem_id_t done;
	tb_enrollment_credentials_t credentials;
} response_state_t;

static bool printable_value(const char *value, size_t max_len, bool no_space)
{
	size_t length;
	if (value == NULL || value[0] == '\0') {
		return false;
	}
	length = strnlen(value, max_len + 1U);
	if (length == 0U || length > max_len) {
		return false;
	}
	for (size_t i = 0U; i < length; ++i) {
		unsigned char ch = (unsigned char)value[i];
		if (ch < 0x20U || ch > 0x7eU || (no_space && ch == ' ')) {
			return false;
		}
	}
	return true;
}

static void response_callback(const char *json, void *user_data)
{
	response_state_t *state = (response_state_t *)user_data;
	cJSON *root;
	cJSON *status;
	cJSON *type;
	cJSON *value;
	if (json == NULL || state == NULL) {
		return;
	}
	root = cJSON_Parse(json);
	status = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "status");
	type = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "credentialsType");
	value = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "credentialsValue");
	if (cJSON_IsString(status) && strcmp(status->valuestring, "SUCCESS") == 0 &&
	    cJSON_IsString(type) && strcmp(type->valuestring, "ACCESS_TOKEN") == 0 &&
	    cJSON_IsString(value) &&
	    printable_value(type->valuestring,
			    TB_ENROLLMENT_CREDENTIAL_TYPE_MAX_LEN, true) &&
	    printable_value(value->valuestring,
			    TB_ENROLLMENT_CREDENTIAL_VALUE_MAX_LEN, true)) {
		(void)strncpy(state->credentials.credentials_type, type->valuestring,
			      sizeof(state->credentials.credentials_type) - 1U);
		(void)strncpy(state->credentials.credentials_value, value->valuestring,
			      sizeof(state->credentials.credentials_value) - 1U);
		state->valid = true;
	}
	(void)osal_bin_sem_give(state->done);
	cJSON_Delete(root);
}

static void complete(tb_enrollment_complete_cb_t callback,
		     tb_enrollment_status_t status,
		     const tb_enrollment_credentials_t *credentials,
		     void *user_data)
{
	if (callback != NULL) {
		callback(status, credentials, user_data);
	}
}

tb_enrollment_status_t tb_enrollment_enroll(
	tb_client_t *client, const tb_enrollment_config_t *config,
	uint32_t timeout_ms, tb_enrollment_persist_cb_t persist_cb,
	void *persist_user_data, tb_enrollment_complete_cb_t complete_cb,
	void *complete_user_data)
{
	response_state_t response = { 0 };
	tb_provision_request_t request;
	uint32_t start;
	tb_enrollment_status_t result;

	if (client == NULL || config == NULL || timeout_ms == 0U ||
	    !printable_value(config->device_name, TB_ENROLLMENT_MAX_STRING_LEN, false) ||
	    !printable_value(config->provision_device_key,
			     TB_ENROLLMENT_MAX_STRING_LEN, true) ||
	    !printable_value(config->provision_device_secret,
			     TB_ENROLLMENT_MAX_STRING_LEN, true)) {
		result = TB_ENROLLMENT_ERR_REQUEST;
		complete(complete_cb, result, NULL, complete_user_data);
		return result;
	}
	if (osal_bin_sem_create(&response.done, "tb_enrollment_done",
	                        OSAL_SEM_EMPTY) != OSAL_SUCCESS) {
		complete(complete_cb, TB_ENROLLMENT_ERR_REQUEST, NULL,
		         complete_user_data);
		return TB_ENROLLMENT_ERR_REQUEST;
	}
	request = (tb_provision_request_t){
		.device_name = config->device_name,
		.provision_device_key = config->provision_device_key,
		.provision_device_secret = config->provision_device_secret,
	};
	if (tb_provision_request(client, &request, response_callback, &response,
			 timeout_ms) != 0) {
		/* The transport may have registered the callback before publish failed. */
		tb_provision_cancel(client);
		osal_bin_sem_delete(response.done);
		result = TB_ENROLLMENT_ERR_REQUEST;
		complete(complete_cb, result, NULL, complete_user_data);
		return result;
	}
	start = osal_task_get_time_ms();
	(void)start;
	if (osal_bin_sem_timed_wait(response.done, timeout_ms) != OSAL_SUCCESS ||
	    !response.valid) {
		/* Clear the transport callback before the stack-owned response state ends. */
		tb_provision_cancel(client);
		memset(&response.credentials, 0, sizeof(response.credentials));
		result = TB_ENROLLMENT_ERR_RESPONSE;
		complete(complete_cb, result, NULL, complete_user_data);
		osal_bin_sem_delete(response.done);
		return result;
	}
	if (persist_cb != NULL && !persist_cb(&response.credentials, persist_user_data)) {
		memset(&response.credentials, 0, sizeof(response.credentials));
		result = TB_ENROLLMENT_ERR_PERSIST;
		complete(complete_cb, result, NULL, complete_user_data);
		osal_bin_sem_delete(response.done);
		return result;
	}
	complete(complete_cb, TB_ENROLLMENT_OK, &response.credentials,
		 complete_user_data);
	memset(&response.credentials, 0, sizeof(response.credentials));
	osal_bin_sem_delete(response.done);
	return TB_ENROLLMENT_OK;
}