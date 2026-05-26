/*
 * MQTT Config Tests
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_config.h"
#include "osal_file.h"
#include "osal_mount.h"

#ifdef ESP_PLATFORM
#define MQTT_TEST_IMAGE_PATH  "flash_test"
#define MQTT_TEST_MOUNT_POINT "/littlefs"
#else
#define MQTT_TEST_IMAGE_PATH  "/tmp/mqtt_config_test.img"
#define MQTT_TEST_MOUNT_POINT "/"
#endif

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static bool g_callback_called = false;

#define TEST_ASSERT(condition, message)                                           \
	do {                                                                        \
		tests_run++;                                                          \
		if (condition) {                                                      \
			tests_passed++;                                                 \
			printf("[PASS] %s\n", message);                                \
		} else {                                                              \
			tests_failed++;                                                 \
			printf("[FAIL] %s\n", message);                                \
		}                                                                     \
	} while (0)

#define TEST_START(name)                                                           \
	printf("\n==================================================\n");      \
	printf("TEST: %s\n", name);                                              \
	printf("==================================================\n")

#define TEST_END() printf("--------------------------------------------------\n")

static void on_apply_config(void)
{
	g_callback_called = true;
}

static bool mqtt_test_fs_setup(void)
{
	(void)osal_unmount(MQTT_TEST_MOUNT_POINT);
	(void)osal_rmfs(MQTT_TEST_IMAGE_PATH);

	if (osal_initfs(NULL, MQTT_TEST_IMAGE_PATH, MQTT_TEST_MOUNT_POINT,
			4096U, 256U) != OSAL_SUCCESS) {
		return false;
	}

	if (osal_mkfs(NULL, MQTT_TEST_IMAGE_PATH, MQTT_TEST_MOUNT_POINT, 4096U,
		      256U) != OSAL_SUCCESS) {
		return false;
	}

	if (osal_mount(MQTT_TEST_IMAGE_PATH, MQTT_TEST_MOUNT_POINT) !=
	    OSAL_SUCCESS) {
		return false;
	}

	return true;
}

static void mqtt_test_fs_teardown(void)
{
	(void)osal_unmount(MQTT_TEST_MOUNT_POINT);
	(void)osal_rmfs(MQTT_TEST_IMAGE_PATH);
}

static bool write_test_file(const char *path, const char *content)
{
	osal_file_id_t fd;
	size_t len = strlen(content);
	int32_t n;

	fd = osal_open_create(
		path, OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
		OSAL_WRITE_ONLY);
	if (fd < 0)
		return false;

	n = osal_write(fd, content, len);
	(void)osal_close(fd);
	return n >= 0 && (size_t)n == len;
}

static void test_defaults_loaded(void)
{
	bool ssl_enabled = true;
	const char *address = NULL;
	const char *prefix = NULL;
	const char *post_topic = NULL;

	TEST_START("Load Defaults");

	mqtt_config_init();
	address = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
	prefix = mqtt_config_get_string(MQTT_CONFIG_VALUE_TOPIC_PREFIX);
	post_topic = mqtt_config_get_string(MQTT_CONFIG_VALUE_POST_DATA_TOPIC);

	TEST_ASSERT(address != NULL && address[0] != '\0',
		    "Default address is available");
	TEST_ASSERT(prefix != NULL && prefix[0] != '\0',
		    "Default topic prefix is available");
	TEST_ASSERT(post_topic != NULL && post_topic[0] != '\0',
		    "Default post topic is available");
	TEST_ASSERT(mqtt_config_get_bool(&ssl_enabled, MQTT_CONFIG_VALUE_SSL),
		    "Default SSL flag can be read");

	TEST_END();
}

static void test_save_triggers_callback(void)
{
	bool save_ok;

	TEST_START("Save Triggers Callback");

	g_callback_called = false;
	mqtt_config_set_callback(on_apply_config);
	TEST_ASSERT(mqtt_config_set_string("mqtt://127.0.0.1:1883",
				      MQTT_CONFIG_VALUE_ADDRESS),
		    "Address can be updated");

	save_ok = mqtt_config_save();
	TEST_ASSERT(save_ok, "Config save succeeded");
	TEST_ASSERT(g_callback_called, "Apply callback was called");

	mqtt_config_set_callback(NULL);
	TEST_END();
}

static void test_cert_source_file_path(void)
{
	const char *cert_content;
	mqtt_cert_source_t source;
	const char *value;
	const char *test_pem = "-----BEGIN CERTIFICATE-----\nTESTDATA\n"
			       "-----END CERTIFICATE-----\n";
	const char *test_file = "test_ca.pem";

	TEST_START("Cert Source file_path");

	mqtt_config_init();

	TEST_ASSERT(write_test_file(test_file, test_pem),
		    "Test PEM file created");

	TEST_ASSERT(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						test_file,
						MQTT_CONFIG_VALUE_CERT),
		    "Set cert source file_path succeeds");

	cert_content = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CERT);
	TEST_ASSERT(cert_content != NULL && strcmp(cert_content, test_pem) == 0,
		    "Cert content resolved from file");

	TEST_ASSERT(mqtt_config_get_cert_source(&source, &value,
						MQTT_CONFIG_VALUE_CERT),
		    "Get cert source succeeds");
	TEST_ASSERT(source == MQTT_CERT_SOURCE_FILE_PATH,
		    "Source is file_path");
	TEST_ASSERT(value != NULL && strcmp(value, test_file) == 0,
		    "Value is the file path");

	(void)osal_remove(test_file);
	TEST_END();
}

static void test_cert_source_raw(void)
{
	const char *cert_content;
	mqtt_cert_source_t source;
	const char *value;
	const char *raw_pem = "-----BEGIN CERTIFICATE-----\nRAWDATA\n"
			      "-----END CERTIFICATE-----\n";

	TEST_START("Cert Source raw");

	mqtt_config_init();

	TEST_ASSERT(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_RAW, raw_pem,
						MQTT_CONFIG_VALUE_CLIENT_CERT),
		    "Set cert source raw succeeds");

	cert_content = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CLIENT_CERT);
	TEST_ASSERT(cert_content != NULL && strcmp(cert_content, raw_pem) == 0,
		    "Cert content is raw value");

	TEST_ASSERT(mqtt_config_get_cert_source(&source, &value,
						MQTT_CONFIG_VALUE_CLIENT_CERT),
		    "Get cert source succeeds");
	TEST_ASSERT(source == MQTT_CERT_SOURCE_RAW, "Source is raw");

	TEST_END();
}

static void test_cert_source_none(void)
{
	const char *cert_content;

	TEST_START("Cert Source none (clear)");

	mqtt_config_init();

	TEST_ASSERT(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_RAW, "data",
						MQTT_CONFIG_VALUE_CLIENT_KEY),
		    "Set cert source raw first");

	TEST_ASSERT(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_NONE, NULL,
						MQTT_CONFIG_VALUE_CLIENT_KEY),
		    "Clear cert source succeeds");

	cert_content = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CLIENT_KEY);
	TEST_ASSERT(cert_content != NULL && cert_content[0] == '\0',
		    "Cert content is empty after clear");

	TEST_END();
}

static void test_cert_save_load_roundtrip(void)
{
	const char *cert_content;
	mqtt_cert_source_t source;
	const char *value;
	const char *test_pem = "-----BEGIN CERTIFICATE-----\nROUNDTRIP\n"
			       "-----END CERTIFICATE-----\n";
	const char *test_file = "test_roundtrip.pem";

	TEST_START("Cert Save/Load Roundtrip");

	mqtt_config_init();

	TEST_ASSERT(write_test_file(test_file, test_pem),
		    "Test PEM file created");

	TEST_ASSERT(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						test_file,
						MQTT_CONFIG_VALUE_CERT),
		    "Set cert source file_path");
	TEST_ASSERT(mqtt_config_set_string("mqtt://10.0.0.1:8883",
					   MQTT_CONFIG_VALUE_ADDRESS),
		    "Set address");

	TEST_ASSERT(mqtt_config_save(), "Save config");

	/* Re-init to simulate restart */
	mqtt_config_init();

	TEST_ASSERT(mqtt_config_get_cert_source(&source, &value,
						MQTT_CONFIG_VALUE_CERT),
		    "Get cert source after reload");
	TEST_ASSERT(source == MQTT_CERT_SOURCE_FILE_PATH,
		    "Source persisted as file_path");
	TEST_ASSERT(value != NULL && strcmp(value, test_file) == 0,
		    "File path persisted");

	cert_content = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CERT);
	TEST_ASSERT(cert_content != NULL && strcmp(cert_content, test_pem) == 0,
		    "Cert content re-resolved from file after reload");

	(void)osal_remove(test_file);
	TEST_END();
}

static void mqtt_config_tests_reset(void)
{
	tests_run = 0;
	tests_passed = 0;
	tests_failed = 0;
}

int mqtt_config_tests_run(void)
{
	mqtt_config_tests_reset();

	if (!mqtt_test_fs_setup()) {
		printf("[FAIL] MQTT test filesystem setup failed\n");
		return 1;
	}

	printf("\n");
	printf("==================================================\n");
	printf("             MQTT Config Tests                    \n");
	printf("==================================================\n");
	printf("\n");

	test_defaults_loaded();
	test_save_triggers_callback();
	test_cert_source_file_path();
	test_cert_source_raw();
	test_cert_source_none();
	test_cert_save_load_roundtrip();

	printf("\n");
	printf("==================================================\n");
	printf("                  TEST SUMMARY                    \n");
	printf("==================================================\n");
	printf("  Total tests:  %d\n", tests_run);
	printf("  Passed:       %d\n", tests_passed);
	printf("  Failed:       %d\n", tests_failed);
	printf("  Success rate: %.1f%%\n",
	       (tests_run > 0) ? (100.0 * tests_passed / tests_run) : 0.0);
	printf("==================================================\n");

	mqtt_test_fs_teardown();

	return tests_failed;
}

#ifdef ESP_PLATFORM
void app_main(void)
{
	(void)mqtt_config_tests_run();
}
#else
int main(void)
{
	return mqtt_config_tests_run() == 0 ? 0 : 1;
}
#endif
