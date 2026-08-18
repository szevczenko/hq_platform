#include "tb_test_common.h"

/* Unity lifecycle hooks — owner of setUp/tearDown per executable runner rule */
void setUp(void)
{
}

void tearDown(void)
{
}

#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(void)
#endif
{
	UNITY_BEGIN();

	run_client_tests();
	run_telemetry_tests();
	run_attributes_tests();
	run_rpc_tests();
	run_provision_claim_tests();
	run_fwu_tests();

#ifndef ESP_PLATFORM
	return UNITY_END();
#else
	UNITY_END();
#endif
}
