/**
 *******************************************************************************
 * @file    tb_provision.c
 * @brief   ThingsBoard client – device provisioning implementation
 *******************************************************************************
 */

#include "tb_provision.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "osal_log.h"

#define TB_PROVISION_REQUEST_TOPIC "/provision/request"
#define TB_PROVISION_RESPONSE_TOPIC "/provision/response"
#define TB_PROVISION_TIMEOUT_MS 10000

static tb_provision_cb_t s_provision_cb = NULL;
static void *s_provision_user_data = NULL;
static bool s_provision_subscribed = false;

static void provision_response_handler(const char *topic, const char *payload,
				       size_t payload_len)
{
	(void)topic;
	(void)payload_len;

	if (s_provision_cb != NULL) {
		char *buf = malloc(payload_len + 1);
		if (buf != NULL) {
			memcpy(buf, payload, payload_len);
			buf[payload_len] = '\0';
			s_provision_cb(buf, s_provision_user_data);
			free(buf);
		} else {
			s_provision_cb(NULL, s_provision_user_data);
		}
		s_provision_cb = NULL;
		s_provision_user_data = NULL;
	}
}

int tb_provision_request(tb_client_t *client, const tb_provision_request_t *req,
			 tb_provision_cb_t cb, void *user_data,
			 uint32_t timeout_ms)
{
	if (client == NULL || req == NULL || cb == NULL) {
		return -1;
	}
	if (req->provision_device_key == NULL ||
	    req->provision_device_secret == NULL) {
		return -1;
	}

	/* Subscribe to response topic */
	if (!s_provision_subscribed) {
		int ret = tb_client_subscribe(
			client, TB_PROVISION_RESPONSE_TOPIC,
			provision_response_handler,
			timeout_ms > 0 ? timeout_ms : TB_PROVISION_TIMEOUT_MS);
		if (ret != 0) {
			return ret;
		}
		s_provision_subscribed = true;
	}

	s_provision_cb = cb;
	s_provision_user_data = user_data;

	/* Build provisioning request JSON */
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return -1;
	}

	if (req->device_name != NULL && req->device_name[0] != '\0') {
		cJSON_AddStringToObject(root, "deviceName", req->device_name);
	}
	cJSON_AddStringToObject(root, "provisionDeviceKey",
				req->provision_device_key);
	cJSON_AddStringToObject(root, "provisionDeviceSecret",
				req->provision_device_secret);

	if (req->credentials_type != NULL && req->credentials_type[0] != '\0') {
		cJSON_AddStringToObject(root, "credentialsType",
					req->credentials_type);
	}
	if (req->token != NULL && req->token[0] != '\0') {
		cJSON_AddStringToObject(root, "token", req->token);
	}
	if (req->username != NULL && req->username[0] != '\0') {
		cJSON_AddStringToObject(root, "username", req->username);
	}
	if (req->password != NULL && req->password[0] != '\0') {
		cJSON_AddStringToObject(root, "password", req->password);
	}
	if (req->client_id != NULL && req->client_id[0] != '\0') {
		cJSON_AddStringToObject(root, "clientId", req->client_id);
	}
	if (req->certificate_hash != NULL && req->certificate_hash[0] != '\0') {
		cJSON_AddStringToObject(root, "hash", req->certificate_hash);
	}

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (json == NULL) {
		return -1;
	}

	int ret = tb_client_publish(client, TB_PROVISION_REQUEST_TOPIC, json);
	cJSON_free(json);
	return ret;
}
