/*
 * MQTT Config Tests
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_config.h"
#include "osal_file.h"
#include "osal_mount.h"
#include "unity.h"

#ifdef ESP_PLATFORM
#define MQTT_TEST_IMAGE_PATH  "flash_test"
#define MQTT_TEST_MOUNT_POINT "/littlefs"
#else
#define MQTT_TEST_IMAGE_PATH  "/tmp/mqtt_config_test.img"
#define MQTT_TEST_MOUNT_POINT "/"
#endif

static bool g_callback_called = false;

void setUp(void)
{
	g_callback_called = false;
}

void tearDown(void)
{
	g_callback_called = false;
}

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

	mqtt_config_init();
	address = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
	prefix = mqtt_config_get_string(MQTT_CONFIG_VALUE_TOPIC_PREFIX);
	post_topic = mqtt_config_get_string(MQTT_CONFIG_VALUE_POST_DATA_TOPIC);

	TEST_ASSERT_NOT_NULL(address);
	TEST_ASSERT_NOT_EMPTY(address);
	TEST_ASSERT_NOT_NULL(prefix);
	TEST_ASSERT_NOT_EMPTY(prefix);
	TEST_ASSERT_NOT_NULL(post_topic);
	TEST_ASSERT_NOT_EMPTY(post_topic);
	TEST_ASSERT_TRUE(
		mqtt_config_get_bool(&ssl_enabled, MQTT_CONFIG_VALUE_SSL));
}

static void test_save_triggers_callback(void)
{
	bool save_ok;

	g_callback_called = false;
	mqtt_config_set_callback(on_apply_config);
	TEST_ASSERT_TRUE(mqtt_config_set_string("mqtt://127.0.0.1:1883",
						  MQTT_CONFIG_VALUE_ADDRESS));

	save_ok = mqtt_config_save();
	TEST_ASSERT_TRUE(save_ok);
	TEST_ASSERT_TRUE(g_callback_called);

	mqtt_config_set_callback(NULL);
}

static void test_cert_source_file_path(void)
{
	const char *cert_content;
	mqtt_cert_source_t source;
	const char *value;
	const char *test_pem = "-----BEGIN CERTIFICATE-----\nTESTDATA\n"
			       "-----END CERTIFICATE-----\n";
	const char *test_file = "test_ca.pem";

	mqtt_config_init();

	TEST_ASSERT_TRUE(write_test_file(test_file, test_pem));

	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						     test_file,
						     MQTT_CONFIG_VALUE_CERT));

	cert_content = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CERT);
	TEST_ASSERT_NOT_NULL(cert_content);
	TEST_ASSERT_EQUAL_STRING(test_pem, cert_content);

	TEST_ASSERT_TRUE(mqtt_config_get_cert_source(&source, &value,
						     MQTT_CONFIG_VALUE_CERT));
	TEST_ASSERT_EQUAL_INT((int)MQTT_CERT_SOURCE_FILE_PATH, (int)source);
	TEST_ASSERT_NOT_NULL(value);
	TEST_ASSERT_EQUAL_STRING(test_file, value);

	(void)osal_remove(test_file);
}

static void test_cert_source_raw(void)
{
	const char *cert_content;
	mqtt_cert_source_t source;
	const char *value;
	const char *raw_pem = "-----BEGIN CERTIFICATE-----\nRAWDATA\n"
			      "-----END CERTIFICATE-----\n";

	mqtt_config_init();

	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_RAW,
						     raw_pem,
						     MQTT_CONFIG_VALUE_CLIENT_CERT));

	cert_content = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CLIENT_CERT);
	TEST_ASSERT_NOT_NULL(cert_content);
	TEST_ASSERT_EQUAL_STRING(raw_pem, cert_content);

	TEST_ASSERT_TRUE(mqtt_config_get_cert_source(&source, &value,
						     MQTT_CONFIG_VALUE_CLIENT_CERT));
	TEST_ASSERT_EQUAL_INT((int)MQTT_CERT_SOURCE_RAW, (int)source);
}

static void test_cert_source_none(void)
{
	const char *cert_content;

	mqtt_config_init();

	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_RAW,
						     "data",
						     MQTT_CONFIG_VALUE_CLIENT_KEY));

	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_NONE,
						     NULL,
						     MQTT_CONFIG_VALUE_CLIENT_KEY));

	cert_content = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CLIENT_KEY);
	TEST_ASSERT_NOT_NULL(cert_content);
	TEST_ASSERT_EMPTY(cert_content);
}

static void test_cert_save_load_roundtrip(void)
{
	const char *cert_content;
	mqtt_cert_source_t source;
	const char *value;
	const char *test_pem = "-----BEGIN CERTIFICATE-----\nROUNDTRIP\n"
			       "-----END CERTIFICATE-----\n";
	const char *test_file = "test_roundtrip.pem";

	mqtt_config_init();

	TEST_ASSERT_TRUE(write_test_file(test_file, test_pem));

	TEST_ASSERT_TRUE(mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						     test_file,
						     MQTT_CONFIG_VALUE_CERT));
	TEST_ASSERT_TRUE(mqtt_config_set_string("mqtt://10.0.0.1:8883",
						   MQTT_CONFIG_VALUE_ADDRESS));

	TEST_ASSERT_TRUE(mqtt_config_save());

	/* Re-init to simulate restart */
	mqtt_config_init();

	TEST_ASSERT_TRUE(mqtt_config_get_cert_source(&source, &value,
						     MQTT_CONFIG_VALUE_CERT));
	TEST_ASSERT_EQUAL_INT((int)MQTT_CERT_SOURCE_FILE_PATH, (int)source);
	TEST_ASSERT_NOT_NULL(value);
	TEST_ASSERT_EQUAL_STRING(test_file, value);

	cert_content = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CERT);
	TEST_ASSERT_NOT_NULL(cert_content);
	TEST_ASSERT_EQUAL_STRING(test_pem, cert_content);

	(void)osal_remove(test_file);
}

static void mqtt_config_tests_run(void)
{
	if (!mqtt_test_fs_setup()) {
		printf("MQTT test filesystem setup failed\n");
		return;
	}

	RUN_TEST(test_defaults_loaded);
	RUN_TEST(test_save_triggers_callback);
	RUN_TEST(test_cert_source_file_path);
	RUN_TEST(test_cert_source_raw);
	RUN_TEST(test_cert_source_none);
	RUN_TEST(test_cert_save_load_roundtrip);

	mqtt_test_fs_teardown();
}

#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(void)
#endif
{
	UNITY_BEGIN();

	mqtt_config_tests_run();

#ifndef ESP_PLATFORM
	return UNITY_END();
#else
	UNITY_END();
#endif
}