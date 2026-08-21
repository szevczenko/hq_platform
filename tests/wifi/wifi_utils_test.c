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
#include "unity.h"

/* ============================================================================
 * Test 1: IP info to JSON — valid data
 * ========================================================================== */
static void test_ip_info_to_json_valid( void )
{
  wifi_mgmt_ip_info_t info;
  memset( &info, 0, sizeof( info ) );
  strncpy( info.ssid, "TestNetwork", MAX_SSID_SIZE );
  strncpy( info.ip, "192.168.1.100", sizeof( info.ip ) - 1 );
  strncpy( info.netmask, "255.255.255.0", sizeof( info.netmask ) - 1 );
  strncpy( info.gw, "192.168.1.1", sizeof( info.gw ) - 1 );
  info.urc = 0;

  char buf[WIFI_UTILS_IP_JSON_MAX_SIZE];
  osal_status_t st = wifi_utils_ip_info_to_json( &info, buf, sizeof( buf ) );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, st, "returns SUCCESS" );

  /* Parse and verify JSON content */
  cJSON* root = cJSON_Parse( buf );
  TEST_ASSERT_NOT_NULL_MESSAGE( root, "output is valid JSON" );

  if ( root )
  {
    cJSON* ssid = cJSON_GetObjectItemCaseSensitive( root, "ssid" );
    TEST_ASSERT_NOT_NULL_MESSAGE( ssid, "ssid field exists" );
    TEST_ASSERT_TRUE_MESSAGE( cJSON_IsString( ssid ), "ssid field is a string" );
    TEST_ASSERT_EQUAL_STRING_MESSAGE( "TestNetwork", ssid->valuestring, "ssid field matches" );

    cJSON* ip = cJSON_GetObjectItemCaseSensitive( root, "ip" );
    TEST_ASSERT_NOT_NULL_MESSAGE( ip, "ip field exists" );
    TEST_ASSERT_TRUE_MESSAGE( cJSON_IsString( ip ), "ip field is a string" );
    TEST_ASSERT_EQUAL_STRING_MESSAGE( "192.168.1.100", ip->valuestring, "ip field matches" );

    cJSON* netmask = cJSON_GetObjectItemCaseSensitive( root, "netmask" );
    TEST_ASSERT_NOT_NULL_MESSAGE( netmask, "netmask field exists" );
    TEST_ASSERT_TRUE_MESSAGE( cJSON_IsString( netmask ), "netmask field is a string" );
    TEST_ASSERT_EQUAL_STRING_MESSAGE( "255.255.255.0", netmask->valuestring, "netmask field matches" );

    cJSON* gw = cJSON_GetObjectItemCaseSensitive( root, "gw" );
    TEST_ASSERT_NOT_NULL_MESSAGE( gw, "gw field exists" );
    TEST_ASSERT_TRUE_MESSAGE( cJSON_IsString( gw ), "gw field is a string" );
    TEST_ASSERT_EQUAL_STRING_MESSAGE( "192.168.1.1", gw->valuestring, "gw field matches" );

    cJSON* urc = cJSON_GetObjectItemCaseSensitive( root, "urc" );
    TEST_ASSERT_NOT_NULL_MESSAGE( urc, "urc field exists" );
    TEST_ASSERT_TRUE_MESSAGE( cJSON_IsNumber( urc ), "urc field is a number" );
    TEST_ASSERT_EQUAL_MESSAGE( 0, urc->valueint, "urc field is 0" );

    cJSON_Delete( root );
  }
}

/* ============================================================================
 * Test 2: IP info to JSON — buffer too small
 * ========================================================================== */
static void test_ip_info_to_json_small_buf( void )
{
  wifi_mgmt_ip_info_t info;
  memset( &info, 0, sizeof( info ) );
  strncpy( info.ssid, "TestNetwork", MAX_SSID_SIZE );
  strncpy( info.ip, "192.168.1.100", sizeof( info.ip ) - 1 );

  char buf[10];
  osal_status_t st = wifi_utils_ip_info_to_json( &info, buf, sizeof( buf ) );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERR_OUTPUT_TOO_LARGE, st, "returns OUTPUT_TOO_LARGE" );
}

/* ============================================================================
 * Test 3: IP info to JSON — null pointers
 * ========================================================================== */
static void test_ip_info_to_json_null( void )
{
  char buf[128];
  wifi_mgmt_ip_info_t info = { 0 };

  TEST_ASSERT_EQUAL_MESSAGE( OSAL_INVALID_POINTER,
                             wifi_utils_ip_info_to_json( NULL, buf, sizeof( buf ) ),
                             "NULL info" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_INVALID_POINTER,
                             wifi_utils_ip_info_to_json( &info, NULL, sizeof( buf ) ),
                             "NULL buffer" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_INVALID_POINTER,
                             wifi_utils_ip_info_to_json( &info, buf, 0 ),
                             "zero size" );
}

/* ============================================================================
 * Test 4: AP list to JSON — valid data
 * ========================================================================== */
static void test_ap_list_to_json_valid( void )
{
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
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, st, "returns SUCCESS" );

  /* Parse and verify */
  cJSON* root = cJSON_Parse( buf );
  TEST_ASSERT_NOT_NULL_MESSAGE( root, "output is valid JSON" );
  TEST_ASSERT_TRUE_MESSAGE( cJSON_IsArray( root ), "root is array" );

  if ( root )
  {
    TEST_ASSERT_EQUAL_MESSAGE( 3, cJSON_GetArraySize( root ), "array has 3 items" );

    cJSON* first = cJSON_GetArrayItem( root, 0 );
    TEST_ASSERT_NOT_NULL_MESSAGE( first, "first array item exists" );
    if ( first )
    {
      cJSON* ssid = cJSON_GetObjectItemCaseSensitive( first, "ssid" );
      TEST_ASSERT_NOT_NULL_MESSAGE( ssid, "ssid field exists" );
      TEST_ASSERT_TRUE_MESSAGE( cJSON_IsString( ssid ), "ssid field is a string" );
      TEST_ASSERT_EQUAL_STRING_MESSAGE( "NetworkA", ssid->valuestring, "first AP ssid is NetworkA" );

      cJSON* chan = cJSON_GetObjectItemCaseSensitive( first, "chan" );
      TEST_ASSERT_NOT_NULL_MESSAGE( chan, "chan field exists" );
      TEST_ASSERT_TRUE_MESSAGE( cJSON_IsNumber( chan ), "chan field is a number" );
      TEST_ASSERT_EQUAL_MESSAGE( 1, chan->valueint, "first AP chan is 1" );

      cJSON* rssi = cJSON_GetObjectItemCaseSensitive( first, "rssi" );
      TEST_ASSERT_NOT_NULL_MESSAGE( rssi, "rssi field exists" );
      TEST_ASSERT_TRUE_MESSAGE( cJSON_IsNumber( rssi ), "rssi field is a number" );
      TEST_ASSERT_EQUAL_MESSAGE( -45, rssi->valueint, "first AP rssi is -45" );

      cJSON* auth = cJSON_GetObjectItemCaseSensitive( first, "auth" );
      TEST_ASSERT_NOT_NULL_MESSAGE( auth, "auth field exists" );
      TEST_ASSERT_TRUE_MESSAGE( cJSON_IsNumber( auth ), "auth field is a number" );
      TEST_ASSERT_EQUAL_MESSAGE( 3, auth->valueint, "first AP auth is 3" );
    }

    cJSON_Delete( root );
  }
}

/* ============================================================================
 * Test 5: AP list to JSON — empty list
 * ========================================================================== */
static void test_ap_list_to_json_empty( void )
{
  wifi_mgmt_ap_list_t list;
  memset( &list, 0, sizeof( list ) );

  char buf[256];
  osal_status_t st = wifi_utils_ap_list_to_json( &list, buf, sizeof( buf ) );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_SUCCESS, st, "returns SUCCESS" );

  cJSON* root = cJSON_Parse( buf );
  TEST_ASSERT_NOT_NULL_MESSAGE( root, "output is valid JSON" );
  TEST_ASSERT_TRUE_MESSAGE( cJSON_IsArray( root ), "root is array" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, cJSON_GetArraySize( root ), "array is empty" );
  cJSON_Delete( root );
}

/* ============================================================================
 * Test 6: AP list to JSON — buffer too small
 * ========================================================================== */
static void test_ap_list_to_json_small_buf( void )
{
  wifi_mgmt_ap_list_t list;
  memset( &list, 0, sizeof( list ) );
  list.count = 1;
  strncpy( list.items[0].ssid, "SomeNetwork", MAX_SSID_SIZE );

  char buf[5];
  osal_status_t st = wifi_utils_ap_list_to_json( &list, buf, sizeof( buf ) );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_ERR_OUTPUT_TOO_LARGE, st, "returns OUTPUT_TOO_LARGE" );
}

/* ============================================================================
 * Test 7: AP list to JSON — null pointers
 * ========================================================================== */
static void test_ap_list_to_json_null( void )
{
  char buf[256];
  wifi_mgmt_ap_list_t list = { 0 };

  TEST_ASSERT_EQUAL_MESSAGE( OSAL_INVALID_POINTER,
                             wifi_utils_ap_list_to_json( NULL, buf, sizeof( buf ) ),
                             "NULL list" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_INVALID_POINTER,
                             wifi_utils_ap_list_to_json( &list, NULL, sizeof( buf ) ),
                             "NULL buffer" );
  TEST_ASSERT_EQUAL_MESSAGE( OSAL_INVALID_POINTER,
                             wifi_utils_ap_list_to_json( &list, buf, 0 ),
                             "zero size" );
}

/* ============================================================================
 * Runner
 * ========================================================================== */

void wifi_utils_tests_run( void )
{
  RUN_TEST( test_ip_info_to_json_valid );
  RUN_TEST( test_ip_info_to_json_small_buf );
  RUN_TEST( test_ip_info_to_json_null );
  RUN_TEST( test_ap_list_to_json_valid );
  RUN_TEST( test_ap_list_to_json_empty );
  RUN_TEST( test_ap_list_to_json_small_buf );
  RUN_TEST( test_ap_list_to_json_null );
}

#ifndef OSAL_TESTS_AGGREGATE

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  wifi_utils_tests_run();

#ifndef ESP_PLATFORM
  return 0;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */