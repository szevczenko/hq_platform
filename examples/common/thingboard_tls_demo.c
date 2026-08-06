/*
 * ThingsBoard TLS Demo
 *
 * Demonstrates MQTT over TLS using mqtts:// with server CA verification,
 * optional client certificate/key, and expected failure scenarios.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mongoose_process.h"
#include "mqtt_config.h"
#include "osal_task.h"
#include "tb_client.h"
#include "tb_telemetry.h"

#ifndef CONFIG_TLS_DEMO_CLIENT_ID
#define CONFIG_TLS_DEMO_CLIENT_ID "tb_tls_demo_0001"
#endif
#ifndef CONFIG_TLS_DEMO_USERNAME
#define CONFIG_TLS_DEMO_USERNAME "PUT_ACCESS_TOKEN_HERE"
#endif
#ifndef CONFIG_TLS_DEMO_PASSWORD
#define CONFIG_TLS_DEMO_PASSWORD ""
#endif
#ifndef CONFIG_TLS_DEMO_DEVICE_NAME
#define CONFIG_TLS_DEMO_DEVICE_NAME "TLS Demo Device"
#endif
#ifndef CONFIG_TLS_DEMO_MQTT_URL
#define CONFIG_TLS_DEMO_MQTT_URL "mqtts://localhost:8883"
#endif
#ifndef CONFIG_TLS_DEMO_CA_CERT_PATH
#define CONFIG_TLS_DEMO_CA_CERT_PATH "ca.crt"
#endif
#ifndef CONFIG_TLS_DEMO_CLIENT_CERT_PATH
#define CONFIG_TLS_DEMO_CLIENT_CERT_PATH ""
#endif
#ifndef CONFIG_TLS_DEMO_CLIENT_KEY_PATH
#define CONFIG_TLS_DEMO_CLIENT_KEY_PATH ""
#endif
#ifndef CONFIG_TLS_DEMO_UNKNOWN_CA_PATH
#define CONFIG_TLS_DEMO_UNKNOWN_CA_PATH ""
#endif
#ifndef CONFIG_TLS_DEMO_HOSTNAME_MISMATCH_URL
#define CONFIG_TLS_DEMO_HOSTNAME_MISMATCH_URL ""
#endif
#ifndef CONFIG_TLS_DEMO_CONNECT_TIMEOUT_MS
#define CONFIG_TLS_DEMO_CONNECT_TIMEOUT_MS 10000
#endif
#ifndef CONFIG_TLS_DEMO_RECONNECT_DELAY_MS
#define CONFIG_TLS_DEMO_RECONNECT_DELAY_MS 3000
#endif

#define TLS_DEMO_CLIENT_ID CONFIG_TLS_DEMO_CLIENT_ID
#define TLS_DEMO_USERNAME CONFIG_TLS_DEMO_USERNAME
#define TLS_DEMO_PASSWORD CONFIG_TLS_DEMO_PASSWORD
#define TLS_DEMO_DEVICE_NAME CONFIG_TLS_DEMO_DEVICE_NAME
#define TLS_DEMO_MQTT_URL CONFIG_TLS_DEMO_MQTT_URL
#define TLS_DEMO_CA_CERT_PATH CONFIG_TLS_DEMO_CA_CERT_PATH
#define TLS_DEMO_CLIENT_CERT_PATH CONFIG_TLS_DEMO_CLIENT_CERT_PATH
#define TLS_DEMO_CLIENT_KEY_PATH CONFIG_TLS_DEMO_CLIENT_KEY_PATH
#define TLS_DEMO_UNKNOWN_CA_PATH CONFIG_TLS_DEMO_UNKNOWN_CA_PATH
#define TLS_DEMO_HOSTNAME_MISMATCH_URL CONFIG_TLS_DEMO_HOSTNAME_MISMATCH_URL
#define TLS_DEMO_CONNECT_TIMEOUT_MS CONFIG_TLS_DEMO_CONNECT_TIMEOUT_MS
#define TLS_DEMO_RECONNECT_DELAY_MS CONFIG_TLS_DEMO_RECONNECT_DELAY_MS

static tb_client_t *g_client = NULL;
static volatile int g_connect_count = 0;
static volatile int g_disconnect_count = 0;
static volatile int g_connect_failure_count = 0;

static void on_connect(tb_client_t *client, void *user_data)
{
	(void)client;
	(void)user_data;
	g_connect_count++;
	printf("[TLS_DEMO] Connected\n");
}

static void on_disconnect(tb_client_t *client,
			 tb_client_disconnect_reason_t reason,
			 void *user_data)
{
	(void)client;
	(void)user_data;
	g_disconnect_count++;
	printf("[TLS_DEMO] Disconnected (reason=%d)\n", (int)reason);
}

static void on_connect_failure(tb_client_t *client,
			      tb_client_connect_failure_reason_t reason,
			      void *user_data)
{
	(void)client;
	(void)user_data;
	g_connect_failure_count++;
	printf("[TLS_DEMO] Connect failure (reason=%d)\n", (int)reason);
}

static void reset_connection_state(void)
{
	g_connect_count = 0;
	g_disconnect_count = 0;
	g_connect_failure_count = 0;
}

static bool configure_tls_adapter(const char *ca_path, const char *client_cert,
				  const char *client_key)
{
	if (!mqtt_config_set_bool(true, MQTT_CONFIG_VALUE_SSL)) {
		return false;
	}
	if (!mqtt_config_set_bool(false, MQTT_CONFIG_VALUE_SKIP_VERIFY)) {
		return false;
	}
	if (ca_path == NULL || ca_path[0] == '\0') {
		return false;
	}
	if (!mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH, ca_path,
					 MQTT_CONFIG_VALUE_CERT)) {
		return false;
	}

	if (client_cert != NULL && client_key != NULL && client_cert[0] != '\0' &&
	    client_key[0] != '\0') {
		if (!mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						 client_cert,
						 MQTT_CONFIG_VALUE_CLIENT_CERT)) {
			return false;
		}
		if (!mqtt_config_set_cert_source(MQTT_CERT_SOURCE_FILE_PATH,
						 client_key,
						 MQTT_CONFIG_VALUE_CLIENT_KEY)) {
			return false;
		}
	}

	return true;
}

static int init_client_for_url(const char *url)
{
	tb_client_config_t cfg;

	memset(&cfg, 0, sizeof(cfg));
	strncpy(cfg.server_url, url, sizeof(cfg.server_url) - 1);
	strncpy(cfg.access_token, TLS_DEMO_USERNAME, sizeof(cfg.access_token) - 1);
	strncpy(cfg.client_id, TLS_DEMO_CLIENT_ID, sizeof(cfg.client_id) - 1);
	strncpy(cfg.device_name, TLS_DEMO_DEVICE_NAME, sizeof(cfg.device_name) - 1);
	cfg.on_connect = on_connect;
	cfg.on_disconnect = on_disconnect;
	cfg.on_connect_failure = on_connect_failure;

	return tb_client_init(&g_client, &cfg);
}

static int wait_connection_result(bool expect_success)
{
	uint32_t waited_ms = 0;
	while (waited_ms < TLS_DEMO_CONNECT_TIMEOUT_MS) {
		if (tb_client_is_connected(g_client)) {
			return expect_success ? 0 : -1;
		}
		if (!expect_success && g_connect_failure_count > 0) {
			return 0;
		}
		osal_task_delay_ms(100);
		waited_ms += 100;
	}

	if (!expect_success && !tb_client_is_connected(g_client)) {
		return 0;
	}
	return -1;
}

static int run_tls_attempt(const char *name, const char *url, const char *ca_path,
			   bool expect_success)
{
	int rc;

	printf("[TLS_DEMO] Scenario: %s\n", name);
	reset_connection_state();

	rc = init_client_for_url(url);
	if (rc != 0) {
		printf("[TLS_DEMO] tb_client_init failed: %d\n", rc);
		return -1;
	}

	mqtt_config_set_string(TLS_DEMO_PASSWORD, MQTT_CONFIG_VALUE_PASSWORD);
	if (!configure_tls_adapter(ca_path, TLS_DEMO_CLIENT_CERT_PATH,
				   TLS_DEMO_CLIENT_KEY_PATH)) {
		printf("[TLS_DEMO] TLS adapter configuration failed\n");
		tb_client_deinit(g_client);
		g_client = NULL;
		return -1;
	}

	rc = tb_client_connect(g_client);
	if (rc != 0) {
		printf("[TLS_DEMO] tb_client_connect failed: %d\n", rc);
		tb_client_deinit(g_client);
		g_client = NULL;
		return -1;
	}

	rc = wait_connection_result(expect_success);
	if (rc != 0) {
		if (expect_success) {
			printf("[TLS_DEMO] Expected successful TLS connection but got failure\n");
		} else {
			printf("[TLS_DEMO] Expected TLS failure but connected unexpectedly\n");
		}
		tb_client_deinit(g_client);
		g_client = NULL;
		return -1;
	}

	if (expect_success) {
		printf("[TLS_DEMO] TLS connection established with verification\n");
		(void)tb_telemetry_send_string(g_client, "tls_state", "connected");
		osal_task_delay_ms(300);
	} else {
		printf("[TLS_DEMO] Expected TLS failure observed\n");
	}

	tb_client_deinit(g_client);
	g_client = NULL;
	osal_task_delay_ms(TLS_DEMO_RECONNECT_DELAY_MS);
	return 0;
}

int main_function(void)
{
	int rc;

	printf("\n=============================================\n");
	printf("   ThingsBoard TLS Demo\n");
	printf("=============================================\n");
	printf("  Client ID : %s\n", TLS_DEMO_CLIENT_ID);
	printf("  Broker    : %s\n", TLS_DEMO_MQTT_URL);
	printf("=============================================\n\n");

	MongooseProcess_Init();

	rc = run_tls_attempt("verified tls", TLS_DEMO_MQTT_URL,
			     TLS_DEMO_CA_CERT_PATH, true);
	if (rc != 0) {
		printf("[TLS_DEMO] Verified TLS scenario failed\n");
		MongooseProcess_Deinit();
		return 1;
	}

	if (TLS_DEMO_UNKNOWN_CA_PATH[0] != '\0') {
		rc = run_tls_attempt("unknown CA failure", TLS_DEMO_MQTT_URL,
				     TLS_DEMO_UNKNOWN_CA_PATH, false);
		if (rc != 0) {
			printf("[TLS_DEMO] Unknown CA failure scenario did not fail as expected\n");
			MongooseProcess_Deinit();
			return 1;
		}
	} else {
		printf("[TLS_DEMO] Unknown CA scenario skipped (no CA path provided)\n");
	}

	if (TLS_DEMO_HOSTNAME_MISMATCH_URL[0] != '\0') {
		rc = run_tls_attempt("hostname mismatch failure",
				     TLS_DEMO_HOSTNAME_MISMATCH_URL,
				     TLS_DEMO_CA_CERT_PATH, false);
		if (rc != 0) {
			printf("[TLS_DEMO] Hostname mismatch scenario did not fail as expected\n");
			MongooseProcess_Deinit();
			return 1;
		}
	} else {
		printf("[TLS_DEMO] Hostname mismatch scenario skipped (no mismatch URL provided)\n");
	}

	printf("[TLS_DEMO] All configured TLS scenarios completed\n");
	MongooseProcess_Deinit();
	return 0;
}

#ifndef CONFIG_HQ_PLATFORM_ESP
int main(void)
{
	return main_function();
}
#endif
