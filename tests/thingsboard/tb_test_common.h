#ifndef TB_TEST_COMMON_H
#define TB_TEST_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mqtt_app_mock.h"
#include "mqtt_config.h"
#include "osal_ota.h"
#include "osal_ota_state.h"
#include "osal_task.h"
#include "tb_attributes.h"
#include "tb_claim.h"
#include "tb_client.h"
#include "tb_firmware_update.h"
#include "tb_provision.h"
#include "tb_rpc.h"
#include "tb_telemetry.h"

#define FW_SHA256_ABCDEFGH "9ac2197d9258257b1ae8463e4214e4cd0a578bc1517f2415928b91be4283fc48"

extern int tests_run;
extern int tests_passed;
extern int tests_failed;

#define TEST_ASSERT(condition, message)                                       \
	do {                                                                  \
		tests_run++;                                                  \
		if (condition) {                                              \
			tests_passed++;                                       \
			printf("  [PASS] %s\n", message);                     \
		} else {                                                      \
			tests_failed++;                                       \
			printf("  [FAIL] %s (line %d)\n", message, __LINE__); \
		}                                                             \
	} while (0)

#define TEST_START(name)                                                  \
	printf("\n--------------------------------------------------\n"); \
	printf("TEST: %s\n", name);                                       \
	printf("--------------------------------------------------\n")

tb_client_t *create_test_client(void);
void destroy_test_client(tb_client_t *client);
uint32_t parse_topic_suffix_id(const char *topic);
int find_last_publish_with_prefix(const char *prefix);
int find_last_publish_on_topic(const char *topic);

void run_client_tests(void);
void run_telemetry_tests(void);
void run_attributes_tests(void);
void run_rpc_tests(void);
void run_provision_claim_tests(void);
void run_fwu_tests(void);

#endif
