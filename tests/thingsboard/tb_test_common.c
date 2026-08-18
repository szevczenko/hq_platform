#include "tb_test_common.h"

tb_client_t *create_test_client(void)
{
	mqtt_app_mock_reset();

	tb_client_config_t cfg = {
		.server_url = "mqtt://localhost:1883",
		.access_token = "test_token",
		.device_name = "test_device",
	};

	tb_client_t *client = NULL;
	int ret = tb_client_init(&client, &cfg);
	if (ret != 0 || client == NULL) {
		return NULL;
	}
	tb_client_connect(client);
	return client;
}

void destroy_test_client(tb_client_t *client)
{
	if (client) {
		tb_client_deinit(client);
	}
}

uint32_t parse_topic_suffix_id(const char *topic)
{
	const char *id_start;

	if (topic == NULL) {
		return 0;
	}

	id_start = strrchr(topic, '/');
	if (id_start == NULL || *(id_start + 1) == '\0') {
		return 0;
	}

	return (uint32_t)strtoul(id_start + 1, NULL, 10);
}

int find_last_publish_with_prefix(const char *prefix)
{
	for (int i = mock_publish_count - 1; i >= 0; i--) {
		if (strncmp(mock_publishes[i].topic, prefix, strlen(prefix)) == 0) {
			return i;
		}
	}
	return -1;
}

int find_last_publish_on_topic(const char *topic)
{
	for (int i = mock_publish_count - 1; i >= 0; i--) {
		if (strcmp(mock_publishes[i].topic, topic) == 0) {
			return i;
		}
	}
	return -1;
}
