/*
 * TASK-143 — Deterministic race / stress regression matrix (POSIX).
 *
 * These tests are the deterministic counterpart of the former pre-TASK-140 /
 * 141 ordering races in the Wi-Fi HTTP provisioning stack.  Each scenario is
 * barrier-driven: it parks the component under test at a known lifecycle
 * point (never a fixed wall-clock sleep) and then releases / races it against
 * a second actor, so the outcome does not depend on machine speed.
 *
 * Covered scenarios:
 *   1. Delayed HAL init        - Wi-Fi startup is parked inside wifi_hal_init
 *                                while an early STA GOT_IP is injected.  The
 *                                early event must be dropped (no delivery) and
 *                                readiness must NOT be reported until the HAL
 *                                init completes.  Releasing the barrier lets
 *                                startup finish and a normal connect complete.
 *   2. Concurrent Mongoose invocations
 *                              - the shared Mongoose poll thread is invoked
 *                                from several caller threads at once, and a
 *                                later deinit is proven clean after every
 *                                concurrent caller has observed its own
 *                                completion.
 *   3. Grace expiry vs disconnect and deinit
 *                              - a success grace timer is armed, then a
 *                                station disconnect and a controller deinit
 *                                race it from two worker tasks.  The grace is
 *                                cancelled (never retired twice), the
 *                                controller reaches a consistent DISABLED
 *                                state, a stale expiry cannot retire a fresh
 *                                session, and re-init is clean.
 *   4. Teardown while listener operations are pending
 *                              - a provisioning HTTP request worker is parked
 *                                on the shared listener when
 *                                wifi_http_provisioning_stop() and a shared
 *                                Mongoose teardown run.  The parked operation
 *                                must be released / closed without a
 *                                use-after-free and provisioning must report
 *                                STOPPED.
 *
 * All scenarios install the guarded filesystem + ownership fixture from
 * TASK-142 so a failed or parallel CTest run never contaminates another
 * test's state.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "mongoose_process.h"
#include "osal_bin_sem.h"
#include "osal_count_sem.h"
#include "osal_file.h"
#include "osal_mount.h"
#include "osal_task.h"
#include "unity.h"
#include "wifi_config.h"
#include "wifi_hal_mock.h"
#include "wifi_http_provisioning.h"
#include "wifi_managment.h"
#include "wifi_provisioning_controller.h"

#ifdef ESP_PLATFORM
#error "wifi_provisioning_race_test.c targets POSIX only"
#endif

#define STATUS_PATH      "/api/v1/wifi/status"
#define TEST_GRACE_MS    250u
#define TEST_IMAGE_PATH  "/tmp/wifi_prov_race.img"
#define TEST_MOUNT_POINT "/"

#define READY_WAIT_MS      4000u
#define HOLD_WAIT_MS       2000u
#define CONNECT_WAIT_MS    6000u
#define EVENT_WAIT_MS      6000u
#define TRANSITION_WAIT_MS ( TEST_GRACE_MS + 4000u )
#define MONITOR_TICK_MS    5u
#define MONITOR_STOP_MS    20u

/* --- typed-event completion semaphores ------------------------------------ */

static osal_bin_sem_id_t s_connected_sem    = NULL;
static osal_bin_sem_id_t s_disconnected_sem = NULL;
static osal_bin_sem_id_t s_mode_changed_sem = NULL;
static osal_bin_sem_id_t s_prov_stopped_sem = NULL;
static osal_bin_sem_id_t s_deinit_done      = NULL;
static osal_count_sem_id_t s_race_start_sem = NULL;

static volatile bool  s_watch_stop = false;
static osal_task_id_t s_watch_task = 0;

static void on_connected_event( wifi_mgmt_event_t event, void * user_data )
{
  ( void ) event;
  ( void ) user_data;
  if ( s_connected_sem != NULL ) ( void ) osal_bin_sem_give( s_connected_sem );
}

static void on_disconnected_event( wifi_mgmt_event_t event, void * user_data )
{
  ( void ) event;
  ( void ) user_data;
  if ( s_disconnected_sem != NULL ) ( void ) osal_bin_sem_give( s_disconnected_sem );
}

static void on_mode_changed_event( wifi_mgmt_event_t event, void * user_data )
{
  wifi_hal_mock_state_t mock;

  ( void ) event;
  ( void ) user_data;
  ( void ) wifi_hal_mock_get_state( &mock );
  if ( mock.mode == WIFI_HAL_MODE_STA && s_mode_changed_sem != NULL )
  {
    ( void ) osal_bin_sem_give( s_mode_changed_sem );
  }
}

static void prov_stopped_watch_fn( void * arg )
{
  bool signalled = false;

  ( void ) arg;
  while ( !s_watch_stop )
  {
    if ( wifi_http_provisioning_get_state() == WIFI_PROVISIONING_STOPPED )
    {
      if ( !signalled && s_prov_stopped_sem != NULL )
      {
        signalled = true;
        ( void ) osal_bin_sem_give( s_prov_stopped_sem );
      }
    }
    else
    {
      signalled = false;
    }
    ( void ) osal_task_delay_ms( MONITOR_TICK_MS );
  }
}

static bool start_prov_stopped_watch( void )
{
  if ( s_watch_task != 0 ) return true;
  if ( s_prov_stopped_sem == NULL ) return false;
  s_watch_stop = false;
  return osal_task_create( &s_watch_task, "race_watch", prov_stopped_watch_fn,
                           NULL, NULL, OSAL_TASK_MIN_STACK_SIZE * 2,
                           1u, NULL ) == OSAL_SUCCESS;
}

static void stop_prov_stopped_watch( void )
{
  if ( s_watch_task != 0 )
  {
    s_watch_stop = true;
    ( void ) osal_task_delay_ms( MONITOR_STOP_MS );
    ( void ) osal_task_delete( s_watch_task );
    s_watch_task = 0;
  }
}

static bool wait_semaphore( osal_bin_sem_id_t sem, uint32_t timeout_ms )
{
  if ( sem == NULL ) return false;
  return osal_bin_sem_timed_wait( sem, timeout_ms ) == OSAL_SUCCESS;
}

/* --- guarded ownership / filesystem fixture (TASK-142) --------------------- */

static void subscribe_test_events( void );

static void setup_fs( void )
{
  ( void ) osal_unmount( TEST_MOUNT_POINT );
  ( void ) osal_rmfs( TEST_IMAGE_PATH );
  ( void ) osal_mkfs( NULL, TEST_IMAGE_PATH, TEST_MOUNT_POINT, 4096U, 256U );
  ( void ) osal_mount( TEST_IMAGE_PATH, TEST_MOUNT_POINT );
  ( void ) osal_remove( WIFI_CONFIG_FILE_PATH );
}

static void cleanup_fs( void )
{
  ( void ) osal_remove( WIFI_CONFIG_FILE_PATH );
  ( void ) osal_unmount( TEST_MOUNT_POINT );
  ( void ) osal_rmfs( TEST_IMAGE_PATH );
}

void setUp( void )
{
  setup_fs();
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock must reset in setUp" );
  wifi_hal_mock_set_start_result( OSAL_SUCCESS );
  wifi_hal_mock_set_connect_result( OSAL_SUCCESS );

  if ( s_connected_sem == NULL )
    ( void ) osal_bin_sem_create( &s_connected_sem, "r_conn", OSAL_SEM_EMPTY );
  if ( s_disconnected_sem == NULL )
    ( void ) osal_bin_sem_create( &s_disconnected_sem, "r_disc", OSAL_SEM_EMPTY );
  if ( s_mode_changed_sem == NULL )
    ( void ) osal_bin_sem_create( &s_mode_changed_sem, "r_mode", OSAL_SEM_EMPTY );
  if ( s_prov_stopped_sem == NULL )
    ( void ) osal_bin_sem_create( &s_prov_stopped_sem, "r_stop", OSAL_SEM_EMPTY );
  if ( s_deinit_done == NULL )
    ( void ) osal_bin_sem_create( &s_deinit_done, "r_deinit", OSAL_SEM_EMPTY );
  if ( s_race_start_sem == NULL )
    ( void ) osal_count_sem_create( &s_race_start_sem, "race_start", 0u, 2u );

  wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLI_SER );
  wifi_mgmt_init();
  MongooseProcess_Init();
  TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );
  subscribe_test_events();
}

void tearDown( void )
{
  wifi_hal_mock_set_init_hold( false );
  wifi_hal_mock_set_start_hold( false );
  wifi_hal_mock_set_connect_hold( false );
  wifi_hal_mock_set_deinit_hold( false );
  wifi_hal_mock_set_got_ip_hold( false );
  wifi_hal_mock_set_scan_done_hold( false );

  ( void ) wifi_http_provisioning_stop();
  ( void ) wifi_provisioning_controller_deinit();
  stop_prov_stopped_watch();
  ( void ) wifi_mgmt_stop();
  ( void ) wifi_mgmt_deinit();
  MongooseProcess_Deinit();

  cleanup_fs();

  if ( s_connected_sem != NULL )
  {
    ( void ) osal_bin_sem_delete( s_connected_sem );
    s_connected_sem = NULL;
  }
  if ( s_disconnected_sem != NULL )
  {
    ( void ) osal_bin_sem_delete( s_disconnected_sem );
    s_disconnected_sem = NULL;
  }
  if ( s_mode_changed_sem != NULL )
  {
    ( void ) osal_bin_sem_delete( s_mode_changed_sem );
    s_mode_changed_sem = NULL;
  }
  if ( s_prov_stopped_sem != NULL )
  {
    ( void ) osal_bin_sem_delete( s_prov_stopped_sem );
    s_prov_stopped_sem = NULL;
  }
  if ( s_deinit_done != NULL )
  {
    ( void ) osal_bin_sem_delete( s_deinit_done );
    s_deinit_done = NULL;
  }
  if ( s_race_start_sem != NULL )
  {
    ( void ) osal_count_sem_delete( s_race_start_sem );
    s_race_start_sem = NULL;
  }
}

/* --- small helpers --------------------------------------------------------- */

static int reserve_port( int type )
{
  struct sockaddr_in addr;
  socklen_t          len = sizeof( addr );
  int                fd  = ( int ) socket( AF_INET, type, 0 );

  if ( fd < 0 ) return -1;
  memset( &addr, 0, sizeof( addr ) );
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  addr.sin_port        = 0;
  if ( bind( fd, ( struct sockaddr * ) &addr, sizeof( addr ) ) != 0 )
  {
    close( fd );
    return -1;
  }
  if ( getsockname( fd, ( struct sockaddr * ) &addr, &len ) != 0 )
  {
    close( fd );
    return -1;
  }
  int port = ( int ) ntohs( addr.sin_port );
  close( fd );
  return port;
}

static int http_request( int port, const char * method, const char * path,
                         char * out, size_t outcap )
{
  struct sockaddr_in addr;
  struct timeval     tv;
  char               req[256];
  char               buf[512];
  int                fd;
  size_t             total = 0;

  fd = ( int ) socket( AF_INET, SOCK_STREAM, 0 );
  if ( fd < 0 ) return -1;

  memset( &addr, 0, sizeof( addr ) );
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  addr.sin_port        = htons( ( uint16_t ) port );
  if ( connect( fd, ( struct sockaddr * ) &addr, sizeof( addr ) ) != 0 )
  {
    close( fd );
    return -1;
  }

  snprintf( req, sizeof( req ), "%s %s HTTP/1.1\r\nHost: localhost\r\n\r\n",
            method, path );
  ( void ) send( fd, req, ( int ) strlen( req ), 0 );

  tv.tv_sec  = 2;
  tv.tv_usec = 0;
  ( void ) setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );

  while ( total < outcap )
  {
    int n = ( int ) recv( fd, buf, 512, 0 );
    if ( n <= 0 ) break;
    memcpy( out + total, buf, ( size_t ) n );
    total += ( size_t ) n;
  }
  close( fd );
  return ( int ) total;
}

static void start_wifi_ready( void )
{
  wifi_mgmt_start();
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_wait_ready( READY_WAIT_MS ),
                            "Wi-Fi management must become ready" );
  TEST_ASSERT_TRUE( wifi_mgmt_is_running() );
}

static int reserve_listeners( char * http_url, size_t http_cap,
                              char * dns_url, size_t dns_cap )
{
  int http_port = reserve_port( SOCK_STREAM );
  int dns_port  = reserve_port( SOCK_DGRAM );

  TEST_ASSERT_TRUE( http_port > 0 );
  TEST_ASSERT_TRUE( dns_port > 0 );
  snprintf( http_url, http_cap, "http://127.0.0.1:%d", http_port );
  snprintf( dns_url, dns_cap, "udp://127.0.0.1:%d", dns_port );
  TEST_ASSERT_TRUE( wifi_http_provisioning_set_http_url( http_url ) );
  TEST_ASSERT_TRUE( wifi_http_provisioning_set_dns_url( dns_url ) );
  return http_port;
}

static void subscribe_test_events( void )
{
  TEST_ASSERT_TRUE_MESSAGE(
    wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECTED, on_connected_event, NULL ),
    "must subscribe to CONNECTED" );
  TEST_ASSERT_TRUE_MESSAGE(
    wifi_mgmt_subscribe( WIFI_MGMT_EVENT_DISCONNECTED, on_disconnected_event, NULL ),
    "must subscribe to DISCONNECTED" );
  TEST_ASSERT_TRUE_MESSAGE(
    wifi_mgmt_subscribe( WIFI_MGMT_EVENT_MODE_CHANGED, on_mode_changed_event, NULL ),
    "must subscribe to MODE_CHANGED" );
}

static void complete_station_connection( const char * ip )
{
  wifi_hal_ip_info_t    ip_info;
  wifi_hal_event_data_t evt;

  memset( &ip_info, 0, sizeof( ip_info ) );
  strncpy( ip_info.ip,      ip,              sizeof( ip_info.ip ) - 1 );
  strncpy( ip_info.netmask, "255.255.255.0", sizeof( ip_info.netmask ) - 1 );
  strncpy( ip_info.gw,      "10.170.0.1",    sizeof( ip_info.gw ) - 1 );
  wifi_hal_mock_set_ip_info( &ip_info );
  memset( &evt, 0, sizeof( evt ) );
  evt.ip_info = ip_info;
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );

  TEST_ASSERT_TRUE_MESSAGE( wait_semaphore( s_connected_sem, EVENT_WAIT_MS ),
                            "GOT_IP must complete the station connection" );
  TEST_ASSERT_TRUE( wifi_mgmt_is_connected() );
}

/* Deliver a GOT_IP that completes a connect already parked in WAIT_CONNECT. */
static void connect_station_through_portal( void )
{
  TEST_ASSERT_TRUE( wifi_mgmt_set_ap_name( "testnet", ( size_t ) 7 ) );
  TEST_ASSERT_TRUE( wifi_mgmt_set_password( "pw", ( size_t ) 2 ) );
  TEST_ASSERT_TRUE( wifi_mgmt_connect() );
  TEST_ASSERT_TRUE_MESSAGE(
    wifi_hal_mock_wait_connect_completed_level( 1, CONNECT_WAIT_MS ),
    "station must reach WAIT_CONNECT before GOT_IP injection" );
  complete_station_connection( "10.170.0.50" );
}

/* ----------------------------------------------------------------- */
/*  Scenario 1 - delayed HAL init: early GOT_IP is rejected, then   */
/*  the readiness barrier is released and the stack recovers.        */
/* ----------------------------------------------------------------- */

static void test_delayed_hal_init_drops_early_gotip_and_recovers( void )
{
  wifi_hal_event_data_t ev_data;
  uint32_t              delivered_before;

  wifi_hal_mock_set_init_hold( true );
  wifi_mgmt_start();

  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered( HOLD_WAIT_MS ),
                            "worker must park inside HAL init at the barrier" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_wait_ready( 150u ),
                             "readiness must NOT be reported while init is held" );
  TEST_ASSERT_FALSE( wifi_mgmt_is_running() );

  /* An early GOT_IP is dropped: no delivery to the state machine. */
  delivered_before = wifi_hal_mock_get_got_ip_delivered_count();
  memset( &ev_data, 0, sizeof( ev_data ) );
  strncpy( ev_data.ip_info.ip, "10.170.0.50", sizeof( ev_data.ip_info.ip ) - 1 );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &ev_data );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_connected(),
                             "early GOT_IP must not connect the station" );
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
    delivered_before, wifi_hal_mock_get_got_ip_delivered_count(),
    "early GOT_IP must not be delivered before HAL readiness" );

  wifi_hal_mock_set_init_hold( false );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_wait_ready( READY_WAIT_MS ),
                            "Wi-Fi must become ready once init completes" );
  TEST_ASSERT_TRUE( wifi_mgmt_is_running() );

  connect_station_through_portal();
  printf( "PASS: delayed HAL init dropped early GOT_IP and recovered\n" );
}

/* ----------------------------------------------------------------- */
/*  Scenario 2 - concurrent Mongoose invocations.                    */
/* ----------------------------------------------------------------- */

#define RACE_CONCURRENT_THREADS 6
#define RACE_CONCURRENT_CALLS   5

static int g_cb_runs;
static pthread_mutex_t g_race_mutex = PTHREAD_MUTEX_INITIALIZER;

static void race_counting_cb( struct mg_mgr * mgr, void * user )
{
  ( void ) mgr;
  ( void ) user;
  ( void ) pthread_mutex_lock( &g_race_mutex );
  g_cb_runs++;
  ( void ) pthread_mutex_unlock( &g_race_mutex );
}

static void *race_mongoose_worker( void * arg )
{
  int * ok_count = ( int * ) arg;

  for ( int i = 0; i < RACE_CONCURRENT_CALLS; i++ )
  {
    if ( MongooseProcess_Invoke( race_counting_cb, NULL, 2000u ) )
      *ok_count += 1;
  }
  return NULL;
}

static void test_concurrent_mongoose_invocations( void )
{
  pthread_t threads[RACE_CONCURRENT_THREADS];
  int       successes[RACE_CONCURRENT_THREADS] = { 0 };

  g_cb_runs = 0;
  for ( int i = 0; i < RACE_CONCURRENT_THREADS; i++ )
  {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
      0, pthread_create( &threads[i], NULL, race_mongoose_worker,
                         &successes[i] ),
      "pthread_create failed" );
  }
  for ( int i = 0; i < RACE_CONCURRENT_THREADS; i++ )
    pthread_join( threads[i], NULL );

  for ( int i = 0; i < RACE_CONCURRENT_THREADS; i++ )
  {
    TEST_ASSERT_EQUAL_INT_MESSAGE(
      RACE_CONCURRENT_CALLS, successes[i],
      "a concurrent invoker missed its own completion" );
  }
  TEST_ASSERT_EQUAL_INT( RACE_CONCURRENT_THREADS * RACE_CONCURRENT_CALLS,
                         g_cb_runs );
  printf( "PASS: concurrent Mongoose invocations all completed\n" );
}

/* ----------------------------------------------------------------- */
/*  Scenario 3 - grace expiry racing station disconnect and          */
/*  controller deinit.                                               */
/* ----------------------------------------------------------------- */

/* A counting-semaphore barrier parks both worker tasks until the main task
 * arms the race, so the grace/disconnect/deinit interleaving is deterministic
 * and the barrier itself is not a cross-thread data race (ThreadSanitizer). */
static void race_disconnect_fn( void * arg )
{
  ( void ) arg;
  if ( s_race_start_sem != NULL ) ( void ) osal_count_sem_take( s_race_start_sem );
  ( void ) wifi_mgmt_disconnect();
}

static void race_deinit_fn( void * arg )
{
  ( void ) arg;
  if ( s_race_start_sem != NULL ) ( void ) osal_count_sem_take( s_race_start_sem );
  ( void ) wifi_provisioning_controller_deinit();
  if ( s_deinit_done != NULL ) ( void ) osal_bin_sem_give( s_deinit_done );
}

static void test_grace_expiry_racing_disconnect_and_deinit( void )
{
  char                  http_url[64], dns_url[64];
  osal_task_id_t        t_disc   = 0;
  osal_task_id_t        t_deinit = 0;
  wifi_hal_mock_state_t state_snapshot;

  start_wifi_ready();
  ( void ) reserve_listeners( http_url, sizeof( http_url ),
                              dns_url, sizeof( dns_url ) );
  wifi_provisioning_controller_set_success_grace_ms( TEST_GRACE_MS );

  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                         wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );

  /* Arm a genuine grace session (a station connects through the portal). */
  connect_station_through_portal();
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_GRACE,
                         wifi_provisioning_controller_get_state() );

  /* Barrier: race a station disconnect and a controller deinit against the
   * armed grace timer.  controller_deinit cancels and joins the grace timer
   * synchronously, so this is deterministic regardless of machine speed. */
  TEST_ASSERT_TRUE( s_deinit_done != NULL );
  TEST_ASSERT_TRUE( s_race_start_sem != NULL );
  TEST_ASSERT_TRUE( osal_task_create( &t_disc,   "r_disc",   race_disconnect_fn,
                                      NULL, NULL, OSAL_TASK_MIN_STACK_SIZE * 2,
                                      1u, NULL ) == OSAL_SUCCESS );
  TEST_ASSERT_TRUE( osal_task_create( &t_deinit, "r_deinit", race_deinit_fn,
                                      NULL, NULL, OSAL_TASK_MIN_STACK_SIZE * 2,
                                      1u, NULL ) == OSAL_SUCCESS );
  /* Release the barrier so both workers race the armed grace timer. */
  ( void ) osal_count_sem_give( s_race_start_sem );
  ( void ) osal_count_sem_give( s_race_start_sem );

  TEST_ASSERT_TRUE_MESSAGE(
    wait_semaphore( s_deinit_done, TRANSITION_WAIT_MS ),
    "controller deinit must complete while racing the grace timer" );
  ( void ) osal_task_delay_ms( TEST_GRACE_MS + 100u );
  ( void ) osal_task_delete( t_disc );
  ( void ) osal_task_delete( t_deinit );

  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                         wifi_provisioning_controller_get_state() );

  /* The stale expiry of the cancelled session cannot retire a fresh session:
   * re-init immediately and confirm the portal stays up past the old grace. */
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                         wifi_provisioning_controller_get_state() );
  ( void ) osal_task_delay_ms( TEST_GRACE_MS + 200u );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_state( &state_snapshot ) );
  TEST_ASSERT_EQUAL_INT( WIFI_HAL_MODE_APSTA, state_snapshot.mode );

  ( void ) wifi_provisioning_controller_deinit();
  printf( "PASS: grace disconnect/deinit race cancelled timer cleanly\n" );
}

/* ----------------------------------------------------------------- */
/*  Scenario 4 - teardown while a listener operation is pending.     */
/* ----------------------------------------------------------------- */

static volatile int    s_http_port = 0;
static volatile int    s_http_resp = 0;
static osal_task_id_t  s_http_task = 0;

static void pending_listener_op_fn( void * arg )
{
  char resp[2048];

  ( void ) arg;
  ( void ) osal_task_delay_ms( 50u );
  s_http_resp = http_request( s_http_port, "GET", STATUS_PATH,
                              resp, sizeof( resp ) );
}

static void test_teardown_while_listener_operations_pending( void )
{
  char http_url[64], dns_url[64];

  start_wifi_ready();
  s_http_port = reserve_listeners( http_url, sizeof( http_url ),
                                   dns_url, sizeof( dns_url ) );
  TEST_ASSERT_TRUE( wifi_http_provisioning_start() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );

  s_http_resp = 0;
  TEST_ASSERT_TRUE_MESSAGE(
    osal_task_create( &s_http_task, "http_pend", pending_listener_op_fn,
                      NULL, NULL, OSAL_TASK_MIN_STACK_SIZE * 2,
                      1u, NULL ) == OSAL_SUCCESS,
    "pending listener worker must start" );

  ( void ) osal_task_delay_ms( 30u );
  TEST_ASSERT_TRUE_MESSAGE( wifi_http_provisioning_stop(),
                            "listeners must stop while a request is pending" );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
                         wifi_http_provisioning_get_state() );

  /* Tear the shared Mongoose poll thread down underneath the parked request,
   * then re-init so the fixture owns a live process for the next teardown. */
  MongooseProcess_Deinit();
  TEST_ASSERT_FALSE( MongooseProcess_IsRunning() );

  ( void ) osal_task_delay_ms( 100u );
  if ( s_http_task != 0 )
  {
    ( void ) osal_task_delete( s_http_task );
    s_http_task = 0;
  }
  ( void ) s_http_resp;

  MongooseProcess_Init();
  TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );
  printf( "PASS: teardown closed pending listener operations cleanly\n" );
}

/* ----------------------------------------------------------------- */
/*  Runner.                                                           */
/* ----------------------------------------------------------------- */

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  int rc = 0;

  setvbuf( stdout, NULL, _IONBF, 0 );
  UNITY_BEGIN();

  RUN_TEST( test_delayed_hal_init_drops_early_gotip_and_recovers );
  RUN_TEST( test_concurrent_mongoose_invocations );
  RUN_TEST( test_grace_expiry_racing_disconnect_and_deinit );
  RUN_TEST( test_teardown_while_listener_operations_pending );

  rc = UNITY_END();
  return rc;
}