#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_app.h"

#define MQTT_DEMO_CMD_BUF_SIZE 768
#define MQTT_DEMO_TIMEOUT_MS 2000

static void mqtt_demo_on_message(const char *topic, const char *message,
				 size_t message_len)
{
	printf("[MQTT RX] topic=%s payload=%.*s\n", topic,
	       (int)message_len, message ? message : "");
}

static void mqtt_demo_print_help(void)
{
	printf("Commands:\n");
	printf("  status\n");
	printf("  pub <topic> <message>\n");
	printf("  sub <topic>\n");
	printf("  unsub <topic>\n");
	printf("  help\n");
	printf("  exit\n");
}

int main(void)
{
	char line[MQTT_DEMO_CMD_BUF_SIZE] = { 0 };

	printf("\n=============================================\n");
	printf("              MQTT Demo Application          \n");
	printf("=============================================\n");

	mqtt_app_init();
	mqtt_demo_print_help();

	while (true) {
		char *cmd = NULL;
		char *arg1 = NULL;
		char *arg2 = NULL;

		printf("mqtt> ");
		if (fgets(line, sizeof(line), stdin) == NULL) {
			break;
		}

		line[strcspn(line, "\r\n")] = '\0';
		cmd = strtok(line, " ");
		if (cmd == NULL) {
			continue;
		}

		if (strcmp(cmd, "status") == 0) {
			printf("connected=%s\n",
			       mqtt_app_is_connected() ? "true" : "false");
			continue;
		}

		if (strcmp(cmd, "pub") == 0) {
			arg1 = strtok(NULL, " ");
			arg2 = strtok(NULL, "");
			if (!arg1 || !arg2) {
				printf("usage: pub <topic> <message>\n");
				continue;
			}
			printf("publish=%s\n",
			       mqtt_app_post_data(arg1, arg2, 0) ? "ok" : "failed");
			continue;
		}

		if (strcmp(cmd, "sub") == 0) {
			arg1 = strtok(NULL, " ");
			if (!arg1) {
				printf("usage: sub <topic>\n");
				continue;
			}
			printf("subscribe=%s\n",
			       mqtt_app_subscribe(arg1, 0, mqtt_demo_on_message,
						 MQTT_DEMO_TIMEOUT_MS)
				       ? "ok"
				       : "failed");
			continue;
		}

		if (strcmp(cmd, "unsub") == 0) {
			arg1 = strtok(NULL, " ");
			if (!arg1) {
				printf("usage: unsub <topic>\n");
				continue;
			}
			printf("unsubscribe=%s\n",
			       mqtt_app_unsubscribe(arg1, MQTT_DEMO_TIMEOUT_MS)
				       ? "ok"
				       : "failed");
			continue;
		}

		if (strcmp(cmd, "help") == 0) {
			mqtt_demo_print_help();
			continue;
		}

		if (strcmp(cmd, "exit") == 0) {
			break;
		}

		printf("unknown command: %s\n", cmd);
	}

	mqtt_app_deinit();
	return 0;
}
