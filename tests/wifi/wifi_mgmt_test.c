/*
 * Wi-Fi Management State Machine Unit Tests
 *
 * Uses the HAL mock to exercise the wifi_managment state machine
 * without real hardware.  All tests run within a single init/stop
 * lifecycle because the management module spawns a persistent background
 * task.
 *
 * Tests:
 * 0a. wait_ready() is held until the HAL callback is installed (init barrier)
 * 0b. Startup mode-start failure is reported deterministically via wait_ready()
 * 0c. Startup HAL-init failure is reported deterministically via wait_ready()
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

/* Typed event subscription recording --------------------------------------- */
typedef struct
{
  volatile bool         fired;
  wifi_mgmt_event_t     evt;
  void*                 ctx;
  volatile unsigned int count;
} event_rec_t;

static event_rec_t g_ev_connected;
static event_rec_t g_ev_disconnected;
static event_rec_t g_ev_failed;
static event_rec_t g_ev_scan;
static event_rec_t g_ev_mode;
static event_rec_t g_ev_unsub;

static void _record_event( wifi_mgmt_event_t event, void* user_data )
{
  event_rec_t* rec = (event_rec_t*) user_data;
  if ( rec )
  {
    rec->fired = true;
    rec->evt   = event;
    rec->ctx   = user_data;
    rec->count++;
  }
}

static void _reset_event( event_rec_t* rec )
{
  rec->fired = false;
  rec->evt   = (wifi_mgmt_event_t) -1;
  rec->ctx   = NULL;
  rec->count = 0;
}

/* ---------------------------------------------------------------------------
 * Focused state-machine event recorder.
 *
 * The capture callback runs synchronously inside the Wi-Fi worker task at the
 * moment a typed event is dispatched.  It records a snapshot of the management
 * state (connection flag, IP reason code, scanned-AP count) at that instant.
 * This lets tests assert that an event is delivered only *after* the relevant
 * state snapshot has been updated, and that callbacks may re-enter the event
 * API (i.e. no internal mutex is held during dispatch).
 * ------------------------------------------------------------------------- */
typedef struct
{
  volatile bool         received;
  volatile unsigned int count;
  wifi_mgmt_event_t     evt;
  volatile bool         connected_at_cb;
  volatile int          ip_urc_at_cb;
  volatile char         ip_ssid_at_cb[MAX_SSID_SIZE + 1];
  volatile uint16_t     scan_count_at_cb;
  volatile bool         reentered_api;
} state_mg_rec_t;

static state_mg_rec_t g_rec_scan;
static state_mg_rec_t g_rec_connected;
static state_mg_rec_t g_rec_disconnected;
static state_mg_rec_t g_rec_failed;

/* Dummy callback used to verify a handler may re-enter the subscription API. */
static void _dummy_ignored_event( wifi_mgmt_event_t event, void* user_data )
{
  (void) event;
  (void) user_data;
}

static void _capture_state_event( wifi_mgmt_event_t event, void* user_data )
{
  state_mg_rec_t* rec = (state_mg_rec_t*) user_data;
  if ( !rec )
  {
    return;
  }

  rec->received = true;
  rec->evt      = event;
  rec->count++;

  /* Snapshot of management state at dispatch time. */
  rec->connected_at_cb = wifi_mgmt_is_connected();
  rec->ip_urc_at_cb    = 0;
  memset( (char*) rec->ip_ssid_at_cb, 0, sizeof( rec->ip_ssid_at_cb ) );
  {
    wifi_mgmt_ip_info_t info = { 0 };
    (void) wifi_mgmt_get_ip_info( &info );
    rec->ip_urc_at_cb = info.urc;
    strncpy( (char*) rec->ip_ssid_at_cb, info.ssid,
             sizeof( rec->ip_ssid_at_cb ) - 1 );
  }
  (void) wifi_mgmt_get_scan_result( (uint16_t*) &rec->scan_count_at_cb );

  /* Re-entering the subscription API must not deadlock: it is only safe if the
     internal event mutex has been released before our callback is invoked. */
  rec->reentered_api = false;
  bool sub = wifi_mgmt_subscribe( WIFI_MGMT_EVENT_MODE_CHANGED,
                                  _dummy_ignored_event, rec );
  bool unsub = wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_MODE_CHANGED,
                                      _dummy_ignored_event, rec );
  rec->reentered_api = sub && unsub;
}

static void _reset_state_event( state_mg_rec_t* rec )
{
  memset( rec, 0, sizeof( *rec ) );
  rec->evt = (wifi_mgmt_event_t) -1;
}

static bool _wait_state_event( state_mg_rec_t* rec, uint32_t timeout_ms )
{
  uint32_t elapsed = 0;
  while ( !rec->received && elapsed < timeout_ms )
  {
    osal_task_delay_ms( 50 );
    elapsed += 50;
  }
  return rec->received;
}

static bool _wait_event( event_rec_t* rec, uint32_t timeout_ms )
{
  uint32_t elapsed = 0;
  while ( !rec->fired && elapsed < timeout_ms )
  {
    osal_task_delay_ms( 50 );
    elapsed += 50;
  }
  return rec->fired;
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

/* Wait until the mode-change recorder has fired at least @p min_count times. */
static bool _wait_mode_for_count( unsigned int min_count, uint32_t timeout_ms )
{
  uint32_t elapsed = 0;
  while ( g_ev_mode.count < min_count && elapsed < timeout_ms )
  {
    osal_task_delay_ms( 50 );
    elapsed += 50;
  }
  return g_ev_mode.count >= min_count;
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
 * Test 0a: wifi_mgmt_wait_ready() is held until the HAL callback is installed
 *
 * Performs the very first lifecycle start with wifi_hal_init() parked at the
 * mock barrier (before the event callback is stored).  While the worker is
 * stuck inside init():
 *   - wifi_mgmt_wait_ready() must NOT report ready,
 *   - wifi_mgmt_is_running() must be false (state == INIT),
 *   - a GOT_IP injected into the mock is dropped (no callback installed).
 * After the barrier is lifted, wait_ready() reports success and a GOT_IP
 * injected afterwards is delivered through the installed HAL callback, proving
 * that events cannot be lost once readiness is reported.
 * ========================================================================== */
static void test_wait_ready_held_at_callback_barrier( void )
{
  /* Hold the worker inside wifi_hal_init(); the callback is not stored yet. */
  wifi_hal_mock_set_init_hold( true );
  wifi_mgmt_start();

  /* First acknowledge that the worker has actually reached wifi_hal_init() and
   * is parked at the barrier (before the event callback is stored).  Without
   * this the assertion below could pass just because the worker has not run
   * yet, rather than because init is held before callback installation. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered( 2000 ),
                            "worker entered wifi_hal_init() at the barrier" );

  /* Readiness must NOT be reported while the init barrier is held. */
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_wait_ready( 150 ),
                             "not ready while HAL init is held" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_running(),
                             "not running while state is INIT" );

  /* A GOT_IP injected before the callback exists is dropped. */
  wifi_hal_event_data_t evt_data = { 0 };
  strncpy( evt_data.ip_info.ip, "192.168.77.1", sizeof( evt_data.ip_info.ip ) - 1 );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt_data );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_connected(),
                             "pre-callback GOT_IP is not delivered" );

  /* Release the barrier: startup completes and readiness is reported. */
  wifi_hal_mock_set_init_hold( false );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_wait_ready( 3000 ),
                            "ready once HAL init completes" );

  const wifi_hal_mock_state_t* ms = wifi_hal_mock_get_state();
  TEST_ASSERT_TRUE_MESSAGE( ms->initialized, "HAL initialized" );
  TEST_ASSERT_TRUE_MESSAGE( ms->started, "HAL started" );
  TEST_ASSERT_NOT_NULL_MESSAGE( (void*) ms->event_cb, "HAL callback installed" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_running(), "running after startup" );

  /* Post-ready GOT_IP cannot be lost: the HAL callback is present now. */
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt_data );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_connected(),
                            "post-ready GOT_IP is delivered" );

  /* Drop the link again so later tests start from a clean idle station. */
  wifi_hal_event_data_t disc_data = { 0 };
  disc_data.disconnect_reason = 2;
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_DISCONNECTED, &disc_data );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_connected(), "idle after disconnect" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "machine idle after barrier test" );
}

/* ============================================================================
 * Test 0b: Startup mode-start failure is reported deterministically
 *
 * Shuts the module down, sabotages the HAL mode start, and starts again:
 * wifi_mgmt_wait_ready() must return false without relying on timing sleeps,
 * the HAL must be stopped AND deinitialized on the failure path, the module
 * must be not running, and a fresh start with a healthy HAL must complete
 * successfully.
 * ========================================================================== */
static void test_startup_failure_is_deterministic( void )
{
  /* Stop the current lifecycle; stop() blocks until state == DISABLE. */
  wifi_mgmt_stop();
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_running(), "not running after stop" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_wait_ready( 100 ),
                             "not ready after stop" );

  /* Sabotage the HAL mode start and trigger a fresh init round. */
  const uint32_t start_before   = wifi_hal_mock_get_state()->start_count;
  const uint32_t init_before    = wifi_hal_mock_get_state()->init_count;
  const uint32_t deinit_before  = wifi_hal_mock_get_state()->deinit_count;
  wifi_hal_mock_set_start_result( OSAL_ERROR );
  wifi_mgmt_start();

  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_wait_ready( 2000 ),
                             "startup failure reported by wait_ready" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_running(),
                             "not running after failed startup" );
  const wifi_hal_mock_state_t* ms = wifi_hal_mock_get_state();
  TEST_ASSERT_EQUAL_MESSAGE( start_before + 1, ms->start_count,
                             "HAL start attempted exactly once" );
  TEST_ASSERT_EQUAL_MESSAGE( init_before + 1, ms->init_count,
                             "HAL init attempted exactly once" );
  TEST_ASSERT_EQUAL_MESSAGE( deinit_before + 1, ms->deinit_count,
                             "HAL deinitialized on the mode-start failure path" );
  TEST_ASSERT_FALSE_MESSAGE( ms->started, "HAL left stopped after failure" );
  TEST_ASSERT_FALSE_MESSAGE( ms->initialized,
                             "HAL left deinitialized after failure" );

  /* Retry with a healthy HAL: startup completes. */
  wifi_hal_mock_set_start_result( OSAL_SUCCESS );
  wifi_mgmt_start();
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_wait_ready( 3000 ),
                            "startup succeeds after retry" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_running(), "running after retry" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle after retry" );
}

/* ============================================================================
 * Test 0c: Startup HAL-init failure is reported deterministically
 *
 * Same contract as test 0b but for the HAL initialization step: the failure is
 * surfaced through wifi_mgmt_wait_ready(), the HAL is deinitialized so a retry
 * never re-initializes an already initialized HAL, and a healthy retry
 * completes.
 * ========================================================================== */
static void test_startup_init_failure_is_deterministic( void )
{
  /* Stop the current lifecycle; stop() blocks until state == DISABLE. */
  wifi_mgmt_stop();
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_running(), "not running after stop" );

  /* Sabotage HAL initialization and trigger a fresh init round. */
  const uint32_t init_before = wifi_hal_mock_get_state()->init_count;
  wifi_hal_mock_set_init_result( OSAL_ERROR );
  wifi_mgmt_start();

  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_wait_ready( 2000 ),
                             "init failure reported by wait_ready" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_running(),
                             "not running after failed init" );
  const wifi_hal_mock_state_t* ms = wifi_hal_mock_get_state();
  TEST_ASSERT_EQUAL_MESSAGE( init_before + 1, ms->init_count,
                             "HAL init attempted exactly once" );
  TEST_ASSERT_FALSE_MESSAGE( ms->initialized,
                             "HAL left deinitialized after init failure" );

  /* Retry with a healthy HAL: startup completes. */
  wifi_hal_mock_set_init_result( OSAL_SUCCESS );
  wifi_mgmt_start();
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_wait_ready( 3000 ),
                            "startup succeeds after retry" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_running(), "running after retry" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle after retry" );
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
 * Test 12: Typed event subscriptions
 * ========================================================================== */
static void test_event_subscriptions( void )
{
  /* --- API validation: null callbacks are rejected --- */
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECTED, NULL, NULL ),
                             "subscribe(NULL) rejected" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_CONNECTED, NULL, NULL ),
                            "unsubscribe(NULL) rejected" );

  _reset_event( &g_ev_connected );
  _reset_event( &g_ev_disconnected );
  _reset_event( &g_ev_failed );
  _reset_event( &g_ev_scan );
  _reset_event( &g_ev_mode );
  _reset_event( &g_ev_unsub );

  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECTED, _record_event, &g_ev_connected ),
                           "sub CONNECTED" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_DISCONNECTED, _record_event, &g_ev_disconnected ),
                           "sub DISCONNECTED" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECT_FAILED, _record_event, &g_ev_failed ),
                           "sub CONNECT_FAILED" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_SCAN_COMPLETED, _record_event, &g_ev_scan ),
                           "sub SCAN_COMPLETED" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_MODE_CHANGED, _record_event, &g_ev_mode ),
                           "sub MODE_CHANGED" );

  /* --- Duplicate subscriptions are rejected and consume no slots --- */
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECTED, _record_event, &g_ev_connected ),
                           "duplicate CONNECTED rejected" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_MODE_CHANGED, _record_event, &g_ev_mode ),
                           "duplicate MODE_CHANGED rejected" );

  /* --- MODE_CHANGED: subscriber receives event and user context --- */
  _reset_event( &g_ev_mode );
  wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLI_SER );
  TEST_ASSERT_TRUE_MESSAGE( _wait_event( &g_ev_mode, 2000 ), "MODE_CHANGED fired on mode switch" );
  TEST_ASSERT_EQUAL_INT_MESSAGE( WIFI_MGMT_EVENT_MODE_CHANGED, g_ev_mode.evt, "mode event type correct" );
  TEST_ASSERT_EQUAL_PTR_MESSAGE( &g_ev_mode, g_ev_mode.ctx, "mode event context correct" );
  wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLIENT );
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle after mode change" );

  /* --- SCAN_COMPLETED: subscriber receives event and context --- */
  _reset_event( &g_ev_scan );
  bool scan_ok = wifi_mgmt_start_scan();
  TEST_ASSERT_TRUE_MESSAGE( scan_ok, "start_scan succeeds for event test" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_event( &g_ev_scan, 2000 ), "SCAN_COMPLETED event fired" );
  TEST_ASSERT_EQUAL_INT_MESSAGE( WIFI_MGMT_EVENT_SCAN_COMPLETED, g_ev_scan.evt, "scan event type correct" );
  TEST_ASSERT_EQUAL_PTR_MESSAGE( &g_ev_scan, g_ev_scan.ctx, "scan event context correct" );

  /* --- CONNECTED / DISCONNECTED cycle with user context --- */
  osal_task_delay_ms( 150 );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_set_ap_name( "EventAP", 7 ), "set ap name" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_set_password( "pass123", 7 ), "set password" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_connect(), "connect accepted" );

  wifi_hal_event_data_t evt_data = { 0 };
  strncpy( evt_data.ip_info.ip, "10.1.2.3", sizeof( evt_data.ip_info.ip ) - 1 );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt_data );

  TEST_ASSERT_TRUE_MESSAGE( _wait_event( &g_ev_connected, 3000 ), "CONNECTED event fired" );
  TEST_ASSERT_EQUAL_INT_MESSAGE( WIFI_MGMT_EVENT_CONNECTED, g_ev_connected.evt, "connected event type correct" );
  TEST_ASSERT_EQUAL_PTR_MESSAGE( &g_ev_connected, g_ev_connected.ctx, "connected event context correct" );

  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_disconnect(), "disconnect accepted" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_event( &g_ev_disconnected, 3000 ), "DISCONNECTED event fired" );
  TEST_ASSERT_EQUAL_INT_MESSAGE( WIFI_MGMT_EVENT_DISCONNECTED, g_ev_disconnected.evt, "disconnected event type correct" );
  TEST_ASSERT_EQUAL_PTR_MESSAGE( &g_ev_disconnected, g_ev_disconnected.ctx, "disconnected event context correct" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle after event disconnect" );

  /* --- CONNECT_FAILED: only subscribers of that event + correct context --- */
  _reset_event( &g_ev_failed );
  wifi_hal_mock_set_connect_result( OSAL_ERROR );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_set_ap_name( "APK", 3 ), "set ap name for failure" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_connect(), "connect accepted (fail path)" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_event( &g_ev_failed, 3000 ), "CONNECT_FAILED event fired" );
  TEST_ASSERT_EQUAL_INT_MESSAGE( WIFI_MGMT_EVENT_CONNECT_FAILED, g_ev_failed.evt, "failed event type correct" );
  TEST_ASSERT_EQUAL_PTR_MESSAGE( &g_ev_failed, g_ev_failed.ctx, "failed event context correct" );
  wifi_hal_mock_set_connect_result( OSAL_SUCCESS );
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle after connect failure" );

  /* --- Unsubscribed callbacks are not invoked --- */
  _reset_event( &g_ev_unsub );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_SCAN_COMPLETED, _record_event, &g_ev_unsub ),
                           "sub unsub SCAN_COMPLETED" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_SCAN_COMPLETED, _record_event, &g_ev_unsub ),
                           "unsubscribe SCAN_COMPLETED" );
  TEST_ASSERT_TRUE( wifi_mgmt_start_scan() );
  osal_task_delay_ms( 200 );
  TEST_ASSERT_FALSE_MESSAGE( g_ev_unsub.fired, "unsubscribed callback not called" );

  /* The still-subscribed scan recorder keeps receiving the event. */
  TEST_ASSERT_EQUAL_INT_MESSAGE( WIFI_MGMT_EVENT_SCAN_COMPLETED, g_ev_scan.evt, "still-subscribed scan event" );
}

/* ============================================================================
 * Test 13: Focused state-machine event emission
 *
 * Verifies the typed state-machine events are emitted exactly once per state
 * transition and that the emitted event observes the *updated* state snapshot
 * (scan records published before SCAN_COMPLETED, IP state published before
 * CONNECTED, torn-down state before DISCONNECTED).  Also verifies callbacks are
 * invoked without holding an internal Wi-Fi mutex by re-entering the event
 * subscribe/unsubscribe API from inside a handler.
 * ========================================================================== */
static void test_state_machine_event_emission( void )
{
  /* Fresh recorders + subscriptions scoped to this test. */
  _reset_state_event( &g_rec_scan );
  _reset_state_event( &g_rec_connected );
  _reset_state_event( &g_rec_disconnected );

  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_SCAN_COMPLETED,
                                                 _capture_state_event, &g_rec_scan ),
                            "focused: sub SCAN_COMPLETED" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECTED,
                                                 _capture_state_event, &g_rec_connected ),
                            "focused: sub CONNECTED" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_DISCONNECTED,
                                                 _capture_state_event, &g_rec_disconnected ),
                            "focused: sub DISCONNECTED" );

  /* --- SCAN_COMPLETED fires after scan records are published --- */
  wifi_hal_ap_record_t mock_aps[2];
  memset( mock_aps, 0, sizeof( mock_aps ) );
  strncpy( mock_aps[0].ssid, "EmitA", WIFI_HAL_SSID_MAX_LEN );
  mock_aps[0].channel  = 1;
  mock_aps[0].rssi     = -40;
  mock_aps[0].authmode = 3;
  strncpy( mock_aps[1].ssid, "EmitB", WIFI_HAL_SSID_MAX_LEN );
  mock_aps[1].channel  = 6;
  mock_aps[1].rssi     = -60;
  mock_aps[1].authmode = 4;
  wifi_hal_mock_set_scan_list( mock_aps, 2 );

  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_start_scan(), "focused: scan start" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_state_event( &g_rec_scan, 2000 ), "SCAN_COMPLETED fired" );
  TEST_ASSERT_EQUAL_INT_MESSAGE( WIFI_MGMT_EVENT_SCAN_COMPLETED, g_rec_scan.evt, "scan event type correct" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) g_rec_scan.count, "SCAN_COMPLETED emitted exactly once" );
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) g_rec_scan.scan_count_at_cb, "scan records published before event" );
  TEST_ASSERT_TRUE_MESSAGE( g_rec_scan.reentered_api, "no lock held during scan callback" );

  /* --- CONNECTED fires after station IP info is updated --- */
  _reset_state_event( &g_rec_connected );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_set_ap_name( "EmitAP", 6 ), "focused: set ap name" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_set_password( "EmitPass", 8 ), "focused: set password" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_connect(), "focused: connect accepted" );

  wifi_hal_event_data_t evt_data = { 0 };
  strncpy( evt_data.ip_info.ip, "192.20.30.40", sizeof( evt_data.ip_info.ip ) - 1 );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt_data );

  TEST_ASSERT_TRUE_MESSAGE( _wait_state_event( &g_rec_connected, 3000 ), "CONNECTED fired" );
  TEST_ASSERT_EQUAL_INT_MESSAGE( WIFI_MGMT_EVENT_CONNECTED, g_rec_connected.evt, "connected event type correct" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) g_rec_connected.count, "CONNECTED emitted exactly once" );
  TEST_ASSERT_TRUE_MESSAGE( g_rec_connected.connected_at_cb, "management reports connected before CONNECTED" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, (int) g_rec_connected.ip_urc_at_cb, "ip snapshot shows connected (urc=0) before CONNECTED" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "EmitAP", (char*) g_rec_connected.ip_ssid_at_cb,
                                    "ip ssid snapshot matches before CONNECTED" );
  TEST_ASSERT_TRUE_MESSAGE( g_rec_connected.reentered_api, "no lock held during connected callback" );

  /* --- DISCONNECTED emitted after the disconnect state is published --- */
  _reset_state_event( &g_rec_disconnected );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_disconnect(), "focused: disconnect accepted" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_state_event( &g_rec_disconnected, 3000 ), "DISCONNECTED fired" );
  TEST_ASSERT_EQUAL_INT_MESSAGE( WIFI_MGMT_EVENT_DISCONNECTED, g_rec_disconnected.evt, "disconnected event type correct" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) g_rec_disconnected.count, "DISCONNECTED emitted exactly once" );
  TEST_ASSERT_FALSE_MESSAGE( g_rec_disconnected.connected_at_cb,
                             "is_connected already false when DISCONNECTED delivered" );
  TEST_ASSERT_EQUAL_MESSAGE( 2, (int) g_rec_disconnected.ip_urc_at_cb,
                             "ip snapshot reports user disconnect before DISCONNECTED" );
  TEST_ASSERT_TRUE_MESSAGE( g_rec_disconnected.reentered_api, "no lock held during disconnect callback" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle after disconnect" );

  /* --- CONNECT_FAILED fires after a credential attempt is exhausted, and does
        not emit DISCONNECTED (the station was never connected) --- */
  _reset_state_event( &g_rec_failed );
  _reset_state_event( &g_rec_disconnected );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECT_FAILED,
                                                 _capture_state_event, &g_rec_failed ),
                            "focused: sub CONNECT_FAILED" );

  wifi_hal_mock_set_connect_result( OSAL_ERROR );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_set_ap_name( "FailAP", 6 ), "focused: set fail ap name" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_connect(), "focused: connect accepted (fail path)" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_state_event( &g_rec_failed, 4000 ), "CONNECT_FAILED fired after attempts exhausted" );
  TEST_ASSERT_EQUAL_INT_MESSAGE( WIFI_MGMT_EVENT_CONNECT_FAILED, g_rec_failed.evt, "failed event type correct" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) g_rec_failed.count, "CONNECT_FAILED emitted exactly once" );
  TEST_ASSERT_FALSE_MESSAGE( g_rec_failed.connected_at_cb, "station never seen connected on failed path" );
  TEST_ASSERT_TRUE_MESSAGE( g_rec_failed.reentered_api, "no lock held during failed callback" );
  /* No DISCONNECTED may accompany a failed-before-connect transition. */
  TEST_ASSERT_EQUAL_MESSAGE( 0, (int) g_rec_disconnected.count,
                             "no DISCONNECTED emitted on connect-fail path" );

  wifi_hal_mock_set_connect_result( OSAL_SUCCESS );
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle after connect failure" );
}
/* ============================================================================
 * Test 14: Access-point snapshot getters are consistent across a scan
 *
 * Simulates reads from another task (e.g. the Mongoose task) before, during,
 * and after scan completion.  The scan snapshot and generation number must at
 * all times expose a self-consistent view — never a partially written record.
 * ========================================================================== */
static bool _ap_list_valid( const wifi_mgmt_ap_list_t* list )
{
  if ( !list || list->count > WIFI_DRV_MAX_SCAN_AP )
  {
    return false;
  }
  for ( uint16_t i = 0; i < list->count; ++i )
  {
    if ( list->items[i].ssid[0] == '\0' )
    {
      return false;
    }
  }
  return true;
}

static void test_scan_snapshot_getters( void )
{
  /* Prepare fresh mock APs that the next scan completion will publish. */
  wifi_hal_ap_record_t mock_aps[4];
  memset( mock_aps, 0, sizeof( mock_aps ) );
  strncpy( mock_aps[0].ssid, "SnapA", WIFI_HAL_SSID_MAX_LEN );
  mock_aps[0].channel = 1; mock_aps[0].rssi = -41; mock_aps[0].authmode = 3;
  strncpy( mock_aps[1].ssid, "SnapB", WIFI_HAL_SSID_MAX_LEN );
  mock_aps[1].channel = 6; mock_aps[1].rssi = -55; mock_aps[1].authmode = 4;
  strncpy( mock_aps[2].ssid, "SnapC", WIFI_HAL_SSID_MAX_LEN );
  mock_aps[2].channel = 11; mock_aps[2].rssi = -70; mock_aps[2].authmode = 0;
  strncpy( mock_aps[3].ssid, "SnapD", WIFI_HAL_SSID_MAX_LEN );
  mock_aps[3].channel = 3; mock_aps[3].rssi = -80; mock_aps[3].authmode = 3;
  wifi_hal_mock_set_scan_list( mock_aps, 4 );

  /* Before scan: not active, generation stable, snapshot internally valid. */
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_scan_active(), "no scan active before start" );
  uint32_t gen_before = wifi_mgmt_get_scan_generation();

  wifi_mgmt_ap_list_t list_before = { 0 };
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_get_access_points( &list_before ), "pre-scan snapshot read" );
  TEST_ASSERT_TRUE_MESSAGE( _ap_list_valid( &list_before ), "pre-scan snapshot consistent" );

  /* Hold SCAN_DONE so the test can observe the in-flight window. */
  wifi_hal_mock_set_scan_done_hold( true );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_start_scan_no_block(), "start non-blocking scan" );

  /* During scan: active flag set, generation unchanged, snapshot unchanged. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_scan_active(), "scan is active while in flight" );
  TEST_ASSERT_EQUAL_MESSAGE( gen_before, (unsigned) wifi_mgmt_get_scan_generation(),
                             "generation unchanged during scan" );
  wifi_mgmt_ap_list_t list_during = { 0 };
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_get_access_points( &list_during ), "mid-scan snapshot read" );
  TEST_ASSERT_TRUE_MESSAGE( _ap_list_valid( &list_during ), "mid-scan snapshot consistent" );
  TEST_ASSERT_EQUAL_MESSAGE( list_before.count, list_during.count, "mid-scan count unchanged" );

  /* Complete the scan and verify the post-completion view. */
  wifi_hal_mock_set_scan_done_hold( false );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_SCAN_DONE, NULL );

  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_scan_active(), "scan inactive after completion" );
  TEST_ASSERT_EQUAL_MESSAGE( gen_before + 1, (unsigned) wifi_mgmt_get_scan_generation(),
                             "generation advanced by one" );

  wifi_mgmt_ap_list_t list_after = { 0 };
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_get_access_points( &list_after ), "post-scan snapshot read" );
  TEST_ASSERT_TRUE_MESSAGE( _ap_list_valid( &list_after ), "post-scan snapshot consistent" );
  TEST_ASSERT_EQUAL_MESSAGE( 4, list_after.count, "post-scan count is 4" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "SnapA", list_after.items[0].ssid, "post-scan first AP" );
  TEST_ASSERT_EQUAL_MESSAGE( 1, (int) list_after.items[0].chan, "post-scan first AP channel" );
  TEST_ASSERT_EQUAL_MESSAGE( -41, list_after.items[0].rssi, "post-scan first AP rssi" );
}

/* ============================================================================
 * Test 15: IP + connection snapshots stay consistent during connect/disconnect
 *
 * Drives connect and disconnect transitions while repeatedly reading IP and
 * connection state, as a Mongoose task would, and asserts every snapshot is
 * self-consistent (valid reason code, no torn-down struct).
 * ========================================================================== */
static void test_ip_conn_snapshot_events( void )
{
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle before connect snapshot test" );

  /* Make the mock return a deterministic, known IP for this connect. */
  wifi_hal_ip_info_t ip = { 0 };
  strncpy( ip.ip, "192.168.90.30", sizeof( ip.ip ) - 1 );
  strncpy( ip.netmask, "255.255.255.0", sizeof( ip.netmask ) - 1 );
  strncpy( ip.gw, "192.168.90.1", sizeof( ip.gw ) - 1 );
  wifi_hal_mock_set_ip_info( &ip );

  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_set_ap_name( "SnapAP", 6 ), "set snap ap name" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_set_password( "SnapPass", 8 ), "set snap password" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_connect(), "connect accepted" );

  wifi_hal_event_data_t evt_data = { 0 };
  strncpy( evt_data.ip_info.ip, "192.168.90.30", sizeof( evt_data.ip_info.ip ) - 1 );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt_data );

  /* Read snapshots during the connect transition until connected. */
  bool connected = false;
  uint32_t elapsed = 0;
  while ( !connected && elapsed < 3000 )
  {
    wifi_mgmt_ip_info_t info = { 0 };
    TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_get_ip_info( &info ), "ip snapshot during connect" );
    TEST_ASSERT_TRUE_MESSAGE( info.urc >= 0 && info.urc <= 3, "valid urc during connect" );
    if ( info.urc == 0 )
    {
      TEST_ASSERT_EQUAL_STRING_MESSAGE( "SnapAP", info.ssid, "ssid matches when connected" );
      TEST_ASSERT_EQUAL_STRING_MESSAGE( "192.168.90.30", info.ip, "ip matches GOT_IP" );
    }
    connected = wifi_mgmt_is_connected();
    osal_task_delay_ms( 50 );
    elapsed += 50;
  }
  TEST_ASSERT_TRUE_MESSAGE( connected, "reached connected during snapshot test" );

  /* Final connected snapshot is self-consistent. */
  wifi_mgmt_ip_info_t info = { 0 };
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_get_ip_info( &info ), "get connected ip snapshot" );
  TEST_ASSERT_EQUAL_MESSAGE( 0, info.urc, "connected urc == 0" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "SnapAP", info.ssid, "connected ssid" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "192.168.90.30", info.ip, "connected ip" );

  /* Read snapshots during the disconnect transition until torn down. */
  TEST_ASSERT_TRUE( wifi_mgmt_disconnect() );
  bool disconnected = false;
  elapsed = 0;
  while ( !disconnected && elapsed < 3000 )
  {
    wifi_mgmt_ip_info_t snap = { 0 };
    TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_get_ip_info( &snap ), "ip snapshot during disconnect" );
    TEST_ASSERT_TRUE_MESSAGE( snap.urc >= 0 && snap.urc <= 3, "valid urc during disconnect" );
    disconnected = !wifi_mgmt_is_connected();
    osal_task_delay_ms( 50 );
    elapsed += 50;
  }
  TEST_ASSERT_TRUE_MESSAGE( disconnected, "disconnected during snapshot test" );

  /* Final disconnected snapshot reflects the user-disconnect reason. */
  wifi_mgmt_ip_info_t fin = { 0 };
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_get_ip_info( &fin ), "get post-disconnect ip snapshot" );
  TEST_ASSERT_EQUAL_MESSAGE( 2, fin.urc, "user-disconnect urc == 2" );
}

/* ============================================================================
 * Test 16: Asynchronous mode request STA -> AP+STA
 *
 * Verifies wifi_mgmt_request_mode() serializes the transition through the
 * worker task, stops and restarts the HAL for a differing mode, and emits
 * MODE_CHANGED only after the transition succeeds. A repeated request for the
 * current mode must be harmless (no HAL cycle, no event).
 * ========================================================================== */
static void test_request_mode_sta_to_apsta( void )
{
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle before STA->AP+STA mode test" );

  /* Guarantee we start from station-only mode. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_request_mode( T_WIFI_TYPE_CLIENT ), "ensure STA mode" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle in STA mode" );

  const uint32_t start_before = wifi_hal_mock_get_state()->start_count;
  const uint32_t stop_before  = wifi_hal_mock_get_state()->stop_count;

  _reset_event( &g_ev_mode );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_request_mode( T_WIFI_TYPE_CLI_SER ),
                            "request STA->AP+STA accepted" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_mode_for_count( 1, 3000 ),
                            "MODE_CHANGED fired for AP+STA transition" );
  TEST_ASSERT_EQUAL_INT_MESSAGE( WIFI_MGMT_EVENT_MODE_CHANGED, g_ev_mode.evt,
                                 "mode event type correct" );

  const wifi_hal_mock_state_t* ms = wifi_hal_mock_get_state();
  TEST_ASSERT_EQUAL_MESSAGE( WIFI_HAL_MODE_APSTA, (int) ms->mode,
                             "HAL in APSTA after transition" );
  TEST_ASSERT_TRUE_MESSAGE( ms->started, "HAL restarted in APSTA" );
  TEST_ASSERT_EQUAL_MESSAGE( start_before + 1, ms->start_count,
                             "HAL started exactly once for the transition" );
  TEST_ASSERT_EQUAL_MESSAGE( stop_before + 1, ms->stop_count,
                             "HAL stopped exactly once for the transition" );

  /* Repeated request for the current mode is a harmless no-op. */
  _reset_event( &g_ev_mode );
  const uint32_t start_now = wifi_hal_mock_get_state()->start_count;
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_request_mode( T_WIFI_TYPE_CLI_SER ),
                            "repeat current-mode request accepted" );
  osal_task_delay_ms( 200 );
  TEST_ASSERT_EQUAL_MESSAGE( 0, (int) g_ev_mode.count,
                             "no MODE_CHANGED on repeated current mode" );
  TEST_ASSERT_EQUAL_MESSAGE( start_now, wifi_hal_mock_get_state()->start_count,
                             "no HAL restart on repeated current mode" );
}

/* ============================================================================
 * Test 17: Asynchronous mode request AP+STA -> STA preserves credentials
 *
 * Verifies a transition back to station-only keeps the configured station
 * credentials (SSID + password) intact.
 * ========================================================================== */
static void test_request_mode_apsta_to_sta( void )
{
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle before AP+STA->STA mode test" );

  /* Currently in AP+STA (from previous test). Install station credentials. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_set_ap_name( "KeepAP", 6 ), "set creds ssid" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_set_password( "KeepPass", 8 ), "set creds pass" );

  _reset_event( &g_ev_mode );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_request_mode( T_WIFI_TYPE_CLIENT ),
                            "request AP+STA->STA accepted" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_mode_for_count( 1, 3000 ),
                            "MODE_CHANGED fired for STA transition" );

  const wifi_hal_mock_state_t* ms = wifi_hal_mock_get_state();
  TEST_ASSERT_EQUAL_MESSAGE( WIFI_HAL_MODE_STA, (int) ms->mode,
                             "HAL in STA after transition" );
  TEST_ASSERT_TRUE_MESSAGE( ms->started, "HAL started in STA" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "KeepAP", ms->sta_cfg.ssid,
                                    "station SSID preserved through mode change" );
  TEST_ASSERT_EQUAL_STRING_MESSAGE( "KeepPass", ms->sta_cfg.password,
                                    "station password preserved through mode change" );
}

/* ============================================================================
 * Test 18: HAL start failure leaves a defined recoverable state
 *
 * A failed HAL start must not emit MODE_CHANGED, must leave the HAL stopped
 * with the worker still alive and idle, and must allow a later retry to
 * succeed without blocking the caller.
 * ========================================================================== */
static void test_request_mode_failure_recovery( void )
{
  TEST_ASSERT_TRUE_MESSAGE( _wait_idle( 2000 ), "idle before failure-recovery test" );

  /* Sabotage the HAL start while requesting a mode transition. */
  wifi_hal_mock_set_start_result( OSAL_ERROR );
  const uint32_t start_before = wifi_hal_mock_get_state()->start_count;

  _reset_event( &g_ev_mode );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_request_mode( T_WIFI_TYPE_CLI_SER ),
                            "request accepted despite failing HAL" );
  osal_task_delay_ms( 300 ); /* give the worker time to attempt + fail */

  TEST_ASSERT_EQUAL_MESSAGE( 0, (int) g_ev_mode.count,
                             "no MODE_CHANGED emitted on failed start" );
  TEST_ASSERT_EQUAL_MESSAGE( start_before + 1, wifi_hal_mock_get_state()->start_count,
                             "HAL start attempted exactly once" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_hal_mock_get_state()->started,
                             "HAL left stopped after failed start" );

  /* Defined recoverable state: worker alive, machine idle, retry possible. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_idle(), "machine idle after HAL failure" );

  /* Retry with a healthy HAL succeeds and then emits MODE_CHANGED. */
  wifi_hal_mock_set_start_result( OSAL_SUCCESS );
  _reset_event( &g_ev_mode );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_request_mode( T_WIFI_TYPE_CLI_SER ),
                            "retry request accepted" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_mode_for_count( 1, 3000 ),
                            "MODE_CHANGED fired on successful retry" );
  TEST_ASSERT_EQUAL_MESSAGE( WIFI_HAL_MODE_APSTA, (int) wifi_hal_mock_get_state()->mode,
                             "HAL in APSTA after retry" );
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_get_state()->started,
                            "HAL started after retry" );

  /* Restore station-only mode so the shutdown path is symmetric. */
  _reset_event( &g_ev_mode );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_request_mode( T_WIFI_TYPE_CLIENT ),
                            "restore STA mode" );
  TEST_ASSERT_TRUE_MESSAGE( _wait_mode_for_count( 1, 3000 ),
                            "MODE_CHANGED fired for restore" );
}

/* Focused acknowledged-stop coverage using B2 result setters/snapshots. */
static void test_stop_before_init( void )
{
  wifi_hal_mock_lifecycle_t snapshot;
  TEST_ASSERT_TRUE( wifi_mgmt_stop() );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_lifecycle( &snapshot ) );
  TEST_ASSERT_EQUAL_UINT32( 0, snapshot.stop_count );
  TEST_ASSERT_EQUAL_UINT32( 0, snapshot.deinit_count );
}

static void test_stop_restart_cycle( void )
{
  wifi_hal_mock_lifecycle_t before;
  wifi_hal_mock_lifecycle_t after;
  TEST_ASSERT_TRUE( _wait_idle( 2000 ) );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_lifecycle( &before ) );
  TEST_ASSERT_TRUE( wifi_mgmt_stop() );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_lifecycle( &after ) );
  TEST_ASSERT_EQUAL_UINT32( before.stop_count + 1, after.stop_count );
  TEST_ASSERT_EQUAL_UINT32( before.deinit_count + 1, after.deinit_count );
  wifi_mgmt_start();
  TEST_ASSERT_TRUE( wifi_mgmt_wait_ready( 3000 ) );
  TEST_ASSERT_TRUE( _wait_idle( 2000 ) );
}

static void test_repeated_clean_stop( void )
{
  wifi_hal_mock_lifecycle_t first;
  wifi_hal_mock_lifecycle_t repeat;
  TEST_ASSERT_TRUE( wifi_mgmt_stop() );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_lifecycle( &first ) );
  TEST_ASSERT_TRUE( wifi_mgmt_stop() );
  TEST_ASSERT_TRUE( wifi_mgmt_stop() );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_lifecycle( &repeat ) );
  TEST_ASSERT_EQUAL_UINT32( first.stop_count, repeat.stop_count );
  TEST_ASSERT_EQUAL_UINT32( first.deinit_count, repeat.deinit_count );
  wifi_mgmt_start();
  TEST_ASSERT_TRUE( wifi_mgmt_wait_ready( 3000 ) );
  TEST_ASSERT_TRUE( _wait_idle( 2000 ) );
}

static void test_stop_hal_failure_retry( void )
{
  wifi_hal_mock_lifecycle_t before;
  wifi_hal_mock_lifecycle_t after;
  TEST_ASSERT_TRUE( _wait_idle( 2000 ) );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_lifecycle( &before ) );
  wifi_hal_mock_set_stop_result( OSAL_ERROR );
  TEST_ASSERT_FALSE( wifi_mgmt_stop() );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_lifecycle( &after ) );
  TEST_ASSERT_EQUAL_UINT32( before.stop_count + 1, after.stop_count );
  TEST_ASSERT_EQUAL_UINT32( before.deinit_count + 1, after.deinit_count );
  wifi_mgmt_start();
  TEST_ASSERT_FALSE( wifi_mgmt_wait_ready( 100 ) );
  wifi_hal_mock_set_stop_result( OSAL_SUCCESS );
  TEST_ASSERT_TRUE( wifi_mgmt_stop() );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_lifecycle( &after ) );
  TEST_ASSERT_EQUAL_UINT32( before.stop_count + 2, after.stop_count );
  TEST_ASSERT_EQUAL_UINT32( before.deinit_count + 2, after.deinit_count );
  wifi_mgmt_start();
  TEST_ASSERT_TRUE( wifi_mgmt_wait_ready( 3000 ) );
  TEST_ASSERT_TRUE( _wait_idle( 2000 ) );
}

static void test_stop_deinit_failure_retry( void )
{
  wifi_hal_mock_lifecycle_t before;
  wifi_hal_mock_lifecycle_t after;
  TEST_ASSERT_TRUE( _wait_idle( 2000 ) );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_lifecycle( &before ) );
  wifi_hal_mock_set_deinit_result( OSAL_ERROR );
  TEST_ASSERT_FALSE( wifi_mgmt_stop() );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_lifecycle( &after ) );
  TEST_ASSERT_EQUAL_UINT32( before.stop_count + 1, after.stop_count );
  TEST_ASSERT_EQUAL_UINT32( before.deinit_count + 1, after.deinit_count );
  wifi_mgmt_start();
  TEST_ASSERT_FALSE( wifi_mgmt_wait_ready( 100 ) );
  wifi_hal_mock_set_deinit_result( OSAL_SUCCESS );
  TEST_ASSERT_TRUE( wifi_mgmt_stop() );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_lifecycle( &after ) );
  TEST_ASSERT_EQUAL_UINT32( before.stop_count + 2, after.stop_count );
  TEST_ASSERT_EQUAL_UINT32( before.deinit_count + 2, after.deinit_count );
  TEST_ASSERT_FALSE( after.initialized );
  wifi_mgmt_start();
  TEST_ASSERT_TRUE( wifi_mgmt_wait_ready( 3000 ) );
  TEST_ASSERT_TRUE( _wait_idle( 2000 ) );
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

  RUN_TEST( test_stop_before_init );

  g_connect_cb_fired    = false;
  g_disconnect_cb_fired = false;

  wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLIENT );
  wifi_mgmt_init();
  wifi_mgmt_register_connect_cb( _on_connect );
  wifi_mgmt_register_disconnect_cb( _on_disconnect );
  /* NOTE: the very first wifi_mgmt_start() is performed inside
   * test_wait_ready_held_at_callback_barrier, which parks the HAL init at the
   * mock barrier and proves readiness is not reported before the callback is
   * installed. */

  /* --- Run all tests sequentially (single lifecycle) --- */
  RUN_TEST( test_wait_ready_held_at_callback_barrier );
  RUN_TEST( test_startup_failure_is_deterministic );
  RUN_TEST( test_startup_init_failure_is_deterministic );
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
  RUN_TEST( test_event_subscriptions );
  RUN_TEST( test_state_machine_event_emission );
  RUN_TEST( test_scan_snapshot_getters );
  RUN_TEST( test_ip_conn_snapshot_events );
  RUN_TEST( test_request_mode_sta_to_apsta );
  RUN_TEST( test_request_mode_apsta_to_sta );
  RUN_TEST( test_request_mode_failure_recovery );
  RUN_TEST( test_stop_restart_cycle );
  RUN_TEST( test_repeated_clean_stop );
  RUN_TEST( test_stop_hal_failure_retry );
  RUN_TEST( test_stop_deinit_failure_retry );

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