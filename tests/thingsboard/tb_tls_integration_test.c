/**
 *******************************************************************************
 * @file    tb_tls_integration_test.c
 * @brief   ThingsBoard TLS integration coverage using real MQTT adapter
 *******************************************************************************
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mongoose_process.h"
#include "mqtt_config.h"
#include "osal_file.h"
#include "osal_mount.h"
#include "osal_task.h"
#include "tb_client.h"

#ifdef ESP_PLATFORM
#define TEST_IMAGE_PATH "flash_test"
#define TEST_MOUNT_POINT "/littlefs"
#else
#define TEST_IMAGE_PATH "/tmp/tb_tls_it.img"
#define TEST_MOUNT_POINT "/"
#endif

#define TEST_WAIT_STEP_MS 100U
#define TEST_WAIT_TOTAL_MS 12000U

#define TEST_ASSERT(condition, message)                                            \
	do {                                                                         \
		tests_run++;                                                           \
		if (condition) {                                                       \
			tests_passed++;                                                  \
			printf("  [PASS] %s\n", message);                               \
		} else {                                                               \
			tests_failed++;                                                  \
			printf("  [FAIL] %s (line %d)\n", message, __LINE__);          \
		}                                                                      \
	} while (0)

#define TEST_START(name)                                                           \
	printf("\n--------------------------------------------------\n");        \
	printf("TEST: %s\n", name);                                               \
	printf("--------------------------------------------------\n")

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static volatile int g_connect_count = 0;
static volatile int g_disconnect_count = 0;
static volatile int g_connect_failure_count = 0;

static void on_connect(tb_client_t *client, void *user_data)
{
	(void)client;
	(void)user_data;
	g_connect_count++;
}

static void on_disconnect(tb_client_t *client,
			 tb_client_disconnect_reason_t reason,
			 void *user_data)
{
	(void)client;
	(void)reason;
	(void)user_data;
	g_disconnect_count++;
}

static void on_connect_failure(tb_client_t *client,
			      tb_client_connect_failure_reason_t reason,
			      void *user_data)
{
	(void)client;
	(void)reason;
	(void)user_data;
	g_connect_failure_count++;
}

static void reset_connection_counters(void)
{
	g_connect_count = 0;
	g_disconnect_count = 0;
	g_connect_failure_count = 0;
}

static bool setup_fs(void)
{
	(void)osal_unmount(TEST_MOUNT_POINT);
	(void)osal_rmfs(TEST_IMAGE_PATH);

	if (osal_initfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, 4096U, 256U) !=
	    OSAL_SUCCESS) {
		return false;
	}
	if (osal_mkfs(NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, 4096U, 256U) !=
	    OSAL_SUCCESS) {
		return false;
	}
	if (osal_mount(TEST_IMAGE_PATH, TEST_MOUNT_POINT) != OSAL_SUCCESS) {
		return false;
	}
	return true;
}

static void teardown_fs(void)
{
	(void)osal_unmount(TEST_MOUNT_POINT);
	(void)osal_rmfs(TEST_IMAGE_PATH);
}

static bool osal_write_file(const char *path, const char *data, size_t len)
{
	osal_file_id_t fd;
	int32_t n;

	fd = osal_open_create(path,
			      OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
			      OSAL_WRITE_ONLY);
	if (fd < 0) {
		return false;
	}

	n = osal_write(fd, data, len);
	(void)osal_close(fd);
	return n >= 0 && (size_t)n == len;
}

static bool provision_cert_from_host(const char *host_path,
				     const char *osal_path)
{
	FILE *fp;
	char buf[MQTT_CERT_MAX_SIZE];
	size_t n;

	fp = fopen(host_path, "rb");
	if (!fp) {
		return false;
	}

	n = fread(buf, 1, sizeof(buf) - 1, fp);
	(void)fclose(fp);
	if (n == 0) {
		return false;
	}

	buf[n] = '\0';
	return osal_write_file(osal_path, buf, n);
}

static int run_tls_case(const char *name, const char *url, const char *ca_osal_path,
			const char *client_cert_osal_path,
			const char *client_key_osal_path, bool expect_success)
{
	tb_client_t *client = NULL;
	tb_client_config_t cfg;
	uint32_t waited = 0;
	int rc;

	TEST_START(name);
	reset_connection_counters();

	memset(&cfg, 0, sizeof(cfg));
	strncpy(cfg.server_url, url, sizeof(cfg.server_url) - 1);
	strncpy(cfg.access_token, "tb_tls_it_user", sizeof(cfg.access_token) - 1);
	strncpy(cfg.client_id, "tb_tls_it_client", sizeof(cfg.client_id) - 1);
	strncpy(cfg.device_name, "tb_tls_it_device", sizeof(cfg.device_name) - 1);
	cfg.on_connect = on_connect;
	cfg.on_disconnect = on_disconnect;
	cfg.on_connect_failure = on_connect_failure;

	rc = tb_client_init(&client, &cfg);
	TEST_ASSERT(rc == 0 && client != NULL, "tb_client_init succeeds");
	if (rc != 0 || client == NULL) {
		return -1;
	}

	TEST_ASSERT(mqtt_config_set_string("", MQTT_CONFIG_VALUE_PASSWORD),
		    "password configured");
	TEST_ASSERT(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL),
		    "ssl enabled");
	TEST_ASSERT(mqtt_config_set_bool(false, MQTT_CONFIG_VALUE_SKIP_VERIFY),
		    "skip verify disabled");
	TEST_ASSERT(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
					ca_osal_path,
					MQTT_CONFIG_VALUE_CERT),
		    "CA cert configured");

	if (client_cert_osal_path != NULL && client_key_osal_path != NULL &&
	    client_cert_osal_path[0] != '\0' && client_key_osal_path[0] != '\0') {
		TEST_ASSERT(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						client_cert_osal_path,
						MQTT_CONFIG_VALUE_CLIENT_CERT),
			    "client cert configured");
		TEST_ASSERT(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						client_key_osal_path,
						MQTT_CONFIG_VALUE_CLIENT_KEY),
			    "client key configured");
	}

	rc = tb_client_connect(client);
	TEST_ASSERT(rc == 0, "tb_client_connect requested");
	if (rc != 0) {
		tb_client_deinit(client);
		return -1;
	}

	while (waited < TEST_WAIT_TOTAL_MS) {
		if (tb_client_is_connected(client)) {
			break;
		}
		if (g_connect_failure_count > 0 && !expect_success) {
			break;
		}
		osal_task_delay_ms(TEST_WAIT_STEP_MS);
		waited += TEST_WAIT_STEP_MS;
	}

	if (expect_success) {
		TEST_ASSERT(tb_client_is_connected(client),
			    "TLS connection succeeds with trusted CA");
		TEST_ASSERT(g_connect_count > 0,
			    "connect callback called on success");
	} else {
		TEST_ASSERT(!tb_client_is_connected(client),
			    "TLS connection is not established");
		TEST_ASSERT(g_connect_failure_count > 0,
			    "connect failure callback called");
	}

	tb_client_deinit(client);
	return 0;
}

int main(void)
{
	const char *url_ok = getenv("TB_TLS_IT_URL_OK");
	const char *url_badhost = getenv("TB_TLS_IT_URL_HOSTNAME_MISMATCH");
	const char *ca_good_host = getenv("TB_TLS_IT_CA_OK_HOST_PATH");
	const char *ca_bad_host = getenv("TB_TLS_IT_CA_BAD_HOST_PATH");
	const char *client_cert_host = getenv("TB_TLS_IT_CLIENT_CERT_HOST_PATH");
	const char *client_key_host = getenv("TB_TLS_IT_CLIENT_KEY_HOST_PATH");
	const char *ca_good_osal = "ca_good.crt";
	const char *ca_bad_osal = "ca_bad.crt";
	const char *client_cert_osal = "client.crt";
	const char *client_key_osal = "client.key";

	if (url_ok == NULL || url_ok[0] == '\0') {
		url_ok = "mqtts://localhost:8885";
	}
	if (url_badhost == NULL || url_badhost[0] == '\0') {
		url_badhost = "mqtts://localhost:8886";
	}
	if (ca_good_host == NULL || ca_good_host[0] == '\0' ||
	    ca_bad_host == NULL || ca_bad_host[0] == '\0') {
		printf("[FAIL] Missing CA host path environment variables\n");
		return 1;
	}

	printf("\n==================================================\n");
	printf("   ThingsBoard TLS Integration Test\n");
	printf("==================================================\n");
	printf("Trusted URL: %s\n", url_ok);
	printf("Mismatch URL: %s\n", url_badhost);

	if (!setup_fs()) {
		printf("[FAIL] filesystem setup failed\n");
		return 1;
	}

	if (!provision_cert_from_host(ca_good_host, ca_good_osal)) {
		printf("[FAIL] failed to provision trusted CA cert\n");
		teardown_fs();
		return 1;
	}
	if (!provision_cert_from_host(ca_bad_host, ca_bad_osal)) {
		printf("[FAIL] failed to provision unknown CA cert\n");
		teardown_fs();
		return 1;
	}

	const char *client_cert_for_case = "";
	const char *client_key_for_case = "";
	if (client_cert_host != NULL && client_key_host != NULL &&
	    client_cert_host[0] != '\0' && client_key_host[0] != '\0') {
		if (!provision_cert_from_host(client_cert_host, client_cert_osal) ||
		    !provision_cert_from_host(client_key_host, client_key_osal)) {
			printf("[FAIL] failed to provision client cert/key\n");
			teardown_fs();
			return 1;
		}
		client_cert_for_case = client_cert_osal;
		client_key_for_case = client_key_osal;
	}

	MongooseProcess_Init();

	(void)run_tls_case("TLS Trusted CA Success", url_ok, ca_good_osal,
			  client_cert_for_case, client_key_for_case, true);
	(void)run_tls_case("TLS Unknown CA Failure", url_ok, ca_bad_osal,
			  client_cert_for_case, client_key_for_case, false);
	(void)run_tls_case("TLS Hostname Mismatch Failure", url_badhost,
			  ca_good_osal, client_cert_for_case,
			  client_key_for_case, false);

	MongooseProcess_Deinit();
	teardown_fs();

	printf("\n==================================================\n");
	printf("                  TEST SUMMARY                    \n");
	printf("==================================================\n");
	printf("  Run:    %d\n", tests_run);
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("==================================================\n");

	return (tests_failed == 0) ? 0 : 1;
}
