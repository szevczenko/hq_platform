/**
 *******************************************************************************
 * @file    tb_attributes.c
 * @brief   ThingsBoard client – attributes implementation
 *******************************************************************************
 */

#include "tb_attributes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "cJSON.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "osal_timer.h"

#define TB_ATTRIBUTE_TOPIC "v1/devices/me/attributes"
#define TB_ATTRIBUTE_REQUEST_TOPIC "v1/devices/me/attributes/request/%" PRIu32
#define TB_ATTRIBUTE_RESPONSE_TOPIC "v1/devices/me/attributes/response/"
#define TB_ATTRIBUTE_RESPONSE_SUB "v1/devices/me/attributes/response/+"

#define TB_ATTR_REQUEST_TIMEOUT_MS 5000
#define TB_ATTR_MAX_TOPIC_LEN 128
#define TB_ATTR_SWEEP_PERIOD_MS 50

/* Pending attribute request tracking */
typedef struct {
	uint32_t request_id;
	uint32_t deadline_ms;
	tb_attribute_response_cb_t cb;
	void *user_data;
	bool active;
} tb_attr_pending_t;

#define TB_ATTR_MAX_PENDING 8

static tb_attr_pending_t s_pending[TB_ATTR_MAX_PENDING];
static osal_mutex_id_t s_pending_mutex;
static bool s_pending_init = false;
static bool s_pending_init_failed = false;
static osal_timer_id_t s_pending_timer;
static bool s_response_subscribed = false;
static tb_client_t *s_owner_client = NULL;

/* Shared attribute subscription state */
static tb_shared_attribute_cb_t s_shared_cb = NULL;
static void *s_shared_user_data = NULL;
static bool s_shared_subscribed = false;

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
	return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void complete_pending_requests(tb_request_result_t result)
{
	tb_attribute_response_cb_t callbacks[TB_ATTR_MAX_PENDING] = { 0 };
	void *user_data[TB_ATTR_MAX_PENDING] = { 0 };
	int callback_count = 0;

	if (!s_pending_init) {
		return;
	}

	osal_mutex_take(s_pending_mutex);
	for (int i = 0; i < TB_ATTR_MAX_PENDING; i++) {
		if (!s_pending[i].active) {
			continue;
		}

		callbacks[callback_count] = s_pending[i].cb;
		user_data[callback_count] = s_pending[i].user_data;
		callback_count++;
		s_pending[i].active = false;
	}
	osal_mutex_give(s_pending_mutex);

	for (int i = 0; i < callback_count; i++) {
		if (callbacks[i] != NULL) {
			callbacks[i](result, NULL, user_data[i]);
		}
	}
}

static void pending_timeout_timer_cb(osal_timer_id_t timer_id)
{
	tb_attribute_response_cb_t callbacks[TB_ATTR_MAX_PENDING] = { 0 };
	void *user_data[TB_ATTR_MAX_PENDING] = { 0 };
	int callback_count = 0;
	uint32_t now_ms;

	(void)timer_id;

	if (!s_pending_init) {
		return;
	}

	now_ms = osal_task_get_time_ms();

	osal_mutex_take(s_pending_mutex);
	for (int i = 0; i < TB_ATTR_MAX_PENDING; i++) {
		if (!s_pending[i].active ||
		    !time_reached(now_ms, s_pending[i].deadline_ms)) {
			continue;
		}

		callbacks[callback_count] = s_pending[i].cb;
		user_data[callback_count] = s_pending[i].user_data;
		callback_count++;
		s_pending[i].active = false;
	}
	osal_mutex_give(s_pending_mutex);

	for (int i = 0; i < callback_count; i++) {
		if (callbacks[i] != NULL) {
			callbacks[i](TB_REQUEST_RESULT_TIMEOUT, NULL, user_data[i]);
		}
	}
}

static bool ensure_pending_init(void)
{
	if (s_pending_init) {
		return true;
	}
	if (s_pending_init_failed) {
		return false;
	}

	if (osal_mutex_create(&s_pending_mutex, "tb_attr") != OSAL_SUCCESS) {
		osal_log_error(
			"[tb_attr] Failed to create pending request mutex");
		s_pending_init_failed = true;
		return false;
	}

	memset(s_pending, 0, sizeof(s_pending));
	if (osal_timer_create(&s_pending_timer, "tb_attr_timeout",
			      TB_ATTR_SWEEP_PERIOD_MS, true,
			      pending_timeout_timer_cb, NULL, NULL,
			      0) != OSAL_SUCCESS) {
		osal_log_error("[tb_attr] Failed to create timeout sweep timer");
		(void)osal_mutex_delete(s_pending_mutex);
		s_pending_init_failed = true;
		return false;
	}
	(void)osal_timer_start(s_pending_timer, 0);
	s_pending_init = true;
	return true;
}

static bool ensure_owner_client(tb_client_t *client)
{
	if (client == NULL) {
		return false;
	}

	if (s_owner_client == NULL) {
		s_owner_client = client;
		return true;
	}

	if (s_owner_client != client) {
		/* Rebind module state to a new client session. This keeps attribute
		 * subscriptions functional after reconnect/re-init cycles. */
		s_owner_client = client;
		s_response_subscribed = false;
		s_shared_subscribed = false;
		if (s_pending_init) {
			osal_mutex_take(s_pending_mutex);
			memset(s_pending, 0, sizeof(s_pending));
			osal_mutex_give(s_pending_mutex);
		} else {
			memset(s_pending, 0, sizeof(s_pending));
		}
	}

	return true;
}

static void attr_response_handler(const char *topic, const char *payload,
				  size_t payload_len)
{
	/* Parse request ID from topic: v1/devices/me/attributes/response/{id} */
	const char *id_str = topic + strlen(TB_ATTRIBUTE_RESPONSE_TOPIC);
	uint32_t req_id = (uint32_t)strtoul(id_str, NULL, 10);
	tb_attribute_response_cb_t cb = NULL;
	void *ud = NULL;
	bool timed_out = false;
	uint32_t now_ms = osal_task_get_time_ms();

	osal_mutex_take(s_pending_mutex);
	for (int i = 0; i < TB_ATTR_MAX_PENDING; i++) {
		if (s_pending[i].active && s_pending[i].request_id == req_id) {
			cb = s_pending[i].cb;
			ud = s_pending[i].user_data;
			timed_out = time_reached(now_ms, s_pending[i].deadline_ms);
			s_pending[i].active = false;
			osal_mutex_give(s_pending_mutex);
			break;
		}
	}
	if (cb == NULL) {
		osal_mutex_give(s_pending_mutex);
		return;
	}

	if (timed_out) {
		cb(TB_REQUEST_RESULT_TIMEOUT, NULL, ud);
		return;
	}

	char *buf = malloc(payload_len + 1);
	if (buf != NULL) {
		memcpy(buf, payload, payload_len);
		buf[payload_len] = '\0';
		cb(TB_REQUEST_RESULT_SUCCESS, buf, ud);
		free(buf);
	} else {
		cb(TB_REQUEST_RESULT_ERROR, NULL, ud);
	}
}

static void shared_attr_handler(const char *topic, const char *payload,
				size_t payload_len)
{
	(void)topic;
	if (s_shared_cb != NULL) {
		char *buf = malloc(payload_len + 1);
		if (buf != NULL) {
			memcpy(buf, payload, payload_len);
			buf[payload_len] = '\0';
			s_shared_cb(buf, s_shared_user_data);
			free(buf);
		} else {
			s_shared_cb(NULL, s_shared_user_data);
		}
	}
}

void tb_attributes_handle_disconnect(tb_client_t *client)
{
	if (client == NULL || client != s_owner_client || !s_pending_init) {
		return;
	}

	complete_pending_requests(TB_REQUEST_RESULT_CANCELLED);
}

static int send_kv_attribute(tb_client_t *client, cJSON *root)
{
	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (json == NULL) {
		return -1;
	}
	int ret = tb_client_publish(client, TB_ATTRIBUTE_TOPIC, json);
	cJSON_free(json);
	return ret;
}

void tb_attributes_deinit(tb_client_t *client)
{
	if (client == NULL || client != s_owner_client) {
		return;
	}

	if (s_pending_init) {
		complete_pending_requests(TB_REQUEST_RESULT_CANCELLED);
	}

	s_response_subscribed = false;
	s_shared_subscribed = false;
	s_shared_cb = NULL;
	s_shared_user_data = NULL;
	s_owner_client = NULL;
}

int tb_attributes_send_int(tb_client_t *client, const char *key, int64_t value)
{
	if (client == NULL || key == NULL) {
		return -1;
	}
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return -1;
	}
	cJSON_AddNumberToObject(root, key, (double)value);
	return send_kv_attribute(client, root);
}

int tb_attributes_send_double(tb_client_t *client, const char *key,
			      double value)
{
	if (client == NULL || key == NULL) {
		return -1;
	}
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return -1;
	}
	cJSON_AddNumberToObject(root, key, value);
	return send_kv_attribute(client, root);
}

int tb_attributes_send_bool(tb_client_t *client, const char *key, bool value)
{
	if (client == NULL || key == NULL) {
		return -1;
	}
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return -1;
	}
	cJSON_AddBoolToObject(root, key, value);
	return send_kv_attribute(client, root);
}

int tb_attributes_send_string(tb_client_t *client, const char *key,
			      const char *value)
{
	if (client == NULL || key == NULL || value == NULL) {
		return -1;
	}
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return -1;
	}
	cJSON_AddStringToObject(root, key, value);
	return send_kv_attribute(client, root);
}

int tb_attributes_send_json(tb_client_t *client, const char *json)
{
	if (client == NULL || json == NULL) {
		return -1;
	}
	return tb_client_publish(client, TB_ATTRIBUTE_TOPIC, json);
}

static int subscribe_response_topic(tb_client_t *client)
{
	if (s_response_subscribed) {
		return 0;
	}

	int ret = tb_client_subscribe(client, TB_ATTRIBUTE_RESPONSE_SUB,
				      attr_response_handler,
				      TB_ATTR_REQUEST_TIMEOUT_MS);
	if (ret == 0) {
		s_response_subscribed = true;
		return 0;
	}

	return ret;
}

static int attr_request_common(tb_client_t *client, const char *keys[],
			       size_t num_keys, const char *key_type,
			       tb_attribute_response_cb_t cb, void *user_data,
			       uint32_t timeout_ms)
{
	if (client == NULL || keys == NULL || num_keys == 0 || cb == NULL) {
		return -1;
	}

	if (!ensure_owner_client(client)) {
		return -1;
	}

	if (!ensure_pending_init()) {
		return -1;
	}

	int ret = subscribe_response_topic(client);
	if (ret != 0) {
		return ret;
	}

	/* Build comma-separated key list */
	char keys_str[512] = { 0 };
	size_t offset = 0;
	for (size_t i = 0; i < num_keys; i++) {
		if (i > 0) {
			keys_str[offset++] = ',';
		}
		size_t klen = strlen(keys[i]);
		if (offset + klen >= sizeof(keys_str) - 1) {
			break;
		}
		memcpy(keys_str + offset, keys[i], klen);
		offset += klen;
	}
	keys_str[offset] = '\0';

	uint32_t req_id = tb_client_get_next_request_id(client);
	uint32_t effective_timeout =
		timeout_ms > 0 ? timeout_ms : TB_ATTR_REQUEST_TIMEOUT_MS;

	/* Register pending request */
	/* TODO(osal): use timed mutex lock once OSAL exposes mutex take with timeout.
     * This path should respect timeout_ms instead of blocking indefinitely. */
	osal_mutex_take(s_pending_mutex);
	int slot = -1;
	for (int i = 0; i < TB_ATTR_MAX_PENDING; i++) {
		if (!s_pending[i].active) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		osal_mutex_give(s_pending_mutex);
		osal_log_warning("[tb_attr] No free pending request slots");
		return -1;
	}
	s_pending[slot].request_id = req_id;
	s_pending[slot].deadline_ms =
		osal_task_get_time_ms() + effective_timeout;
	s_pending[slot].cb = cb;
	s_pending[slot].user_data = user_data;
	s_pending[slot].active = true;
	osal_mutex_give(s_pending_mutex);

	/* Build request JSON: {"clientKeys":"key1,key2"} or {"sharedKeys":"key1,key2"} */
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		osal_mutex_take(s_pending_mutex);
		s_pending[slot].active = false;
		tb_attribute_response_cb_t slot_cb = s_pending[slot].cb;
		void *slot_ud = s_pending[slot].user_data;
		osal_mutex_give(s_pending_mutex);
		if (slot_cb != NULL) {
			slot_cb(TB_REQUEST_RESULT_ERROR, NULL, slot_ud);
		}
		return -1;
	}
	cJSON_AddStringToObject(root, key_type, keys_str);

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (json == NULL) {
		osal_mutex_take(s_pending_mutex);
		s_pending[slot].active = false;
		tb_attribute_response_cb_t slot_cb = s_pending[slot].cb;
		void *slot_ud = s_pending[slot].user_data;
		osal_mutex_give(s_pending_mutex);
		if (slot_cb != NULL) {
			slot_cb(TB_REQUEST_RESULT_ERROR, NULL, slot_ud);
		}
		return -1;
	}

	/* Publish to request topic */
	char topic[TB_ATTR_MAX_TOPIC_LEN];
	snprintf(topic, sizeof(topic), TB_ATTRIBUTE_REQUEST_TOPIC, req_id);

	ret = tb_client_publish(client, topic, json);
	cJSON_free(json);

	if (ret != 0) {
		osal_mutex_take(s_pending_mutex);
		s_pending[slot].active = false;
		tb_attribute_response_cb_t slot_cb = s_pending[slot].cb;
		void *slot_ud = s_pending[slot].user_data;
		osal_mutex_give(s_pending_mutex);
		if (slot_cb != NULL) {
			slot_cb(TB_REQUEST_RESULT_ERROR, NULL, slot_ud);
		}
	}

	return ret;
}

int tb_attributes_request_client(tb_client_t *client, const char *keys[],
				 size_t num_keys, tb_attribute_response_cb_t cb,
				 void *user_data, uint32_t timeout_ms)
{
	return attr_request_common(client, keys, num_keys, "clientKeys", cb,
				   user_data, timeout_ms);
}

int tb_attributes_request_shared(tb_client_t *client, const char *keys[],
				 size_t num_keys, tb_attribute_response_cb_t cb,
				 void *user_data, uint32_t timeout_ms)
{
	return attr_request_common(client, keys, num_keys, "sharedKeys", cb,
				   user_data, timeout_ms);
}

int tb_attributes_subscribe(tb_client_t *client, tb_shared_attribute_cb_t cb,
			    void *user_data)
{
	if (client == NULL || cb == NULL) {
		return -1;
	}

	if (!ensure_owner_client(client)) {
		return -1;
	}

	s_shared_cb = cb;
	s_shared_user_data = user_data;

	if (!s_shared_subscribed) {
		int ret = tb_client_subscribe(client, TB_ATTRIBUTE_TOPIC,
					      shared_attr_handler,
					      TB_ATTR_REQUEST_TIMEOUT_MS);
		if (ret == 0) {
			s_shared_subscribed = true;
			return 0;
		}

		return ret;
	}
	return 0;
}

int tb_attributes_unsubscribe(tb_client_t *client)
{
	if (client == NULL) {
		return -1;
	}

	if (!ensure_owner_client(client)) {
		return -1;
	}

	s_shared_cb = NULL;
	s_shared_user_data = NULL;

	if (s_shared_subscribed) {
		s_shared_subscribed = false;
		return tb_client_unsubscribe(client, TB_ATTRIBUTE_TOPIC,
					     TB_ATTR_REQUEST_TIMEOUT_MS);
	}
	return 0;
}
