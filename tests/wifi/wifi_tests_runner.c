/*
 * Wi-Fi Aggregated Tests Runner
 *
 * Runs all Wi-Fi tests: config, utils, and management state machine.
 */

#include <stdio.h>

#include "unity.h"

void wifi_config_tests_run( void );
void wifi_utils_tests_run( void );
void wifi_mgmt_tests_run( void );

void setUp( void )
{
}

void tearDown( void )
{
}

int main( void )
{
  UNITY_BEGIN();

  wifi_config_tests_run();
  wifi_utils_tests_run();
  wifi_mgmt_tests_run();

  return UNITY_END();
}
