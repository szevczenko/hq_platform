#include "tb_test_common.h"

#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(void)
#endif
{
	printf("\n==================================================\n");
	printf("        ThingsBoard Client Unit Tests            \n");
	printf("==================================================\n");

	run_client_tests();
	run_telemetry_tests();
	run_attributes_tests();
	run_rpc_tests();
	run_provision_claim_tests();
	run_fwu_tests();

	printf("\n==================================================\n");
	printf("              TEST SUMMARY                       \n");
	printf("==================================================\n");
	printf("  Run:    %d\n", tests_run);
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("==================================================\n");

#ifndef ESP_PLATFORM
	return (tests_failed == 0) ? 0 : 1;
#endif
}
