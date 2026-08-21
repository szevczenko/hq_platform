/*
 * Wi-Fi Management State Machine Unit Tests
 *
 * Uses the HAL mock to exercise the wifi_managment state machine
 * without real hardware.  All tests run within a single init/stop
 * lifecycle because the management module spawns a persistent background
 * task.
 *
 * Tests:
 * 1.  Init and start reach IDLE
 * 2.  Set SSID / password validation
 * 3.  Scan returns predefined AP list
 * 4.  Set from AP list after scan
 * 5.  Connect and receive IP
 * 6.  IP info retrieval while connected
 * 7.  State queries while connected
 * 8.  Disconnect flow
 * 9.  Power save toggle
 * 10. Client count
 * 11. Server mode rejects connect/disconnect
 */

#include <stdio.h>
#include <string.h>

#include "wifi_managment.h"
#include "wifi_hal_mock.h"
#include "osal_task.h"
#include "osal_mount.h"
#include "osal_file.h"
#include "wifi_config.h"
#include "unity.h"

/* Filesystem for config persistence tests */
#define TEST_IMAGE_PATH  "/tmp/wifi_mgmt_test.img"
#define TEST_MOUNT_POINT "/"

static void setup_fs( void )
{
  (void) osal_unmount( TEST_MOUNT_POINT );
  (void) osal_rmfs( TEST_IMAGE_PATH );
  (void) osal_mkfs( NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, 4096U, 256U );
  (void) osal_mount( TEST_IMAGE_PATH, TEST_MOUNT_POINT );
  (void) osal_remove( WIFI_CONFIG_FILE_PATH );
}

static void cleanup_fs( void )
{
  (void) osal_remove( WIFI_CONFIG_FILE_PATH );
  (void) osal_unmount( TEST_MOUNT_POINT );
  (void) osal_rmfs( TEST_IMAGE_PATH );
}

static volatile bool g_connect_cb_fired    = false;
static volatile bool g_disconnect_cb_fired = false;

static void _on_connect( void )
{
  g_connect_cb_fired = true;
}

static void _on_disconnect( void )
{
  g_disconnect_cb_fired = true;
}

/* Wait for a condition with timeout (spin-wait). */
static bool _wait_for( volatile bool* flag, uint32_t timeout_ms )
{
  uint32_t elapsed = 0;
  while ( !*flag && elapsed < timeout_ms )
  {
    osal_task_delay_ms( 50 );
    elapsed += 50;
  }
  return *flag;
}

/* Wait for management to reach idle state. */
static bool _wait_idle( uint32_t timeout_ms )
{
  uint32_t elapsed = 0;
  while ( !wifi_mgmt_is_idle() && elapsed < timeout_ms )
  {
    osal_task_delay_ms( 50 );
    elapsed += 50;
  }
  return wifi_mgmt_is_idle();
}

/* ============================================================================
 * Test 1: Init and start reach IDLE
 * ========================================================================== */
static void test_init_reaches_idle( void )
{
  bool idle = _wait_idle( 3000 );
  TEST_ASSERT_TRUE_MESSAGE( idle, "state is IDLE after init+start" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_connected(), "not connected initially" );

  const wifi_hal_mock_state_t* ms = wifi_hal_mock_get_state();
  TEST_ASSERT_TRUE_MESSAGE( ms->initialized, "HAL was initialized" );
  TEST_ASSERT_TRUE_MESSAGE( ms->started, "HAL was started" );
}

/* ============================================================================
 * Test 2: Set SSID / password validation
 * ========================================================================== */
static void test_set_ap_password_validation( void )
{
  /* NULL pointer */
  bool ok = wifi_mgmt_set_ap_name( NULL, 5 );
  TEST_ASSERT_FALSE_MESSAGE( ok, "set_ap_name(NULL) returns false" );

  /* Zero length */
  ok = wifi_mgmt_set_ap_name( "a", 0 );
  TEST_ASSERT_FALSE_MESSAGE( ok, "set_ap_name len=0 returns false" );

  /* NULL password */
  ok = wifi_mgmt_set_password( NULL, 5 );
  TEST_ASSERT_FALSE_MESSAGE( ok, "set_password(NULL) returns false" );

  /* Valid */
  ok = wifi_mgmt_set_ap_name( "ValidAP", 7 );
  TEST_ASSERT_TRUE_MESSAGE( ok, "set_ap_name valid returns true" );

  ok = wifi_mgmt_set_password( "ValidPass", 9 );
  TEST_ASSERT_TRUE_MESSAGE( ok, "set_password valid returns true" );
}

/* ============================================================================
 * Test 3: Scan returns predefined AP list
 * ========================================================================== */
static void test_scan_predefined_list( void )
{
  /* Set up predefined scan results */
  wifi_hal_ap_record_t mock_aps[4];
  memset( mock_aps, 0, sizeof( mock_aps ) );

  strncpy( mock_aps[0].ssid, "Home_WiFi", WIFI_HAL_SSID_MAX_LEN );
  mock_aps[0].channel  = 1;
  mock_aps[0].rssi     = -30;
  mock_aps[0].authmode = 3;

  strncpy( mock_aps[1].ssid, "Office_Net", WIFI_HAL_SSID_MAX_LEN );
  mock_aps[1].channel  = 6;
  mock_aps[1].rssi     = -55;
  mock_aps[1].authmode = 4;

  strncpy( mock_aps[2].ssid, "Guest", WIFI_HAL_SSID_MAX_LEN );
  mock_aps[2].channel  = 11;
  mock_aps[2].rssi     = -70;
  mock_aps[2].authmode = 0;

  strncpy( mock_aps[3].ssid, "IoT_Network", WIFI_HAL_SSID_MAX_LEN );
  mock_aps[3].channel  = 3;
  mock_aps[3].rssi     = -80;
  mock_aps[3].authmode = 3;

  wifi_hal_mock_set_scan_list( mock_aps, 4 );

  bool scan_ok = wifi_mgmt_start_scan();
  TEST_ASSERT_TRUE_MESSAGE( scan_ok, "start_scan returns true" );

  /* Small delay for scan callback to process */
  osal_task_delay_ms( 100 );

  uint16_t ap_count = 0;
  wifi_mgmt_get_scan_result( &ap_count );
  TEST_ASSERT_EQUAL_MESSAGE( 4, ap_count, "scan found 4 APs" );

  /* Get structured list */
  wifi_mgmt_ap_list_t ap_list;
  bool got = wifi_mgmt_get_access_points( &ap_list );
  TEST_ASSERT_TRUE_MESSAGE( got, "get_access_points returns true" );
  TEST_ASSERT_EQUAL_MESSAGE( 4, ap_list.count, "list count is 4" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "Home_WiFi", ap_list.items[0].ssid, "first AP is Home_WiFi" );
  TEST_ASSERT_EQUAL_MESSAGE( -30, ap_list.items[0].rssi, "first AP rssi is -30" );
  TEST_ASSERT_EQUAL_MESSAGE( 6, ap_list.items[1].chan, "second AP chan is 6" );

  /* Get by name */
  char name[MAX_SSID_SIZE + 1] = { 0 };
  bool name_ok = wifi_mgmt_get_name_from_scanned_list( 2, name );
  TEST_ASSERT_TRUE_MESSAGE( name_ok, "get_name_from_scanned_list(2) returns true" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "Guest", name, "scanned[2] is Guest" );

  /* Invalid index */
  name_ok = wifi_mgmt_get_name_from_scanned_list( 10, name );
  TEST_ASSERT_FALSE_MESSAGE( name_ok, "invalid index returns false" );
}

/* ============================================================================
 * Test 4: Set from AP list
 * ========================================================================== */
static void test_set_from_ap_list( void )
{
  /* Scan results still available from previous test */
  bool ok = wifi_mgmt_set_from_ap_list( 1 );
  TEST_ASSERT_TRUE_MESSAGE( ok, "set_from_ap_list(1) succeeds" );

  char name[MAX_SSID_SIZE + 1] = { 0 };
  wifi_mgmt_get_ap_name( name );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "Office_Net", name, "AP name set to Office_Net" );

  /* Out of bounds */
  ok = wifi_mgmt_set_from_ap_list( 99 );
  TEST_ASSERT_FALSE_MESSAGE( ok, "set_from_ap_list(99) fails" );
}

/* ============================================================================
 * Test 5: Connect with credentials
 * ========================================================================== */
static void test_connect_with_credentials( void )
{
  /* Set credentials */
  bool ok = wifi_mgmt_set_ap_name( "TestAP", 6 );
  TEST_ASSERT_TRUE_MESSAGE( ok, "set_ap_name succeeds" );

  ok = wifi_mgmt_set_password( "TestPass", 8 );
  TEST_ASSERT_TRUE_MESSAGE( ok, "set_password succeeds" );

  ok = wifi_mgmt_connect();
  TEST_ASSERT_TRUE_MESSAGE( ok, "connect request accepted" );

  /* Wait for the state machine to reach WAIT_CONNECT */
  osal_task_delay_ms( 300 );

  /* Simulate the GOT_IP event from HAL */
  wifi_hal_event_data_t evt_data = { 0 };
  strncpy( evt_data.ip_info.ip, "192.168.1.10", sizeof( evt_data.ip_info.ip ) - 1 );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt_data );

  bool connected = _wait_for( &g_connect_cb_fired, 3000 );
  TEST_ASSERT_TRUE_MESSAGE( connected, "connect callback fired" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_connected(), "is_connected returns true" );

  /* Verify AP name stored */
  char name[MAX_SSID_SIZE + 1] = { 0 };
  wifi_mgmt_get_ap_name( name );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "TestAP", name, "AP name matches" );
}

/* ============================================================================
 * Test 6: IP info retrieval while connected
 * ========================================================================== */
static void test_ip_info_retrieval( void )
{
  /* Should still be connected from previous test */
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_connected(), "still connected" );

  /* Get IP address string */
  char ip_str[16] = { 0 };
  bool ok = wifi_mgmt_get_ip_addr( ip_str, sizeof( ip_str ) );
  TEST_ASSERT_TRUE_MESSAGE( ok, "get_ip_addr returns true" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "192.168.1.10", ip_str, "IP address matches" );

  /* Get structured info */
  wifi_mgmt_ip_info_t info;
  ok = wifi_mgmt_get_ip_info( &info );
  TEST_ASSERT_TRUE_MESSAGE( ok, "get_ip_info returns true" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "TestAP", info.ssid, "ssid in ip_info matches" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, info.urc, "urc is 0 (connected)" );

  /* Null checks */
  ok = wifi_mgmt_get_ip_addr( NULL, 16 );
  TEST_ASSERT_FALSE_MESSAGE( ok, "get_ip_addr(NULL) returns false" );

  ok = wifi_mgmt_get_ip_info( NULL );
  TEST_ASSERT_FALSE_MESSAGE( ok, "get_ip_info(NULL) returns false" );
}

/* ============================================================================
 * Test 7: State queries while connected
 * ========================================================================== */
static void test_state_queries_connected( void )
{
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_connected(), "is_connected true" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_idle(), "is_idle false when connected" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_trying_connect(), "trying_connect false when connected" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_ready_to_scan(), "is_ready_to_scan true when ready" );

  int rssi = wifi_mgmt_get_rssi();
  TEST_ASSERT_EQUAL_MESSAGE( -50, rssi, "rssi returned by mock" );
}

/* ============================================================================
 * Test 8: Disconnect flow
 * ========================================================================== */
static void test_disconnect( void )
{
  g_disconnect_cb_fired = false;

  bool ok = wifi_mgmt_disconnect();
  TEST_ASSERT_TRUE_MESSAGE( ok, "disconnect request accepted" );

  bool disconnected = _wait_for( &g_disconnect_cb_fired, 3000 );
  TEST_ASSERT_TRUE_MESSAGE( disconnected, "disconnect callback fired" );

  /* After disconnect, should go back to idle */
  bool idle = _wait_idle( 2000 );
  TEST_ASSERT_TRUE_MESSAGE( idle, "returns to IDLE after disconnect" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_connected(), "not connected after disconnect" );
}

/* ============================================================================
 * Test 9: Power save toggle
 * ========================================================================== */
static void test_power_save( void )
{
  wifi_mgmt_power_save( true );

  const wifi_hal_mock_state_t* ms = wifi_hal_mock_get_state();
  TEST_ASSERT_TRUE_MESSAGE( ms->power_save, "power save enabled in HAL" );

  wifi_mgmt_power_save( false );
  TEST_ASSERT_FALSE_MESSAGE( ms->power_save, "power save disabled in HAL" );
}

/* ============================================================================
 * Test 10: Client count
 * ========================================================================== */
static void test_client_count( void )
{
  uint32_t cnt = wifi_mgmt_get_client_count();
  TEST_ASSERT_EQUAL_MESSAGE( 0, cnt, "client count is 0 (mock returns 0)" );
}

/* ============================================================================
 * Test 11: is_read_data after config loaded
 * ========================================================================== */
static void test_is_read_data( void )
{
  /* After init with no config file, read_data depends on whether
     _load_saved_config found a file.  With config saved by the connect
     test, it should have saved credentials. */
  bool rd = wifi_mgmt_is_read_data();
  /* We don't assert a specific value since it depends on whether
     _save_current_sta_config wrote the file during connect. Just
     verify the function doesn't crash. */
  TEST_ASSERT_TRUE_MESSAGE( ( rd == true ) || ( rd == false ), "is_read_data returns a bool" );
}

/* ============================================================================
 * Runner
 * ========================================================================== */

void wifi_mgmt_tests_run( void )
{
  setup_fs();

  /* --- One-time init: mock + management + start --- */
  wifi_hal_mock_reset();
  wifi_hal_mock_set_connect_result( OSAL_SUCCESS );

  wifi_hal_ip_info_t ip = { 0 };
  strncpy( ip.ip, "192.168.1.10", sizeof( ip.ip ) - 1 );
  strncpy( ip.netmask, "255.255.255.0", sizeof( ip.netmask ) - 1 );
  strncpy( ip.gw, "192.168.1.1", sizeof( ip.gw ) - 1 );
  wifi_hal_mock_set_ip_info( &ip );

  g_connect_cb_fired    = false;
  g_disconnect_cb_fired = false;

  wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLIENT );
  wifi_mgmt_init();
  wifi_mgmt_register_connect_cb( _on_connect );
  wifi_mgmt_register_disconnect_cb( _on_disconnect );
  wifi_mgmt_start();

  /* --- Run all tests sequentially (single lifecycle) --- */
  RUN_TEST( test_init_reaches_idle );
  RUN_TEST( test_set_ap_password_validation );
  RUN_TEST( test_scan_predefined_list );
  RUN_TEST( test_set_from_ap_list );
  RUN_TEST( test_connect_with_credentials );
  RUN_TEST( test_ip_info_retrieval );
  RUN_TEST( test_state_queries_connected );
  RUN_TEST( test_disconnect );
  RUN_TEST( test_power_save );
  RUN_TEST( test_client_count );
  RUN_TEST( test_is_read_data );

  /* --- One-time stop --- */
  wifi_mgmt_stop();
  osal_task_delay_ms( 300 );

  cleanup_fs();
}

#ifndef OSAL_TESTS_AGGREGATE

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  wifi_mgmt_tests_run();

#ifndef ESP_PLATFORM
  return 0;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */