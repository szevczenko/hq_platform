/**
 *******************************************************************************
 * @file    tb_provision.c
 * @brief   ThingsBoard client – device provisioning implementation
 *******************************************************************************
 */

#include "tb_provision.h"
#include "tb_provision_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "osal_log.h"
#include "osal_bin_sem.h"
#include "osal_mutex.h"

#define TB_PROVISION_REQUEST_TOPIC "/provision/request"
#define TB_PROVISION_RESPONSE_TOPIC "/provision/response"
#define TB_PROVISION_TIMEOUT_MS 10000

static tb_provision_cb_t s_provision_cb = NULL;
static void *s_provision_user_data = NULL;
static bool s_provision_subscribed = false;
static tb_client_t *s_owner_client = NULL;
static osal_mutex_id_t s_provision_lock = NULL;
static osal_bin_sem_id_t s_callback_done = NULL;
static bool s_callback_in_flight = false;

static bool provision_lock_init(void)
{
	if (s_provision_lock != NULL) {
		return true;
	}
	return osal_mutex_create(&s_provision_lock, "tb_provision") == OSAL_SUCCESS &&
		osal_bin_sem_create(&s_callback_done, "tb_provision_done",
		                    OSAL_SEM_EMPTY) == OSAL_SUCCESS;
}

static void provision_response_handler(const char *topic, const char *payload,
				       size_t payload_len)
{
	(void)topic;
	(void)payload_len;

	if (!provision_lock_init() || osal_mutex_take(s_provision_lock) != OSAL_SUCCESS) {
		return;
	}
	if (s_provision_cb != NULL) {
		tb_provision_cb_t callback = s_provision_cb;
		void *callback_data = s_provision_user_data;
		s_callback_in_flight = true;
		osal_mutex_give(s_provision_lock);
		char *buf = malloc(payload_len + 1);
		if (buf != NULL) {
			memcpy(buf, payload, payload_len);
			buf[payload_len] = '\0';
			callback(buf, callback_data);
			free(buf);
		} else {
			callback(NULL, callback_data);
		}
		osal_mutex_take(s_provision_lock);
		s_provision_cb = NULL;
		s_provision_user_data = NULL;
		s_callback_in_flight = false;
		osal_bin_sem_give(s_callback_done);
	}
	osal_mutex_give(s_provision_lock);
}

int tb_provision_request(tb_client_t *client, const tb_provision_request_t *req,
			 tb_provision_cb_t cb, void *user_data,
			 uint32_t timeout_ms)
{
	if (client == NULL || req == NULL || cb == NULL || !provision_lock_init()) {
		return -1;
	}
	if (req->provision_device_key == NULL ||
	    req->provision_device_secret == NULL) {
		return -1;
	}

	if (osal_mutex_take(s_provision_lock) != OSAL_SUCCESS) {
		return -1;
	}
	if (s_owner_client != client) {
		s_owner_client = client;
		s_provision_subscribed = false;
	}
	if (s_callback_in_flight) {
		osal_mutex_give(s_provision_lock);
		return -1;
	}
	s_provision_cb = cb;
	s_provision_user_data = user_data;
	osal_mutex_give(s_provision_lock);

	/* Subscribe to response topic */
	if (!s_provision_subscribed) {
		int ret = tb_client_subscribe(
			client, TB_PROVISION_RESPONSE_TOPIC,
			provision_response_handler,
			timeout_ms > 0 ? timeout_ms : TB_PROVISION_TIMEOUT_MS);
		if (ret != 0) {
			tb_provision_cancel(client);
			return ret;
		}
		s_provision_subscribed = true;
	}

	/* Build provisioning request JSON */
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		tb_provision_cancel(client);
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
		tb_provision_cancel(client);
		return -1;
	}

	int ret = tb_client_publish(client, TB_PROVISION_REQUEST_TOPIC, json);
	cJSON_free(json);
	if (ret != 0) {
		tb_provision_cancel(client);
	}
	return ret;
}

void tb_provision_deinit(tb_client_t *client)
{
	if (client == NULL || !provision_lock_init() ||
	    osal_mutex_take(s_provision_lock) != OSAL_SUCCESS) {
		return;
	}
	if (s_owner_client == NULL || s_owner_client != client) {
		osal_mutex_give(s_provision_lock);
		return;
	}

	s_provision_cb = NULL;
	s_provision_user_data = NULL;
	s_provision_subscribed = false;
	s_owner_client = NULL;
	bool wait_for_callback = s_callback_in_flight;
	osal_mutex_give(s_provision_lock);
	if (wait_for_callback) {
		(void)osal_bin_sem_take(s_callback_done);
	}
}

void tb_provision_cancel(tb_client_t *client)
{
	if (client == NULL || !provision_lock_init() ||
	    osal_mutex_take(s_provision_lock) != OSAL_SUCCESS) {
		return;
	}
	if (s_owner_client != client) {
		osal_mutex_give(s_provision_lock);
		return;
	}

	/* Unregister only the pending callback/user_data; the subscription
	 * itself stays intact so a subsequent request on the same client
	 * does not need to re-subscribe. */
	s_provision_cb = NULL;
	s_provision_user_data = NULL;
	bool wait_for_callback = s_callback_in_flight;
	osal_mutex_give(s_provision_lock);
	if (wait_for_callback) {
		(void)osal_bin_sem_take(s_callback_done);
	}
}
