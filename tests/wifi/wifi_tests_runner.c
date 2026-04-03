/*
 * Wi-Fi Aggregated Tests Runner
 *
 * Runs all Wi-Fi tests: config, utils, and management state machine.
 */

#include <stdio.h>

int wifi_config_tests_run( void );
int wifi_utils_tests_run( void );
int wifi_mgmt_tests_run( void );

int main( void )
{
  int failed_total = 0;

  printf( "\n==================================================\n" );
  printf( "          Wi-Fi Aggregated Test Run               \n" );
  printf( "==================================================\n\n" );

  failed_total += wifi_config_tests_run();
  failed_total += wifi_utils_tests_run();
  failed_total += wifi_mgmt_tests_run();

  printf( "\n==================================================\n" );
  printf( "              AGGREGATED SUMMARY                  \n" );
  printf( "==================================================\n" );
  printf( "  Total failed tests: %d\n", failed_total );
  printf( "==================================================\n" );

  return ( failed_total == 0 ) ? 0 : 1;
}
