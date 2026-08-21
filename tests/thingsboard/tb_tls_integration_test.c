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

#include "unity.h"

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

static volatile int g_connect_count = 0;
static volatile int g_disconnect_count = 0;
static volatile int g_connect_failure_count = 0;

static volatile bool g_setup_ready = false;

static const char *g_url_ok = "mqtts://localhost:8885";
static const char *g_url_badhost = "mqtts://localhost:8886";
static char g_ca_good_osal[128] = { 0 };
static char g_ca_bad_osal[128] = { 0 };
static char g_client_cert_osal[128] = { 0 };
static char g_client_key_osal[128] = { 0 };
static const char *g_client_cert_for_case = "";
static const char *g_client_key_for_case = "";

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

static bool make_osal_path(char *dst, size_t dst_size, const char *filename)
{
	int n;

	if (dst == NULL || filename == NULL || filename[0] == '\0') {
		return false;
	}

	n = snprintf(dst, dst_size, "%s/%s", TEST_MOUNT_POINT, filename);
	return n > 0 && (size_t)n < dst_size;
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

	printf("\n--------------------------------------------------\n");
	printf("TEST: %s\n", name);
	printf("--------------------------------------------------\n");
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
	TEST_ASSERT_TRUE_MESSAGE(rc == 0 && client != NULL,
				 "tb_client_init succeeds");
	if (rc != 0 || client == NULL) {
		return -1;
	}

	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_string("", MQTT_CONFIG_VALUE_PASSWORD),
				 "password configured");
	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL),
				 "ssl enabled");
	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_bool(false, MQTT_CONFIG_VALUE_SKIP_VERIFY),
				 "skip verify disabled");
	TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						     ca_osal_path,
						     MQTT_CONFIG_VALUE_CERT),
				 "CA cert configured");

	if (client_cert_osal_path != NULL && client_key_osal_path != NULL &&
	    client_cert_osal_path[0] != '\0' && client_key_osal_path[0] != '\0') {
		TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
							client_cert_osal_path,
							MQTT_CONFIG_VALUE_CLIENT_CERT),
					 "client cert configured");
		TEST_ASSERT_TRUE_MESSAGE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
							client_key_osal_path,
							MQTT_CONFIG_VALUE_CLIENT_KEY),
					 "client key configured");
	}

	rc = tb_client_connect(client);
	TEST_ASSERT_TRUE_MESSAGE(rc == 0, "tb_client_connect requested");
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
		TEST_ASSERT_TRUE_MESSAGE(tb_client_is_connected(client),
					 "TLS connection succeeds with trusted CA");
		TEST_ASSERT_TRUE_MESSAGE(g_connect_count > 0,
					 "connect callback called on success");
	} else {
		TEST_ASSERT_TRUE_MESSAGE(!tb_client_is_connected(client),
					 "TLS connection is not established");
		TEST_ASSERT_TRUE_MESSAGE(g_connect_failure_count > 0,
					 "connect failure callback called");
	}

	tb_client_deinit(client);
	return 0;
}

static void test_tls_trusted_ca_success(void)
{
	TEST_ASSERT_EQUAL_INT(0, run_tls_case("TLS Trusted CA Success", g_url_ok,
					      g_ca_good_osal, g_client_cert_for_case,
					      g_client_key_for_case, true));
}

static void test_tls_unknown_ca_failure(void)
{
	TEST_ASSERT_EQUAL_INT(0, run_tls_case("TLS Unknown CA Failure", g_url_ok,
					      g_ca_bad_osal, g_client_cert_for_case,
					      g_client_key_for_case, false));
}

static void test_tls_hostname_mismatch_failure(void)
{
	TEST_ASSERT_EQUAL_INT(0, run_tls_case("TLS Hostname Mismatch Failure",
					      g_url_badhost, g_ca_good_osal,
					      g_client_cert_for_case, g_client_key_for_case,
					      false));
}

static bool prepare_prerequisites(const char *ca_good_host, const char *ca_bad_host,
				  const char *client_cert_host,
				  const char *client_key_host)
{
	if (!setup_fs()) {
		printf("  [PREREQ] filesystem setup failed\n");
		return false;
	}

	if (!make_osal_path(g_ca_good_osal, sizeof(g_ca_good_osal), "ca_good.crt") ||
	    !make_osal_path(g_ca_bad_osal, sizeof(g_ca_bad_osal), "ca_bad.crt") ||
	    !make_osal_path(g_client_cert_osal, sizeof(g_client_cert_osal),
			    "client.crt") ||
	    !make_osal_path(g_client_key_osal, sizeof(g_client_key_osal),
			    "client.key")) {
		printf("setup [PREREQ] failed to build OSAL certificate paths\n");
		teardown_fs();
		return false;
	}

	if (!provision_cert_from_host(ca_good_host, g_ca_good_osal)) {
		printf("setup [PREREQ] failed to provision trusted CA cert\n");
		teardown_fs();
		return false;
	}
	if (!provision_cert_from_host(ca_bad_host, g_ca_bad_osal)) {
		printf("setup [PREREQ] failed to provision unknown CA cert\n");
		teardown_fs();
		return false;
	}

	if (client_cert_host != NULL && client_key_host != NULL &&
	    client_cert_host[0] != '\0' && client_key_host[0] != '\0') {
		if (!provision_cert_from_host(client_cert_host, g_client_cert_osal) ||
		    !provision_cert_from_host(client_key_host, g_client_key_osal)) {
			printf("setup [PREREQ] failed to provision client cert/key\n");
			teardown_fs();
			return false;
		}
		g_client_cert_for_case = g_client_cert_osal;
		g_client_key_for_case = g_client_key_osal;
	}

	return true;
}

void setUp(void)
{
	/* Filesystem, certificate provisioning and Mongoose lifecycle are
	 * established once from main() before the tests are registered. If that
	 * prerequisite setup failed, report it here through the Unity failure API
	 * so each test is flagged as aborted. */
	if (!g_setup_ready) {
		TEST_FAIL_MESSAGE("prerequisite setup failed; TLS integration tests aborted");
	}
}

void tearDown(void)
{
}

int main(void)
{
	const char *url_ok = getenv("TB_TLS_IT_URL_OK");
	const char *url_badhost = getenv("TB_TLS_IT_URL_HOSTNAME_MISMATCH");
	const char *ca_good_host = getenv("TB_TLS_IT_CA_OK_HOST_PATH");
	const char *ca_bad_host = getenv("TB_TLS_IT_CA_BAD_HOST_PATH");
	const char *client_cert_host = getenv("TB_TLS_IT_CLIENT_CERT_HOST_PATH");
	const char *client_key_host = getenv("TB_TLS_IT_CLIENT_KEY_HOST_PATH");

	if (url_ok == NULL || url_ok[0] == '\0') {
		url_ok = "mqtts://localhost:8885";
	}
	if (url_badhost == NULL || url_badhost[0] == '\0') {
		url_badhost = "mqtts://localhost:8886";
	}
	g_url_ok = url_ok;
	g_url_badhost = url_badhost;

	printf("\n==================================================\n");
	printf("   ThingsBoard TLS Integration Test\n");
	printf("==================================================\n");
	printf("Trusted URL: %s\n", g_url_ok);
	printf("Mismatch URL: %s\n", g_url_badhost);

	UNITY_BEGIN();

	if (ca_good_host == NULL || ca_good_host[0] == '\0' ||
	    ca_bad_host == NULL || ca_bad_host[0] == '\0') {
		printf("[PREREQ] Missing CA host path environment variables\n");
		g_setup_ready = false;
	} else {
		g_setup_ready = prepare_prerequisites(ca_good_host, ca_bad_host,
						      client_cert_host,
						      client_key_host);
	}

	MongooseProcess_Init();

	RUN_TEST(test_tls_trusted_ca_success);
	RUN_TEST(test_tls_unknown_ca_failure);
	RUN_TEST(test_tls_hostname_mismatch_failure);

	MongooseProcess_Deinit();
	teardown_fs();

	return UNITY_END();
}