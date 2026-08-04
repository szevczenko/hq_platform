/**
 *******************************************************************************
 * @file    tb_firmware_update.c
 * @brief   ThingsBoard client – firmware update implementation
 *******************************************************************************
 */

#include "tb_firmware_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "cJSON.h"
#include "osal_log.h"
#include "osal_ota.h"
#include "tb_attributes.h"
#include "tb_telemetry.h"

#define TB_FW_ATTR_TITLE "fw_title"
#define TB_FW_ATTR_VERSION "fw_version"
#define TB_FW_ATTR_CHECKSUM "fw_checksum"
#define TB_FW_ATTR_CHECKSUM_ALG "fw_checksum_algorithm"
#define TB_FW_ATTR_SIZE "fw_size"

#define TB_FW_TELEM_STATE "fw_state"
#define TB_FW_TELEM_ERROR "fw_error"
#define TB_FW_TELEM_CUR_TITLE "current_fw_title"
#define TB_FW_TELEM_CUR_VERSION "current_fw_version"

#define TB_FW_REQ_TOPIC_FMT "v2/fw/request/%" PRIu32 "/chunk/%" PRIu32
#define TB_FW_RESP_TOPIC_SUB "v2/fw/response/+/chunk/+"
#define TB_FW_RESP_TOPIC_PREFIX "v2/fw/response/"

#define TB_FW_TIMEOUT_MS 5000
#define TB_FW_STR_LEN 128

typedef enum {
	TB_FW_STATE_IDLE = 0,
	TB_FW_STATE_DOWNLOADING,
	TB_FW_STATE_DOWNLOADED,
	TB_FW_STATE_VERIFIED,
	TB_FW_STATE_UPDATING,
	TB_FW_STATE_UPDATED,
	TB_FW_STATE_FAILED
} tb_fw_state_t;

typedef struct {
	tb_client_t *client;
	bool initialized;
	bool subscribed;
	bool in_progress;

	uint32_t chunk_size;
	uint32_t request_id;
	uint32_t next_chunk;

	size_t target_size;
	size_t downloaded_size;

	char current_title[TB_FW_STR_LEN];
	char current_version[TB_FW_STR_LEN];

	char target_title[TB_FW_STR_LEN];
	char target_version[TB_FW_STR_LEN];
	char target_checksum[TB_FW_STR_LEN];
	char target_checksum_alg[TB_FW_STR_LEN];

	tb_firmware_applied_cb_t on_applied;
	void *user_data;
} tb_fw_ctx_t;

static tb_fw_ctx_t s_fw;

static const char *fw_state_to_string(tb_fw_state_t state)
{
	switch (state) {
	case TB_FW_STATE_IDLE:
		return "IDLE";
	case TB_FW_STATE_DOWNLOADING:
		return "DOWNLOADING";
	case TB_FW_STATE_DOWNLOADED:
		return "DOWNLOADED";
	case TB_FW_STATE_VERIFIED:
		return "VERIFIED";
	case TB_FW_STATE_UPDATING:
		return "UPDATING";
	case TB_FW_STATE_UPDATED:
		return "UPDATED";
	case TB_FW_STATE_FAILED:
	default:
		return "FAILED";
	}
}

static int fw_report_state(tb_fw_state_t state, const char *error)
{
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return -1;
	}

	cJSON_AddStringToObject(root, TB_FW_TELEM_CUR_TITLE,
				s_fw.current_title);
	cJSON_AddStringToObject(root, TB_FW_TELEM_CUR_VERSION,
				s_fw.current_version);
	cJSON_AddStringToObject(root, TB_FW_TELEM_STATE,
				fw_state_to_string(state));
	cJSON_AddStringToObject(root, TB_FW_TELEM_ERROR, error ? error : "");

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (json == NULL) {
		return -1;
	}

	int ret = tb_telemetry_send_json(s_fw.client, json);
	cJSON_free(json);
	return ret;
}

static int fw_request_chunk(void)
{
	char topic[128];
	char payload[16];

	snprintf(topic, sizeof(topic), TB_FW_REQ_TOPIC_FMT, s_fw.request_id,
		 s_fw.next_chunk);

	uint32_t req_size = s_fw.chunk_size;
	size_t remaining = (s_fw.target_size > s_fw.downloaded_size) ?
				   (s_fw.target_size - s_fw.downloaded_size) :
				   0;
	if (req_size == 0 || remaining == 0) {
		payload[0] = '\0';
	} else {
		if ((size_t)req_size > remaining) {
			req_size = (uint32_t)remaining;
		}
		snprintf(payload, sizeof(payload), "%" PRIu32, req_size);
	}

	return tb_client_publish(s_fw.client, topic, payload);
}

static void fw_fail(const char *reason)
{
	osal_ota_abort();
	s_fw.in_progress = false;
	fw_report_state(TB_FW_STATE_FAILED,
			reason ? reason : "firmware update failed");
}

static bool fw_parse_response_topic(const char *topic, uint32_t *request_id,
				    uint32_t *chunk)
{
	const char *prefix = TB_FW_RESP_TOPIC_PREFIX;
	size_t prefix_len = strlen(prefix);
	if (strncmp(topic, prefix, prefix_len) != 0) {
		return false;
	}

	const char *p = topic + prefix_len;
	char *end_ptr = NULL;
	unsigned long req = strtoul(p, &end_ptr, 10);
	if (end_ptr == p || strncmp(end_ptr, "/chunk/", 7) != 0) {
		return false;
	}

	p = end_ptr + 7;
	unsigned long chk = strtoul(p, &end_ptr, 10);
	if (end_ptr == p || *end_ptr != '\0') {
		return false;
	}

	*request_id = (uint32_t)req;
	*chunk = (uint32_t)chk;
	return true;
}

static void fw_chunk_handler(const char *topic, const char *payload,
			     size_t payload_len)
{
	uint32_t req_id = 0;
	uint32_t chunk = 0;

	if (!s_fw.in_progress) {
		return;
	}
	if (!fw_parse_response_topic(topic, &req_id, &chunk)) {
		return;
	}
	if (req_id != s_fw.request_id || chunk != s_fw.next_chunk) {
		return;
	}

	if (osal_ota_write((const uint8_t *)payload, payload_len) !=
	    OSAL_SUCCESS) {
		fw_fail("ota write failed");
		return;
	}

	s_fw.downloaded_size += payload_len;
	s_fw.next_chunk++;

	if (s_fw.downloaded_size >= s_fw.target_size) {
		if (s_fw.downloaded_size != s_fw.target_size) {
			fw_fail("firmware size mismatch");
			return;
		}

		fw_report_state(TB_FW_STATE_DOWNLOADED, NULL);
		fw_report_state(TB_FW_STATE_VERIFIED, NULL);
		fw_report_state(TB_FW_STATE_UPDATING, NULL);

		if (osal_ota_finish(true) != OSAL_SUCCESS) {
			fw_fail("ota finalize failed");
			return;
		}

		strncpy(s_fw.current_title, s_fw.target_title,
			sizeof(s_fw.current_title) - 1);
		strncpy(s_fw.current_version, s_fw.target_version,
			sizeof(s_fw.current_version) - 1);
		s_fw.current_title[sizeof(s_fw.current_title) - 1] = '\0';
		s_fw.current_version[sizeof(s_fw.current_version) - 1] = '\0';

		s_fw.in_progress = false;
		fw_report_state(TB_FW_STATE_UPDATED, NULL);

		if (s_fw.on_applied != NULL) {
			s_fw.on_applied(s_fw.current_title,
					s_fw.current_version, s_fw.user_data);
		}
		return;
	}

	if (fw_request_chunk() != 0) {
		fw_fail("chunk request failed");
	}
}

static int fw_start_download(void)
{
	osal_ota_descriptor_t ota_desc = {
		.title = s_fw.target_title,
		.version = s_fw.target_version,
		.checksum = s_fw.target_checksum,
		.checksum_algorithm = s_fw.target_checksum_alg,
		.total_size = s_fw.target_size,
	};

	if (osal_ota_begin(&ota_desc) != OSAL_SUCCESS) {
		fw_fail("ota begin failed");
		return -1;
	}

	s_fw.request_id = tb_client_get_next_request_id(s_fw.client);
	s_fw.next_chunk = 0;
	s_fw.downloaded_size = 0;
	s_fw.in_progress = true;

	fw_report_state(TB_FW_STATE_DOWNLOADING, NULL);

	return fw_request_chunk();
}

static void fw_attributes_cb(const char *response_json, void *user_data)
{
	(void)user_data;

	if (response_json == NULL || s_fw.in_progress) {
		return;
	}

	cJSON *root = cJSON_Parse(response_json);
	if (root == NULL) {
		return;
	}

	cJSON *shared = cJSON_GetObjectItemCaseSensitive(root, "shared");
	if (!cJSON_IsObject(shared)) {
		cJSON_Delete(root);
		return;
	}

	cJSON *source = shared;

	cJSON *fw_title =
		cJSON_GetObjectItemCaseSensitive(source, TB_FW_ATTR_TITLE);
	cJSON *fw_version =
		cJSON_GetObjectItemCaseSensitive(source, TB_FW_ATTR_VERSION);
	cJSON *fw_size =
		cJSON_GetObjectItemCaseSensitive(source, TB_FW_ATTR_SIZE);
	cJSON *fw_checksum =
		cJSON_GetObjectItemCaseSensitive(source, TB_FW_ATTR_CHECKSUM);
	cJSON *fw_checksum_alg = cJSON_GetObjectItemCaseSensitive(
		source, TB_FW_ATTR_CHECKSUM_ALG);

	if (!cJSON_IsString(fw_title) || !cJSON_IsString(fw_version) ||
	    !cJSON_IsNumber(fw_size)) {
		cJSON_Delete(root);
		return;
	}

	if (strcmp(s_fw.current_title, fw_title->valuestring) == 0 &&
	    strcmp(s_fw.current_version, fw_version->valuestring) == 0) {
		osal_log_info("Firmware is up to date: %s v%s",
			      s_fw.current_title, s_fw.current_version);
		cJSON_Delete(root);
		return;
	}

	strncpy(s_fw.target_title, fw_title->valuestring,
		sizeof(s_fw.target_title) - 1);
	strncpy(s_fw.target_version, fw_version->valuestring,
		sizeof(s_fw.target_version) - 1);
	s_fw.target_title[sizeof(s_fw.target_title) - 1] = '\0';
	s_fw.target_version[sizeof(s_fw.target_version) - 1] = '\0';

	if (cJSON_IsString(fw_checksum) && fw_checksum->valuestring != NULL) {
		strncpy(s_fw.target_checksum, fw_checksum->valuestring,
			sizeof(s_fw.target_checksum) - 1);
		s_fw.target_checksum[sizeof(s_fw.target_checksum) - 1] = '\0';
	} else {
		s_fw.target_checksum[0] = '\0';
	}

	if (cJSON_IsString(fw_checksum_alg) &&
	    fw_checksum_alg->valuestring != NULL) {
		strncpy(s_fw.target_checksum_alg, fw_checksum_alg->valuestring,
			sizeof(s_fw.target_checksum_alg) - 1);
		s_fw.target_checksum_alg[sizeof(s_fw.target_checksum_alg) - 1] =
			'\0';
	} else {
		s_fw.target_checksum_alg[0] = '\0';
	}

	s_fw.target_size = (size_t)fw_size->valuedouble;

	cJSON_Delete(root);

	if (s_fw.target_size == 0) {
		fw_fail("invalid firmware size");
		return;
	}

	if (fw_start_download() != 0) {
		fw_fail("cannot start firmware download");
	}
}

int tb_firmware_update_init(tb_client_t *client,
			    const tb_firmware_update_config_t *config)
{
	if (client == NULL || config == NULL) {
		return -1;
	}

	memset(&s_fw, 0, sizeof(s_fw));
	s_fw.client = client;
	s_fw.chunk_size = config->chunk_size;
	s_fw.on_applied = config->on_applied;
	s_fw.user_data = config->user_data;

	strncpy(s_fw.current_title,
		config->current_title ? config->current_title : "Initial",
		sizeof(s_fw.current_title) - 1);
	strncpy(s_fw.current_version,
		config->current_version ? config->current_version : "v0",
		sizeof(s_fw.current_version) - 1);
	s_fw.current_title[sizeof(s_fw.current_title) - 1] = '\0';
	s_fw.current_version[sizeof(s_fw.current_version) - 1] = '\0';

	int rc = tb_client_subscribe(client, TB_FW_RESP_TOPIC_SUB,
				     fw_chunk_handler, TB_FW_TIMEOUT_MS);
	if (rc != 0) {
		return rc;
	}

	s_fw.subscribed = true;
	s_fw.initialized = true;
	fw_report_state(TB_FW_STATE_IDLE, NULL);
	return 0;
}

int tb_firmware_update_request_check(tb_client_t *client)
{
	static const char *shared_keys[] = {
		TB_FW_ATTR_CHECKSUM, TB_FW_ATTR_CHECKSUM_ALG, TB_FW_ATTR_SIZE,
		TB_FW_ATTR_TITLE,    TB_FW_ATTR_VERSION,
	};

	if (!s_fw.initialized || client == NULL || client != s_fw.client) {
		return -1;
	}

	return tb_attributes_request_shared(
		client, shared_keys,
		sizeof(shared_keys) / sizeof(shared_keys[0]), fw_attributes_cb,
		NULL, TB_FW_TIMEOUT_MS);
}

bool tb_firmware_update_is_in_progress(void)
{
	return s_fw.in_progress;
}

void tb_firmware_update_deinit(tb_client_t *client)
{
	if (client == NULL || !s_fw.initialized || client != s_fw.client) {
		return;
	}

	if (s_fw.subscribed) {
		tb_client_unsubscribe(client, TB_FW_RESP_TOPIC_SUB,
				      TB_FW_TIMEOUT_MS);
	}

	if (s_fw.in_progress) {
		osal_ota_abort();
	}

	memset(&s_fw, 0, sizeof(s_fw));
}
