/**
 *******************************************************************************
 * @file    tb_firmware_update.c
 * @brief   ThingsBoard client – firmware update implementation
 *******************************************************************************
 */

#include "tb_firmware_update.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>

#include "cJSON.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_ota.h"
#include "osal_ota_state.h"
#include "tb_attributes.h"
#include "tb_telemetry.h"

#define TB_FW_ATTR_TITLE "fw_title"
#define TB_FW_ATTR_VERSION "fw_version"
#define TB_FW_ATTR_CHECKSUM "fw_checksum"
#define TB_FW_ATTR_CHECKSUM_ALG "fw_checksum_algorithm"
#define TB_FW_ATTR_SIZE "fw_size"

#define TB_FW_TELEM_STATE "fw_state"
#define TB_FW_TELEM_ERROR "fw_error"
#define TB_FW_TELEM_TITLE "fw_title"
#define TB_FW_TELEM_VERSION "fw_version"
#define TB_FW_TELEM_CURRENT_TITLE "current_fw_title"
#define TB_FW_TELEM_CURRENT_VERSION "current_fw_version"

#define TB_FW_REQ_TOPIC_FMT "v2/fw/request/%" PRIu32 "/chunk/%" PRIu32
#define TB_FW_RESP_TOPIC_SUB "v2/fw/response/+/chunk/+"
#define TB_FW_RESP_TOPIC_PREFIX "v2/fw/response/"

#define TB_FW_TIMEOUT_MS 5000
#define TB_FW_STR_LEN TB_FIRMWARE_UPDATE_STR_LEN
#define TB_FW_SHA256_HEX_LEN 64

typedef tb_firmware_update_state_t tb_fw_state_t;
#define TB_FW_STATE_IDLE TB_FIRMWARE_UPDATE_STATE_IDLE
#define TB_FW_STATE_DOWNLOADING TB_FIRMWARE_UPDATE_STATE_DOWNLOADING
#define TB_FW_STATE_DOWNLOADED TB_FIRMWARE_UPDATE_STATE_DOWNLOADED
#define TB_FW_STATE_VERIFIED TB_FIRMWARE_UPDATE_STATE_VERIFIED
#define TB_FW_STATE_UPDATING TB_FIRMWARE_UPDATE_STATE_UPDATING
#define TB_FW_STATE_UPDATED TB_FIRMWARE_UPDATE_STATE_UPDATED
#define TB_FW_STATE_FAILED TB_FIRMWARE_UPDATE_STATE_FAILED

typedef struct {
	tb_client_t *client;
	bool initialized;
	bool subscribed;
	bool in_progress;
	bool reboot_pending;

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

	tb_fw_state_t state;
	char last_error[TB_FW_STR_LEN];

	/* Chunk timeout bookkeeping (supervisor-driven via poll()). */
	uint32_t chunk_timeout_ms;
	uint32_t max_chunk_retries;
	uint32_t chunk_retries;
	uint32_t progress_seq;
	uint32_t poll_seen_seq;
	uint32_t chunk_deadline_ms;
	bool poll_armed;

	tb_firmware_applied_cb_t on_applied;
	tb_firmware_reboot_required_cb_t on_reboot_required;
	void *user_data;
} tb_fw_ctx_t;

static tb_fw_ctx_t s_fw;
/* Outlives s_fw resets: guards s_fw against the transport thread. */
static osal_mutex_id_t s_fw_lock;

static bool fw_lock(void)
{
	return s_fw_lock != NULL && osal_mutex_take(s_fw_lock) == OSAL_SUCCESS;
}

static void fw_unlock(void)
{
	(void)osal_mutex_give(s_fw_lock);
}

static osal_ota_download_state_t fw_state_to_persisted_state(
	tb_fw_state_t state)
{
	switch (state) {
	case TB_FW_STATE_IDLE:
		return OSAL_OTA_DOWNLOAD_STATE_IDLE;
	case TB_FW_STATE_DOWNLOADING:
		return OSAL_OTA_DOWNLOAD_STATE_DOWNLOADING;
	case TB_FW_STATE_DOWNLOADED:
		return OSAL_OTA_DOWNLOAD_STATE_DOWNLOADED;
	case TB_FW_STATE_VERIFIED:
		return OSAL_OTA_DOWNLOAD_STATE_VERIFIED;
	case TB_FW_STATE_UPDATING:
		return OSAL_OTA_DOWNLOAD_STATE_UPDATING;
	case TB_FW_STATE_UPDATED:
		return OSAL_OTA_DOWNLOAD_STATE_UPDATED;
	case TB_FW_STATE_FAILED:
	default:
		return OSAL_OTA_DOWNLOAD_STATE_FAILED;
	}
}

static tb_fw_state_t fw_state_from_persisted_state(
	osal_ota_download_state_t state)
{
	switch (state) {
	case OSAL_OTA_DOWNLOAD_STATE_IDLE:
		return TB_FW_STATE_IDLE;
	case OSAL_OTA_DOWNLOAD_STATE_DOWNLOADING:
		return TB_FW_STATE_DOWNLOADING;
	case OSAL_OTA_DOWNLOAD_STATE_DOWNLOADED:
		return TB_FW_STATE_DOWNLOADED;
	case OSAL_OTA_DOWNLOAD_STATE_VERIFIED:
		return TB_FW_STATE_VERIFIED;
	case OSAL_OTA_DOWNLOAD_STATE_UPDATING:
		return TB_FW_STATE_UPDATING;
	case OSAL_OTA_DOWNLOAD_STATE_UPDATED:
		return TB_FW_STATE_UPDATED;
	case OSAL_OTA_DOWNLOAD_STATE_FAILED:
	default:
		return TB_FW_STATE_FAILED;
	}
}

static const char *fw_effective_title(void)
{
	return s_fw.target_title[0] != '\0' ? s_fw.target_title : s_fw.current_title;
}

static const char *fw_effective_version(void)
{
	return s_fw.target_version[0] != '\0' ? s_fw.target_version : s_fw.current_version;
}

static void fw_persist_state(tb_fw_state_t state, const char *error)
{
	osal_ota_state_t persisted_state;
	osal_status_t status;

	memset(&persisted_state, 0, sizeof(persisted_state));
	strncpy(persisted_state.title, fw_effective_title(),
		sizeof(persisted_state.title) - 1);
	strncpy(persisted_state.version, fw_effective_version(),
		sizeof(persisted_state.version) - 1);
	strncpy(persisted_state.checksum, s_fw.target_checksum,
		sizeof(persisted_state.checksum) - 1);
	persisted_state.download_state = fw_state_to_persisted_state(state);
	if (error != NULL) {
		strncpy(persisted_state.last_error, error,
			sizeof(persisted_state.last_error) - 1);
	}

	status = osal_ota_state_save(&persisted_state);
	if (status != OSAL_SUCCESS) {
		osal_log_warning("[tb_fw] Failed to persist OTA state: %d", status);
	}
}

static void fw_clear_persisted_state(void)
{
	osal_status_t status = osal_ota_state_clear();
	if (status != OSAL_SUCCESS) {
		osal_log_warning("[tb_fw] Failed to clear OTA state: %d", status);
	}
}

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

static bool fw_checksum_algorithm_is_supported(const char *algorithm)
{
	return algorithm != NULL && strcmp(algorithm, "SHA256") == 0;
}

static bool fw_checksum_is_valid_hex(const char *checksum)
{
	if (checksum == NULL || strlen(checksum) != TB_FW_SHA256_HEX_LEN) {
		return false;
	}

	for (size_t index = 0; index < TB_FW_SHA256_HEX_LEN; index++) {
		if (!isxdigit((unsigned char)checksum[index])) {
			return false;
		}
	}

	return true;
}

static int fw_report_state(tb_fw_state_t state, const char *error)
{
	s_fw.state = state;
	strncpy(s_fw.last_error, error ? error : "",
		sizeof(s_fw.last_error) - 1);
	s_fw.last_error[sizeof(s_fw.last_error) - 1] = '\0';

	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return -1;
	}

	cJSON_AddStringToObject(root, TB_FW_TELEM_TITLE,
				s_fw.current_title);
	cJSON_AddStringToObject(root, TB_FW_TELEM_VERSION,
				s_fw.current_version);
	cJSON_AddStringToObject(root, TB_FW_TELEM_CURRENT_TITLE,
				s_fw.current_title);
	cJSON_AddStringToObject(root, TB_FW_TELEM_CURRENT_VERSION,
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

	/* ThingsBoard serves offset chunk_size * chunk and truncates the last
	 * chunk itself, so the requested size must never shrink. */
	if (s_fw.chunk_size == 0) {
		payload[0] = '\0';
	} else {
		snprintf(payload, sizeof(payload), "%" PRIu32, s_fw.chunk_size);
	}

	return tb_client_publish(s_fw.client, topic, payload);
}

static void fw_fail(const char *reason)
{
	osal_ota_abort();
	s_fw.in_progress = false;
	osal_log_error("[tb_fw] Firmware update failed: %s",
		       reason ? reason : "firmware update failed");
	fw_persist_state(TB_FW_STATE_FAILED,
			reason ? reason : "firmware update failed");
	fw_report_state(TB_FW_STATE_FAILED,
			reason ? reason : "firmware update failed");
}

static const char *fw_ota_error(osal_status_t status, const char *fallback)
{
	switch (status) {
	case OSAL_ERR_IMAGE_INVALID:
		return "firmware image validation failed";
	case OSAL_ERR_SECURITY_VERSION:
		return "firmware security version rejected";
	default:
		return fallback;
	}
}

static void fw_restore_persisted_state(void)
{
	osal_ota_state_t persisted_state;
	osal_status_t status = osal_ota_state_load(&persisted_state);
	tb_fw_state_t restored_state;

	if (status == OSAL_ERR_EMPTY_SET) {
		fw_persist_state(TB_FW_STATE_IDLE, NULL);
		fw_report_state(TB_FW_STATE_IDLE, NULL);
		return;
	}
	if (status != OSAL_SUCCESS) {
		osal_log_warning("[tb_fw] Failed to load OTA state: %d", status);
		fw_clear_persisted_state();
		fw_report_state(TB_FW_STATE_IDLE, NULL);
		return;
	}

	strncpy(s_fw.target_title, persisted_state.title,
		sizeof(s_fw.target_title) - 1);
	strncpy(s_fw.target_version, persisted_state.version,
		sizeof(s_fw.target_version) - 1);
	strncpy(s_fw.target_checksum, persisted_state.checksum,
		sizeof(s_fw.target_checksum) - 1);
	strncpy(s_fw.target_checksum_alg, "SHA256",
		sizeof(s_fw.target_checksum_alg) - 1);

	restored_state = fw_state_from_persisted_state(
		persisted_state.download_state);
	if (restored_state == TB_FW_STATE_UPDATING) {
		if (strcmp(s_fw.current_title, persisted_state.title) == 0 &&
		    strcmp(s_fw.current_version, persisted_state.version) == 0) {
			fw_persist_state(TB_FW_STATE_UPDATED, NULL);
			fw_report_state(TB_FW_STATE_UPDATED, NULL);
			return;
		}

		fw_persist_state(TB_FW_STATE_FAILED,
				 "firmware update interrupted before confirmation");
		fw_report_state(TB_FW_STATE_FAILED,
				"firmware update interrupted before confirmation");
		return;
	}

	if (restored_state == TB_FW_STATE_DOWNLOADING ||
	    restored_state == TB_FW_STATE_DOWNLOADED ||
	    restored_state == TB_FW_STATE_VERIFIED) {
		fw_persist_state(TB_FW_STATE_FAILED,
				 "firmware download interrupted by restart");
		fw_report_state(TB_FW_STATE_FAILED,
				"firmware download interrupted by restart");
		return;
	}

	if (restored_state == TB_FW_STATE_FAILED) {
		fw_report_state(TB_FW_STATE_FAILED, persisted_state.last_error);
		return;
	}
	if (restored_state == TB_FW_STATE_UPDATED) {
		fw_report_state(TB_FW_STATE_UPDATED, NULL);
		return;
	}

	fw_report_state(TB_FW_STATE_IDLE, NULL);
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
	errno = 0;
	unsigned long req = strtoul(p, &end_ptr, 10);
	if (errno == ERANGE || end_ptr == p || req > UINT32_MAX ||
	    strncmp(end_ptr, "/chunk/", 7) != 0) {
		return false;
	}

	p = end_ptr + 7;
	errno = 0;
	unsigned long chk = strtoul(p, &end_ptr, 10);
	if (errno == ERANGE || end_ptr == p || chk > UINT32_MAX ||
	    *end_ptr != '\0') {
		return false;
	}

	*request_id = (uint32_t)req;
	*chunk = (uint32_t)chk;
	return true;
}

typedef enum {
	FW_NOTIFY_NONE = 0,
	FW_NOTIFY_APPLIED,
	FW_NOTIFY_REBOOT_REQUIRED
} fw_notify_t;

static fw_notify_t fw_chunk_handler_locked(const char *topic,
					   const char *payload,
					   size_t payload_len)
{
	uint32_t req_id = 0;
	uint32_t chunk = 0;

	if (!s_fw.in_progress) {
		return FW_NOTIFY_NONE;
	}
	if (!fw_parse_response_topic(topic, &req_id, &chunk)) {
		return FW_NOTIFY_NONE;
	}
	if (req_id != s_fw.request_id || chunk != s_fw.next_chunk) {
		return FW_NOTIFY_NONE;
	}

	osal_status_t write_status =
		osal_ota_write((const uint8_t *)payload, payload_len);
	if (write_status != OSAL_SUCCESS) {
		fw_fail(fw_ota_error(write_status, "ota write failed"));
		return FW_NOTIFY_NONE;
	}

	s_fw.downloaded_size += payload_len;
	s_fw.next_chunk++;
	s_fw.progress_seq++;

	if (s_fw.downloaded_size >= s_fw.target_size) {
		if (s_fw.downloaded_size != s_fw.target_size) {
			fw_fail("firmware size mismatch");
			return FW_NOTIFY_NONE;
		}

		fw_report_state(TB_FW_STATE_DOWNLOADED, NULL);
		if (osal_ota_verify() != OSAL_SUCCESS) {
			fw_fail("firmware checksum mismatch");
			return FW_NOTIFY_NONE;
		}

		fw_report_state(TB_FW_STATE_VERIFIED, NULL);
		fw_persist_state(TB_FW_STATE_VERIFIED, NULL);
		fw_report_state(TB_FW_STATE_UPDATING, NULL);
		fw_persist_state(TB_FW_STATE_UPDATING, NULL);

		const bool defer_restart = s_fw.on_reboot_required != NULL;
		osal_status_t finish_status =
			osal_ota_finish_ex(true, !defer_restart);
		if (finish_status != OSAL_SUCCESS) {
			fw_fail(fw_ota_error(finish_status, "ota finalize failed"));
			return FW_NOTIFY_NONE;
		}

		s_fw.in_progress = false;
		if (defer_restart) {
			/* UPDATED is reported by the next boot's restore. */
			s_fw.reboot_pending = true;
			osal_log_info("[tb_fw] Firmware %s %s verified and activated; "
				      "restart required",
				      s_fw.target_title, s_fw.target_version);
			return FW_NOTIFY_REBOOT_REQUIRED;
		}

		strncpy(s_fw.current_title, s_fw.target_title,
			sizeof(s_fw.current_title) - 1);
		strncpy(s_fw.current_version, s_fw.target_version,
			sizeof(s_fw.current_version) - 1);
		s_fw.current_title[sizeof(s_fw.current_title) - 1] = '\0';
		s_fw.current_version[sizeof(s_fw.current_version) - 1] = '\0';

		fw_persist_state(TB_FW_STATE_UPDATED, NULL);
		fw_report_state(TB_FW_STATE_UPDATED, NULL);
		return FW_NOTIFY_APPLIED;
	}

	if (fw_request_chunk() != 0) {
		fw_fail("chunk request failed");
	}
	return FW_NOTIFY_NONE;
}

static void fw_chunk_handler(const char *topic, const char *payload,
			     size_t payload_len)
{
	char title[TB_FW_STR_LEN];
	char version[TB_FW_STR_LEN];
	tb_firmware_applied_cb_t on_applied;
	tb_firmware_reboot_required_cb_t on_reboot_required;
	void *user_data;

	if (!fw_lock()) {
		return;
	}
	fw_notify_t notify =
		fw_chunk_handler_locked(topic, payload, payload_len);
	memcpy(title, s_fw.target_title, sizeof(title));
	memcpy(version, s_fw.target_version, sizeof(version));
	on_applied = s_fw.on_applied;
	on_reboot_required = s_fw.on_reboot_required;
	user_data = s_fw.user_data;
	fw_unlock();

	if (notify == FW_NOTIFY_APPLIED && on_applied != NULL) {
		on_applied(title, version, user_data);
	} else if (notify == FW_NOTIFY_REBOOT_REQUIRED &&
		   on_reboot_required != NULL) {
		on_reboot_required(title, version, user_data);
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
	s_fw.chunk_retries = 0;
	s_fw.poll_armed = false;
	s_fw.progress_seq++;

	osal_log_info("[tb_fw] Firmware download started: %s %s size=%zu chunk=%" PRIu32,
		      s_fw.target_title, s_fw.target_version, s_fw.target_size,
		      s_fw.chunk_size);
	fw_persist_state(TB_FW_STATE_DOWNLOADING, NULL);
	fw_report_state(TB_FW_STATE_DOWNLOADING, NULL);

	return fw_request_chunk();
}

static void fw_settle_state(tb_fw_state_t state)
{
	if (s_fw.state == state) {
		return;
	}
	s_fw.target_title[0] = '\0';
	s_fw.target_version[0] = '\0';
	s_fw.target_checksum[0] = '\0';
	fw_persist_state(state, NULL);
	fw_report_state(state, NULL);
}

static void fw_attributes_cb_locked(tb_request_result_t result,
				    const char *response_json)
{
	if (result != TB_REQUEST_RESULT_SUCCESS || response_json == NULL ||
	    !s_fw.initialized || s_fw.in_progress || s_fw.reboot_pending) {
		return;
	}

	cJSON *root = cJSON_Parse(response_json);
	if (root == NULL) {
		return;
	}

	cJSON *shared = cJSON_GetObjectItemCaseSensitive(root, "shared");
	if (!cJSON_IsObject(shared) ||
	    cJSON_GetObjectItemCaseSensitive(shared, TB_FW_ATTR_TITLE) == NULL) {
		cJSON_Delete(root);
		/* Unassigned: a stale failure of a removed package is not current. */
		if (s_fw.state == TB_FW_STATE_FAILED) {
			osal_log_info("No firmware assigned; clearing FAILED state");
			fw_settle_state(TB_FW_STATE_IDLE);
		}
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
	if (!cJSON_IsString(fw_checksum) || fw_checksum->valuestring == NULL ||
	    fw_checksum->valuestring[0] == '\0') {
		cJSON_Delete(root);
		fw_fail("missing firmware checksum");
		return;
	}
	if (!cJSON_IsString(fw_checksum_alg) ||
	    fw_checksum_alg->valuestring == NULL ||
	    fw_checksum_alg->valuestring[0] == '\0') {
		cJSON_Delete(root);
		fw_fail("missing firmware checksum algorithm");
		return;
	}
	if (!fw_checksum_algorithm_is_supported(
			fw_checksum_alg->valuestring)) {
		cJSON_Delete(root);
		fw_fail("unsupported firmware checksum algorithm");
		return;
	}
	if (!fw_checksum_is_valid_hex(fw_checksum->valuestring)) {
		cJSON_Delete(root);
		fw_fail("invalid firmware checksum");
		return;
	}

	if (strcmp(s_fw.current_title, fw_title->valuestring) == 0 &&
	    strcmp(s_fw.current_version, fw_version->valuestring) == 0) {
		osal_log_info("Firmware is up to date: %s v%s",
			      s_fw.current_title, s_fw.current_version);
		cJSON_Delete(root);
		fw_settle_state(TB_FW_STATE_UPDATED);
		return;
	}

	strncpy(s_fw.target_title, fw_title->valuestring,
		sizeof(s_fw.target_title) - 1);
	strncpy(s_fw.target_version, fw_version->valuestring,
		sizeof(s_fw.target_version) - 1);
	s_fw.target_title[sizeof(s_fw.target_title) - 1] = '\0';
	s_fw.target_version[sizeof(s_fw.target_version) - 1] = '\0';

	strncpy(s_fw.target_checksum, fw_checksum->valuestring,
		sizeof(s_fw.target_checksum) - 1);
	s_fw.target_checksum[sizeof(s_fw.target_checksum) - 1] = '\0';

	strncpy(s_fw.target_checksum_alg, fw_checksum_alg->valuestring,
		sizeof(s_fw.target_checksum_alg) - 1);
	s_fw.target_checksum_alg[sizeof(s_fw.target_checksum_alg) - 1] = '\0';

	if (fw_size->valuedouble < 1 ||
	    fw_size->valuedouble >= (double)SIZE_MAX) {
		cJSON_Delete(root);
		fw_fail("invalid firmware size");
		return;
	}

	s_fw.target_size = (size_t)fw_size->valuedouble;
	if ((double)s_fw.target_size != fw_size->valuedouble) {
		cJSON_Delete(root);
		fw_fail("firmware size must be an integer");
		return;
	}

	cJSON_Delete(root);

	if (s_fw.target_size == 0) {
		fw_fail("invalid firmware size");
		return;
	}

	if (fw_start_download() != 0) {
		fw_fail("cannot start firmware download");
	}
}

static void fw_attributes_cb(tb_request_result_t result,
			     const char *response_json, void *user_data)
{
	(void)user_data;

	if (!fw_lock()) {
		return;
	}
	fw_attributes_cb_locked(result, response_json);
	fw_unlock();
}

int tb_firmware_update_init(tb_client_t *client,
			    const tb_firmware_update_config_t *config)
{
	if (client == NULL || config == NULL) {
		return -1;
	}

	if (s_fw_lock == NULL &&
	    osal_mutex_create(&s_fw_lock, "tb_fw") != OSAL_SUCCESS) {
		return -1;
	}

	if (!fw_lock()) {
		return -1;
	}
	if (s_fw.in_progress) {
		/* Re-init without deinit (e.g. reconnect): never leak the session. */
		osal_ota_abort();
		fw_persist_state(TB_FW_STATE_FAILED,
				 "firmware download interrupted by reconnect");
	}
	memset(&s_fw, 0, sizeof(s_fw));
	if (osal_ota_init() != OSAL_SUCCESS) {
		fw_unlock();
		return -1;
	}
	s_fw.client = client;
	s_fw.chunk_size = config->chunk_size;
	s_fw.on_applied = config->on_applied;
	s_fw.on_reboot_required = config->on_reboot_required;
	s_fw.user_data = config->user_data;
	s_fw.chunk_timeout_ms = config->chunk_timeout_ms != 0U ?
		config->chunk_timeout_ms :
		TB_FIRMWARE_UPDATE_CHUNK_TIMEOUT_DEFAULT_MS;
	s_fw.max_chunk_retries = config->max_chunk_retries != 0U ?
		config->max_chunk_retries :
		TB_FIRMWARE_UPDATE_CHUNK_RETRIES_DEFAULT;

	strncpy(s_fw.current_title,
		config->current_title ? config->current_title : "Initial",
		sizeof(s_fw.current_title) - 1);
	strncpy(s_fw.current_version,
		config->current_version ? config->current_version : "v0",
		sizeof(s_fw.current_version) - 1);
	s_fw.current_title[sizeof(s_fw.current_title) - 1] = '\0';
	s_fw.current_version[sizeof(s_fw.current_version) - 1] = '\0';
	fw_unlock();

	/* SUBSCRIBE blocks on the transport thread's SUBACK: never hold the lock. */
	int rc = tb_client_subscribe(client, TB_FW_RESP_TOPIC_SUB,
				     fw_chunk_handler, TB_FW_TIMEOUT_MS);
	if (rc != 0) {
		return rc;
	}

	if (!fw_lock()) {
		return -1;
	}
	s_fw.subscribed = true;
	s_fw.initialized = true;
	fw_restore_persisted_state();
	fw_unlock();
	return 0;
}

int tb_firmware_update_request_check(tb_client_t *client)
{
	static const char *shared_keys[] = {
		TB_FW_ATTR_CHECKSUM, TB_FW_ATTR_CHECKSUM_ALG, TB_FW_ATTR_SIZE,
		TB_FW_ATTR_TITLE,    TB_FW_ATTR_VERSION,
	};
	bool busy;

	if (client == NULL || !fw_lock()) {
		return -1;
	}
	if (!s_fw.initialized || client != s_fw.client) {
		fw_unlock();
		return -1;
	}
	busy = s_fw.in_progress || s_fw.reboot_pending;
	fw_unlock();

	if (busy) {
		return 0;
	}

	return tb_attributes_request_shared(
		client, shared_keys,
		sizeof(shared_keys) / sizeof(shared_keys[0]), fw_attributes_cb,
		NULL, TB_FW_TIMEOUT_MS);
}

int tb_firmware_update_confirm_health(tb_client_t *client)
{
	if (!s_fw.initialized || client == NULL || client != s_fw.client) {
		return -1;
	}
	if (!tb_client_is_connected(client)) {
		return -1;
	}

	return (osal_ota_confirm_running_image() == OSAL_SUCCESS) ? 0 : -1;
}

bool tb_firmware_update_is_in_progress(void)
{
	bool in_progress;

	if (!fw_lock()) {
		return false;
	}
	in_progress = s_fw.in_progress;
	fw_unlock();
	return in_progress;
}

void tb_firmware_update_poll(tb_client_t *client, uint32_t now_ms)
{
	if (client == NULL || !fw_lock()) {
		return;
	}
	if (!s_fw.initialized || client != s_fw.client || !s_fw.in_progress) {
		s_fw.poll_armed = false;
		fw_unlock();
		return;
	}

	if (!s_fw.poll_armed || s_fw.poll_seen_seq != s_fw.progress_seq) {
		s_fw.poll_armed = true;
		s_fw.poll_seen_seq = s_fw.progress_seq;
		s_fw.chunk_retries = 0;
		s_fw.chunk_deadline_ms = now_ms + s_fw.chunk_timeout_ms;
		fw_unlock();
		return;
	}

	if ((int32_t)(now_ms - s_fw.chunk_deadline_ms) < 0) {
		fw_unlock();
		return;
	}

	if (s_fw.chunk_retries >= s_fw.max_chunk_retries) {
		fw_fail("firmware chunk timeout");
		fw_unlock();
		return;
	}

	s_fw.chunk_retries++;
	osal_log_warning("[tb_fw] Chunk %" PRIu32 " timed out; re-request %" PRIu32
			 "/%" PRIu32,
			 s_fw.next_chunk, s_fw.chunk_retries,
			 s_fw.max_chunk_retries);
	/* A failed publish (link down) just consumes this retry. */
	(void)fw_request_chunk();
	s_fw.chunk_deadline_ms = now_ms + s_fw.chunk_timeout_ms;
	fw_unlock();
}

int tb_firmware_update_get_status(tb_firmware_update_status_t *status)
{
	if (status == NULL || !fw_lock()) {
		return -1;
	}

	memset(status, 0, sizeof(*status));
	status->state = s_fw.state;
	status->downloaded_size = s_fw.downloaded_size;
	status->total_size = s_fw.target_size;
	memcpy(status->target_title, s_fw.target_title,
	       sizeof(status->target_title));
	memcpy(status->target_version, s_fw.target_version,
	       sizeof(status->target_version));
	memcpy(status->last_error, s_fw.last_error,
	       sizeof(status->last_error));
	fw_unlock();
	return 0;
}

const char *tb_firmware_update_state_name(tb_firmware_update_state_t state)
{
	return fw_state_to_string(state);
}

void tb_firmware_update_deinit(tb_client_t *client)
{
	bool subscribed;

	if (client == NULL || !fw_lock()) {
		return;
	}
	if (!s_fw.initialized || client != s_fw.client) {
		fw_unlock();
		return;
	}

	if (s_fw.in_progress) {
		osal_ota_abort();
		s_fw.in_progress = false;
		fw_persist_state(TB_FW_STATE_FAILED,
				 "firmware update stopped during deinitialization");
	}
	subscribed = s_fw.subscribed;
	s_fw.initialized = false;
	fw_unlock();

	if (subscribed && tb_client_is_connected(client)) {
		tb_client_unsubscribe(client, TB_FW_RESP_TOPIC_SUB,
				      TB_FW_TIMEOUT_MS);
	}

	if (fw_lock()) {
		memset(&s_fw, 0, sizeof(s_fw));
		fw_unlock();
	}
}
