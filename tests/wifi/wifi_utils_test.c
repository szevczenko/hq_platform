/*
 * Wi-Fi Utils Unit Tests
 *
 * Tests:
 * 1.  IP info to JSON — valid data
 * 2.  IP info to JSON — buffer too small
 * 3.  IP info to JSON — null pointers
 * 4.  AP list to JSON — valid data (multiple APs)
 * 5.  AP list to JSON — empty list
 * 6.  AP list to JSON — buffer too small
 * 7.  AP list to JSON — null pointers
 */

#include <stdio.h>
#include <string.h>

#include "wifi_utils.h"
#include "cJSON.h"

/* Test results tracking */
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Test macros */
#define TEST_ASSERT( condition, message )                       \
  do {                                                          \
    tests_run++;                                                \
    if ( condition ) {                                          \
      tests_passed++;                                           \
      printf( "[PASS] %s\n", message );                         \
    } else {                                                    \
      tests_failed++;                                           \
      printf( "[FAIL] %s\n", message );                         \
    }                                                           \
  } while ( 0 )

#define TEST_START( name )                                      \
  printf( "\n==================================================\n" ); \
  printf( "TEST: %s\n", name );                                 \
  printf( "==================================================\n" )

#define TEST_END() \
  printf( "--------------------------------------------------\n" )

/* ============================================================================
 * Test 1: IP info to JSON — valid data
 * ========================================================================== */
static void test_ip_info_to_json_valid( void )
{
  TEST_START( "IP info to JSON — valid data" );

  wifi_mgmt_ip_info_t info;
  memset( &info, 0, sizeof( info ) );
  strncpy( info.ssid, "TestNetwork", MAX_SSID_SIZE );
  strncpy( info.ip, "192.168.1.100", sizeof( info.ip ) - 1 );
  strncpy( info.netmask, "255.255.255.0", sizeof( info.netmask ) - 1 );
  strncpy( info.gw, "192.168.1.1", sizeof( info.gw ) - 1 );
  info.urc = 0;

  char buf[WIFI_UTILS_IP_JSON_MAX_SIZE];
  osal_status_t st = wifi_utils_ip_info_to_json( &info, buf, sizeof( buf ) );
  TEST_ASSERT( st == OSAL_SUCCESS, "returns SUCCESS" );

  /* Parse and verify JSON content */
  cJSON* root = cJSON_Parse( buf );
  TEST_ASSERT( root != NULL, "output is valid JSON" );

  if ( root )
  {
    cJSON* ssid = cJSON_GetObjectItemCaseSensitive( root, "ssid" );
    TEST_ASSERT( cJSON_IsString( ssid ) && strcmp( ssid->valuestring, "TestNetwork" ) == 0,
                 "ssid field matches" );

    cJSON* ip = cJSON_GetObjectItemCaseSensitive( root, "ip" );
    TEST_ASSERT( cJSON_IsString( ip ) && strcmp( ip->valuestring, "192.168.1.100" ) == 0,
                 "ip field matches" );

    cJSON* netmask = cJSON_GetObjectItemCaseSensitive( root, "netmask" );
    TEST_ASSERT( cJSON_IsString( netmask ) && strcmp( netmask->valuestring, "255.255.255.0" ) == 0,
                 "netmask field matches" );

    cJSON* gw = cJSON_GetObjectItemCaseSensitive( root, "gw" );
    TEST_ASSERT( cJSON_IsString( gw ) && strcmp( gw->valuestring, "192.168.1.1" ) == 0,
                 "gw field matches" );

    cJSON* urc = cJSON_GetObjectItemCaseSensitive( root, "urc" );
    TEST_ASSERT( cJSON_IsNumber( urc ) && urc->valueint == 0, "urc field is 0" );

    cJSON_Delete( root );
  }

  TEST_END();
}

/* ============================================================================
 * Test 2: IP info to JSON — buffer too small
 * ========================================================================== */
static void test_ip_info_to_json_small_buf( void )
{
  TEST_START( "IP info to JSON — buffer too small" );

  wifi_mgmt_ip_info_t info;
  memset( &info, 0, sizeof( info ) );
  strncpy( info.ssid, "TestNetwork", MAX_SSID_SIZE );
  strncpy( info.ip, "192.168.1.100", sizeof( info.ip ) - 1 );

  char buf[10];
  osal_status_t st = wifi_utils_ip_info_to_json( &info, buf, sizeof( buf ) );
  TEST_ASSERT( st == OSAL_ERR_OUTPUT_TOO_LARGE, "returns OUTPUT_TOO_LARGE" );

  TEST_END();
}

/* ============================================================================
 * Test 3: IP info to JSON — null pointers
 * ========================================================================== */
static void test_ip_info_to_json_null( void )
{
  TEST_START( "IP info to JSON — null pointers" );

  char buf[128];
  wifi_mgmt_ip_info_t info = { 0 };

  TEST_ASSERT( wifi_utils_ip_info_to_json( NULL, buf, sizeof( buf ) ) == OSAL_INVALID_POINTER,
               "NULL info" );
  TEST_ASSERT( wifi_utils_ip_info_to_json( &info, NULL, sizeof( buf ) ) == OSAL_INVALID_POINTER,
               "NULL buffer" );
  TEST_ASSERT( wifi_utils_ip_info_to_json( &info, buf, 0 ) == OSAL_INVALID_POINTER,
               "zero size" );

  TEST_END();
}

/* ============================================================================
 * Test 4: AP list to JSON — valid data
 * ========================================================================== */
static void test_ap_list_to_json_valid( void )
{
  TEST_START( "AP list to JSON — multiple APs" );

  wifi_mgmt_ap_list_t list;
  memset( &list, 0, sizeof( list ) );
  list.count = 3;

  strncpy( list.items[0].ssid, "NetworkA", MAX_SSID_SIZE );
  list.items[0].chan  = 1;
  list.items[0].rssi  = -45;
  list.items[0].auth  = 3;

  strncpy( list.items[1].ssid, "NetworkB", MAX_SSID_SIZE );
  list.items[1].chan  = 6;
  list.items[1].rssi  = -60;
  list.items[1].auth  = 4;

  strncpy( list.items[2].ssid, "NetworkC", MAX_SSID_SIZE );
  list.items[2].chan  = 11;
  list.items[2].rssi  = -75;
  list.items[2].auth  = 0;

  char buf[WIFI_UTILS_AP_JSON_MAX_SIZE];
  osal_status_t st = wifi_utils_ap_list_to_json( &list, buf, sizeof( buf ) );
  TEST_ASSERT( st == OSAL_SUCCESS, "returns SUCCESS" );

  /* Parse and verify */
  cJSON* root = cJSON_Parse( buf );
  TEST_ASSERT( root != NULL, "output is valid JSON" );
  TEST_ASSERT( cJSON_IsArray( root ), "root is array" );

  if ( root )
  {
    TEST_ASSERT( cJSON_GetArraySize( root ) == 3, "array has 3 items" );

    cJSON* first = cJSON_GetArrayItem( root, 0 );
    if ( first )
    {
      cJSON* ssid = cJSON_GetObjectItemCaseSensitive( first, "ssid" );
      TEST_ASSERT( cJSON_IsString( ssid ) && strcmp( ssid->valuestring, "NetworkA" ) == 0,
                   "first AP ssid is NetworkA" );

      cJSON* chan = cJSON_GetObjectItemCaseSensitive( first, "chan" );
      TEST_ASSERT( cJSON_IsNumber( chan ) && chan->valueint == 1, "first AP chan is 1" );

      cJSON* rssi = cJSON_GetObjectItemCaseSensitive( first, "rssi" );
      TEST_ASSERT( cJSON_IsNumber( rssi ) && rssi->valueint == -45, "first AP rssi is -45" );

      cJSON* auth = cJSON_GetObjectItemCaseSensitive( first, "auth" );
      TEST_ASSERT( cJSON_IsNumber( auth ) && auth->valueint == 3, "first AP auth is 3" );
    }

    cJSON_Delete( root );
  }

  TEST_END();
}

/* ============================================================================
 * Test 5: AP list to JSON — empty list
 * ========================================================================== */
static void test_ap_list_to_json_empty( void )
{
  TEST_START( "AP list to JSON — empty list" );

  wifi_mgmt_ap_list_t list;
  memset( &list, 0, sizeof( list ) );

  char buf[256];
  osal_status_t st = wifi_utils_ap_list_to_json( &list, buf, sizeof( buf ) );
  TEST_ASSERT( st == OSAL_SUCCESS, "returns SUCCESS" );

  cJSON* root = cJSON_Parse( buf );
  TEST_ASSERT( root != NULL, "output is valid JSON" );
  TEST_ASSERT( cJSON_IsArray( root ) && cJSON_GetArraySize( root ) == 0, "empty array" );
  cJSON_Delete( root );

  TEST_END();
}

/* ============================================================================
 * Test 6: AP list to JSON — buffer too small
 * ========================================================================== */
static void test_ap_list_to_json_small_buf( void )
{
  TEST_START( "AP list to JSON — buffer too small" );

  wifi_mgmt_ap_list_t list;
  memset( &list, 0, sizeof( list ) );
  list.count = 1;
  strncpy( list.items[0].ssid, "SomeNetwork", MAX_SSID_SIZE );

  char buf[5];
  osal_status_t st = wifi_utils_ap_list_to_json( &list, buf, sizeof( buf ) );
  TEST_ASSERT( st == OSAL_ERR_OUTPUT_TOO_LARGE, "returns OUTPUT_TOO_LARGE" );

  TEST_END();
}

/* ============================================================================
 * Test 7: AP list to JSON — null pointers
 * ========================================================================== */
static void test_ap_list_to_json_null( void )
{
  TEST_START( "AP list to JSON — null pointers" );

  char buf[256];
  wifi_mgmt_ap_list_t list = { 0 };

  TEST_ASSERT( wifi_utils_ap_list_to_json( NULL, buf, sizeof( buf ) ) == OSAL_INVALID_POINTER,
               "NULL list" );
  TEST_ASSERT( wifi_utils_ap_list_to_json( &list, NULL, sizeof( buf ) ) == OSAL_INVALID_POINTER,
               "NULL buffer" );
  TEST_ASSERT( wifi_utils_ap_list_to_json( &list, buf, 0 ) == OSAL_INVALID_POINTER,
               "zero size" );

  TEST_END();
}

/* ============================================================================
 * Runner
 * ========================================================================== */

int wifi_utils_tests_run( void )
{
  tests_run    = 0;
  tests_passed = 0;
  tests_failed = 0;

  printf( "\n" );
  printf( "==================================================\n" );
  printf( "           Wi-Fi Utils Tests                      \n" );
  printf( "==================================================\n" );

  test_ip_info_to_json_valid();
  test_ip_info_to_json_small_buf();
  test_ip_info_to_json_null();
  test_ap_list_to_json_valid();
  test_ap_list_to_json_empty();
  test_ap_list_to_json_small_buf();
  test_ap_list_to_json_null();

  printf( "\n" );
  printf( "==================================================\n" );
  printf( "                  TEST SUMMARY                    \n" );
  printf( "==================================================\n" );
  printf( "  Total tests:  %d\n", tests_run );
  printf( "  Passed:       %d\n", tests_passed );
  printf( "  Failed:       %d\n", tests_failed );
  printf( "  Success rate: %.1f%%\n",
          ( tests_run > 0 ) ? ( 100.0 * tests_passed / tests_run ) : 0.0 );
  printf( "==================================================\n" );

  if ( tests_failed == 0 )
  {
    printf( "\nALL TESTS PASSED\n\n" );
  }
  else
  {
    printf( "\nSOME TESTS FAILED\n\n" );
  }

  return tests_failed;
}

#ifndef OSAL_TESTS_AGGREGATE

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  int failed = wifi_utils_tests_run();

#ifndef ESP_PLATFORM
  return ( failed == 0 ) ? 0 : 1;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
