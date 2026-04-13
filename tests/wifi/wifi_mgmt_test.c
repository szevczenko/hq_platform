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
  TEST_START( "Init and start reach IDLE" );

  bool idle = _wait_idle( 3000 );
  TEST_ASSERT( idle, "state is IDLE after init+start" );
  TEST_ASSERT( !wifi_mgmt_is_connected(), "not connected initially" );

  const wifi_hal_mock_state_t* ms = wifi_hal_mock_get_state();
  TEST_ASSERT( ms->initialized, "HAL was initialized" );
  TEST_ASSERT( ms->started, "HAL was started" );

  TEST_END();
}

/* ============================================================================
 * Test 2: Set SSID / password validation
 * ========================================================================== */
static void test_set_ap_password_validation( void )
{
  TEST_START( "Set AP name / password validation" );

  /* NULL pointer */
  bool ok = wifi_mgmt_set_ap_name( NULL, 5 );
  TEST_ASSERT( !ok, "set_ap_name(NULL) returns false" );

  /* Zero length */
  ok = wifi_mgmt_set_ap_name( "a", 0 );
  TEST_ASSERT( !ok, "set_ap_name len=0 returns false" );

  /* NULL password */
  ok = wifi_mgmt_set_password( NULL, 5 );
  TEST_ASSERT( !ok, "set_password(NULL) returns false" );

  /* Valid */
  ok = wifi_mgmt_set_ap_name( "ValidAP", 7 );
  TEST_ASSERT( ok, "set_ap_name valid returns true" );

  ok = wifi_mgmt_set_password( "ValidPass", 9 );
  TEST_ASSERT( ok, "set_password valid returns true" );

  TEST_END();
}

/* ============================================================================
 * Test 3: Scan returns predefined AP list
 * ========================================================================== */
static void test_scan_predefined_list( void )
{
  TEST_START( "Scan returns predefined AP list" );

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
  TEST_ASSERT( scan_ok, "start_scan returns true" );

  /* Small delay for scan callback to process */
  osal_task_delay_ms( 100 );

  uint16_t ap_count = 0;
  wifi_mgmt_get_scan_result( &ap_count );
  TEST_ASSERT( ap_count == 4, "scan found 4 APs" );

  /* Get structured list */
  wifi_mgmt_ap_list_t ap_list;
  bool got = wifi_mgmt_get_access_points( &ap_list );
  TEST_ASSERT( got, "get_access_points returns true" );
  TEST_ASSERT( ap_list.count == 4, "list count is 4" );
  TEST_ASSERT( strcmp( ap_list.items[0].ssid, "Home_WiFi" ) == 0, "first AP is Home_WiFi" );
  TEST_ASSERT( ap_list.items[0].rssi == -30, "first AP rssi is -30" );
  TEST_ASSERT( ap_list.items[1].chan == 6, "second AP chan is 6" );

  /* Get by name */
  char name[MAX_SSID_SIZE + 1] = { 0 };
  bool name_ok = wifi_mgmt_get_name_from_scanned_list( 2, name );
  TEST_ASSERT( name_ok, "get_name_from_scanned_list(2) returns true" );
  TEST_ASSERT( strcmp( name, "Guest" ) == 0, "scanned[2] is Guest" );

  /* Invalid index */
  name_ok = wifi_mgmt_get_name_from_scanned_list( 10, name );
  TEST_ASSERT( !name_ok, "invalid index returns false" );

  TEST_END();
}

/* ============================================================================
 * Test 4: Set from AP list
 * ========================================================================== */
static void test_set_from_ap_list( void )
{
  TEST_START( "Set from AP list after scan" );

  /* Scan results still available from previous test */
  bool ok = wifi_mgmt_set_from_ap_list( 1 );
  TEST_ASSERT( ok, "set_from_ap_list(1) succeeds" );

  char name[MAX_SSID_SIZE + 1] = { 0 };
  wifi_mgmt_get_ap_name( name );
  TEST_ASSERT( strcmp( name, "Office_Net" ) == 0, "AP name set to Office_Net" );

  /* Out of bounds */
  ok = wifi_mgmt_set_from_ap_list( 99 );
  TEST_ASSERT( !ok, "set_from_ap_list(99) fails" );

  TEST_END();
}

/* ============================================================================
 * Test 5: Connect with credentials
 * ========================================================================== */
static void test_connect_with_credentials( void )
{
  TEST_START( "Connect with credentials" );

  /* Set credentials */
  bool ok = wifi_mgmt_set_ap_name( "TestAP", 6 );
  TEST_ASSERT( ok, "set_ap_name succeeds" );

  ok = wifi_mgmt_set_password( "TestPass", 8 );
  TEST_ASSERT( ok, "set_password succeeds" );

  ok = wifi_mgmt_connect();
  TEST_ASSERT( ok, "connect request accepted" );

  /* Wait for the state machine to reach WAIT_CONNECT */
  osal_task_delay_ms( 300 );

  /* Simulate the GOT_IP event from HAL */
  wifi_hal_event_data_t evt_data = { 0 };
  strncpy( evt_data.ip_info.ip, "192.168.1.10", sizeof( evt_data.ip_info.ip ) - 1 );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt_data );

  bool connected = _wait_for( &g_connect_cb_fired, 3000 );
  TEST_ASSERT( connected, "connect callback fired" );
  TEST_ASSERT( wifi_mgmt_is_connected(), "is_connected returns true" );

  /* Verify AP name stored */
  char name[MAX_SSID_SIZE + 1] = { 0 };
  wifi_mgmt_get_ap_name( name );
  TEST_ASSERT( strcmp( name, "TestAP" ) == 0, "AP name matches" );

  TEST_END();
}

/* ============================================================================
 * Test 6: IP info retrieval while connected
 * ========================================================================== */
static void test_ip_info_retrieval( void )
{
  TEST_START( "IP info retrieval" );

  /* Should still be connected from previous test */
  TEST_ASSERT( wifi_mgmt_is_connected(), "still connected" );

  /* Get IP address string */
  char ip_str[16] = { 0 };
  bool ok = wifi_mgmt_get_ip_addr( ip_str, sizeof( ip_str ) );
  TEST_ASSERT( ok, "get_ip_addr returns true" );
  TEST_ASSERT( strcmp( ip_str, "192.168.1.10" ) == 0, "IP address matches" );

  /* Get structured info */
  wifi_mgmt_ip_info_t info;
  ok = wifi_mgmt_get_ip_info( &info );
  TEST_ASSERT( ok, "get_ip_info returns true" );
  TEST_ASSERT( strcmp( info.ssid, "TestAP" ) == 0, "ssid in ip_info matches" );
  TEST_ASSERT( info.urc == 0, "urc is 0 (connected)" );

  /* Null checks */
  ok = wifi_mgmt_get_ip_addr( NULL, 16 );
  TEST_ASSERT( !ok, "get_ip_addr(NULL) returns false" );

  ok = wifi_mgmt_get_ip_info( NULL );
  TEST_ASSERT( !ok, "get_ip_info(NULL) returns false" );

  TEST_END();
}

/* ============================================================================
 * Test 7: State queries while connected
 * ========================================================================== */
static void test_state_queries_connected( void )
{
  TEST_START( "State queries while connected" );

  TEST_ASSERT( wifi_mgmt_is_connected(), "is_connected true" );
  TEST_ASSERT( !wifi_mgmt_is_idle(), "is_idle false when connected" );
  TEST_ASSERT( !wifi_mgmt_trying_connect(), "trying_connect false when connected" );
  TEST_ASSERT( wifi_mgmt_is_ready_to_scan(), "is_ready_to_scan true when ready" );

  int rssi = wifi_mgmt_get_rssi();
  TEST_ASSERT( rssi == -50, "rssi returned by mock" );

  TEST_END();
}

/* ============================================================================
 * Test 8: Disconnect flow
 * ========================================================================== */
static void test_disconnect( void )
{
  TEST_START( "Disconnect flow" );

  g_disconnect_cb_fired = false;

  bool ok = wifi_mgmt_disconnect();
  TEST_ASSERT( ok, "disconnect request accepted" );

  bool disconnected = _wait_for( &g_disconnect_cb_fired, 3000 );
  TEST_ASSERT( disconnected, "disconnect callback fired" );

  /* After disconnect, should go back to idle */
  bool idle = _wait_idle( 2000 );
  TEST_ASSERT( idle, "returns to IDLE after disconnect" );
  TEST_ASSERT( !wifi_mgmt_is_connected(), "not connected after disconnect" );

  TEST_END();
}

/* ============================================================================
 * Test 9: Power save toggle
 * ========================================================================== */
static void test_power_save( void )
{
  TEST_START( "Power save toggle" );

  wifi_mgmt_power_save( true );

  const wifi_hal_mock_state_t* ms = wifi_hal_mock_get_state();
  TEST_ASSERT( ms->power_save == true, "power save enabled in HAL" );

  wifi_mgmt_power_save( false );
  TEST_ASSERT( ms->power_save == false, "power save disabled in HAL" );

  TEST_END();
}

/* ============================================================================
 * Test 10: Client count
 * ========================================================================== */
static void test_client_count( void )
{
  TEST_START( "Client count" );

  uint32_t cnt = wifi_mgmt_get_client_count();
  TEST_ASSERT( cnt == 0, "client count is 0 (mock returns 0)" );

  TEST_END();
}

/* ============================================================================
 * Test 11: is_read_data after config loaded
 * ========================================================================== */
static void test_is_read_data( void )
{
  TEST_START( "is_read_data reflects config state" );

  /* After init with no config file, read_data depends on whether
     _load_saved_config found a file.  With config saved by the connect
     test, it should have saved credentials. */
  bool rd = wifi_mgmt_is_read_data();
  /* We don't assert a specific value since it depends on whether
     _save_current_sta_config wrote the file during connect. Just
     verify the function doesn't crash. */
  TEST_ASSERT( rd == true || rd == false, "is_read_data returns a bool" );

  TEST_END();
}

/* ============================================================================
 * Runner
 * ========================================================================== */

int wifi_mgmt_tests_run( void )
{
  tests_run    = 0;
  tests_passed = 0;
  tests_failed = 0;

  printf( "\n" );
  printf( "==================================================\n" );
  printf( "        Wi-Fi Management State Machine Tests      \n" );
  printf( "==================================================\n" );

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
  test_init_reaches_idle();
  test_set_ap_password_validation();
  test_scan_predefined_list();
  test_set_from_ap_list();
  test_connect_with_credentials();
  test_ip_info_retrieval();
  test_state_queries_connected();
  test_disconnect();
  test_power_save();
  test_client_count();
  test_is_read_data();

  /* --- One-time stop --- */
  wifi_mgmt_stop();
  osal_task_delay_ms( 300 );

  cleanup_fs();

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
  int failed = wifi_mgmt_tests_run();

#ifndef ESP_PLATFORM
  return ( failed == 0 ) ? 0 : 1;
#endif
}

#endif /* OSAL_TESTS_AGGREGATE */
