/*
 * Wi-Fi HTTP provisioning station disconnect endpoint unit tests (POSIX).
 *
 * Exercises DELETE /api/v1/wifi/connection over a real local HTTP loopback
 * socket against the shared Mongoose process and the mock Wi-Fi HAL:
 *  - an already-disconnected station returns 202 Accepted,
 *  - a connected station returns 202 Accepted and the station is actually
 *    taken to the disconnected state,
 *  - a repeated disconnect while already disconnected remains accepted and
 *    stable (idempotent),
 *  - a disconnect that cannot be requested (Wi-Fi stopped) returns a stable
 *    503 service error,
 *  - every response is JSON with Cache-Control: no-store and contains no
 *    credential data,
 *  - the provisioning HTTP listener and lifecycle stay reachable (RUNNING)
 *    after a station disconnect,
 *  - unsupported HTTP methods are rejected with 405.
 *
 * TASK-140 makes the scenario event-driven instead of boolean-poll driven:
 *  - each scenario runs through RUN_TEST() so Unity installs its setjmp frame
 *    and an assertion failure is reported as a test failure instead of a
 *    SEGFAULT,
 *  - ownership setup and reverse-order cleanup live in the guarded
 *    setUp()/tearDown() fixture, so cleanup always runs after an assertion,
 *  - Wi-Fi startup waits on wifi_mgmt_wait_ready() rather than polling
 *    wifi_mgmt_is_running(),
 *  - after wifi_mgmt_connect() the test waits for the exact WAIT_CONNECT
 *    milestone (the mock connect-completion channel) before injecting
 *    WIFI_HAL_EVT_STA_GOT_IP, so no event is delivered before the state
 *    machine is ready to accept it,
 *  - the test subscribes to typed CONNECTED/DISCONNECTED events and completes
 *    on binary-test semaphores rather than polling booleans,
 *  - a regression case deliberately holds Wi-Fi initialization, proves an
 *    early GOT_IP injection is rejected (never delivered) before readiness,
 *    then releases initialization and completes a connection.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "mongoose_process.h"
#include "osal_bin_sem.h"
#include "osal_task.h"
#include "unity.h"
#include "wifi_hal_mock.h"
#include "wifi_http_provisioning.h"
#include "wifi_managment.h"

#ifdef ESP_PLATFORM
#error "wifi_http_provisioning_disconnect_test.c targets POSIX only"
#endif

#define DISCONNECT_PATH "/api/v1/wifi/connection"
#define STATUS_PATH     "/api/v1/wifi/status"

#define READY_WAIT_MS    3000u
#define HOLD_WAIT_MS     2000u
#define CONNECT_WAIT_MS  4000u
#define EVENT_WAIT_MS    4000u
#define STATUS_WAIT_MS   4000u

/* -- typed-event completion semaphores -------------------------------------- */

static osal_bin_sem_id_t s_connected_sem    = NULL;
static osal_bin_sem_id_t s_disconnected_sem = NULL;

static void on_connected_event( wifi_mgmt_event_t event, void* user_data )
{
  (void) event;
  (void) user_data;
  if ( s_connected_sem != NULL ) (void) osal_bin_sem_give( s_connected_sem );
}

static void on_disconnected_event( wifi_mgmt_event_t event, void* user_data )
{
  (void) event;
  (void) user_data;
  if ( s_disconnected_sem != NULL ) (void) osal_bin_sem_give( s_disconnected_sem );
}

static bool wait_semaphore( osal_bin_sem_id_t sem, uint32_t timeout_ms )
{
  if ( sem == NULL ) return false;
  return osal_bin_sem_timed_wait( sem, timeout_ms ) == OSAL_SUCCESS;
}

/* -- guarded ownership fixture -------------------------------------------- */

void setUp( void )
{
  /* Start every test from a clean mock and no stale saved network. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock must be reset in setUp" );
  wifi_hal_mock_set_start_result( OSAL_SUCCESS );
  wifi_hal_mock_set_connect_result( OSAL_SUCCESS );
  wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLI_SER );
  (void) remove( "wifi_ap.json" );

  /* Fresh completion semaphores (discard any token from a prior test). */
  if ( s_connected_sem == NULL )
    (void) osal_bin_sem_create( &s_connected_sem, "t_conn", OSAL_SEM_EMPTY );
  if ( s_disconnected_sem == NULL )
    (void) osal_bin_sem_create( &s_disconnected_sem, "t_disc", OSAL_SEM_EMPTY );

  /* Shared Mongoose process hosts the provisioning HTTP/DNS listeners. */
  MongooseProcess_Deinit();
  MongooseProcess_Init();
  TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );

  /* Initialize Wi-Fi management once per test; each test brings it up (and
   * the regression controls exactly where the HAL init is parked). */
  wifi_mgmt_init();
  TEST_ASSERT_TRUE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECTED,
                                         on_connected_event, NULL ) );
  TEST_ASSERT_TRUE( wifi_mgmt_subscribe( WIFI_MGMT_EVENT_DISCONNECTED,
                                         on_disconnected_event, NULL ) );
}

void tearDown( void )
{
  /* Always release any deliberately parked mock invocation so the worker can
   * drain before stop/deinit (an assertion may leave the HAL init held). */
  wifi_hal_mock_set_init_hold( false );
  wifi_hal_mock_set_start_hold( false );
  wifi_hal_mock_set_connect_hold( false );
  wifi_hal_mock_set_deinit_hold( false );
  wifi_hal_mock_set_got_ip_hold( false );
  wifi_hal_mock_set_scan_done_hold( false );

  /* Reverse-order cleanup: stop provisioning first (it owns the listeners and
   * relays Wi-Fi events), then the shared Mongoose process, then Wi-Fi. */
  (void) wifi_http_provisioning_stop();
  MongooseProcess_Deinit();
  (void) wifi_mgmt_stop();
  (void) wifi_mgmt_deinit();

  if ( s_connected_sem != NULL )
    {
      (void) osal_bin_sem_delete( s_connected_sem );
      s_connected_sem = NULL;
    }
  if ( s_disconnected_sem != NULL )
    {
      (void) osal_bin_sem_delete( s_disconnected_sem );
      s_disconnected_sem = NULL;
    }
}

/* -- socket helpers ------------------------------------------------------ */

static int reserve_port( int type )
{
  struct sockaddr_in addr;
  socklen_t          len = sizeof( addr );
  int                fd  = (int) socket( AF_INET, type, 0 );

  if ( fd < 0 ) return -1;
  memset( &addr, 0, sizeof( addr ) );
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  addr.sin_port        = 0;
  if ( bind( fd, (struct sockaddr *) &addr, sizeof( addr ) ) != 0 )
    {
      close( fd );
      return -1;
    }
  if ( getsockname( fd, (struct sockaddr *) &addr, &len ) != 0 )
    {
      close( fd );
      return -1;
    }
  int port = (int) ntohs( addr.sin_port );
  close( fd );
  return port;
}

/* Open a loopback TCP connection, send an HTTP request, read the full reply
 * (the server closes after responding) and return the number of bytes read. */
static int http_request( int port, const char * method, const char * path,
                         char * out, size_t outcap )
{
  struct sockaddr_in addr;
  struct timeval     tv;
  char               req[256];
  char               buf[512];
  int                fd;
  size_t             total = 0;

  fd = (int) socket( AF_INET, SOCK_STREAM, 0 );
  if ( fd < 0 ) return -1;

  memset( &addr, 0, sizeof( addr ) );
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  addr.sin_port        = htons( (uint16_t) port );
  if ( connect( fd, (struct sockaddr *) &addr, sizeof( addr ) ) != 0 )
    {
      close( fd );
      return -1;
    }

  snprintf( req, sizeof( req ), "%s %s HTTP/1.1\r\nHost: localhost\r\n\r\n",
            method, path );
  (void) send( fd, req, (int) strlen( req ), 0 );

  tv.tv_sec  = 2;
  tv.tv_usec = 0;
  (void) setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );

  while ( total < outcap )
    {
      int n = (int) recv( fd, buf, (int) sizeof( buf ), 0 );
      if ( n <= 0 ) break;
      if ( total + (size_t) n > outcap ) n = (int) ( outcap - total );
      if ( n <= 0 ) break;
      memcpy( out + total, buf, (size_t) n );
      total += (size_t) n;
    }
  close( fd );
  return (int) total;
}

static int parse_status_code( const char * resp )
{
  const char * sp = strchr( resp, ' ' );
  int         code;

  if ( !sp ) return -1;
  sp++;
  if ( sp[0] < '0' || sp[0] > '9' ) return -1;
  code = 0;
  while ( sp[0] >= '0' && sp[0] <= '9' )
    {
      code = code * 10 + ( sp[0] - '0' );
      sp++;
    }
  return code;
}

static const char * body_of( const char * resp )
{
  const char * p = strstr( resp, "\r\n\r\n" );
  return p ? p + 4 : "";
}

static bool contains_ci( const char * haystack, const char * needle )
{
  size_t hay_len = strlen( haystack );
  size_t ned_len = strlen( needle );
  size_t i;

  if ( ned_len == 0 ) return true;
  if ( ned_len > hay_len ) return false;

  for ( i = 0; i + ned_len <= hay_len; ++i )
    {
      size_t j;
      for ( j = 0; j < ned_len; ++j )
        {
          char a = haystack[i + j];
          char b = needle[j];
          if ( a >= 'A' && a <= 'Z' ) a = (char) ( a + ( 'a' - 'A' ) );
          if ( b >= 'A' && b <= 'Z' ) b = (char) ( b + ( 'a' - 'A' ) );
          if ( a != b ) break;
        }
      if ( j == ned_len ) return true;
    }
  return false;
}

static void assert_status_code( const char * resp, int expected )
{
  TEST_ASSERT_EQUAL_INT( expected, parse_status_code( resp ) );
}

static void assert_api_headers( const char * resp )
{
  TEST_ASSERT_TRUE_MESSAGE( contains_ci( resp, "Content-Type: application/json" ),
                            "response must set Content-Type: application/json" );
  TEST_ASSERT_TRUE_MESSAGE( contains_ci( resp, "Cache-Control: no-store" ),
                            "response must set Cache-Control: no-store" );
}

static void assert_no_credential_data( const char * resp )
{
  const char * body = body_of( resp );
  TEST_ASSERT_NULL_MESSAGE( strstr( body, "ssid" ),
                            "response must not expose an ssid field" );
  TEST_ASSERT_NULL_MESSAGE( strstr( body, "password" ),
                            "response must not expose a password field" );
}

/* Assert the response is a 202 Accepted disconnect acknowledgement with an
 * empty, credential-free JSON API shape. */
static void assert_accepted( const char * resp )
{
  const char * body;
  assert_status_code( resp, 202 );
  assert_api_headers( resp );
  body = body_of( resp );
  TEST_ASSERT_TRUE_MESSAGE( strstr( body, "\"state\":\"accepted\"" ) != NULL,
                            "accepted response must report state accepted" );
  assert_no_credential_data( resp );
}

/* Repeatedly query the status endpoint until wifi_state equals @p state.
 * Exercises proving that the provisioning HTTP listener stays reachable. */
static bool wait_for_wifi_state( int port, const char * state, int timeout_ms )
{
  char resp[2048];
  int  elapsed = 0;

  while ( elapsed < timeout_ms )
    {
      int n = http_request( port, "GET", STATUS_PATH, resp, sizeof( resp ) );
      if ( n > 0 )
        {
          char needle[64];
          const char * b = body_of( resp );
          snprintf( needle, sizeof( needle ), "\"state\":\"%s\"", state );
          if ( b && strstr( b, needle ) != NULL ) return true;
        }
      (void) osal_task_delay_ms( 20 );
      elapsed += 20;
    }
  return false;
}

/* Bring the Wi-Fi management state machine up and wait for readiness. */
static void start_wifi_ready( void )
{
  wifi_mgmt_start();
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_wait_ready( READY_WAIT_MS ),
                            "Wi-Fi management must become ready" );
  TEST_ASSERT_TRUE( wifi_mgmt_is_running() );
}

/* Reserve fresh HTTP/DNS ports, pin them on the provisioning module and return
 * the reserved HTTP port (used to poll the reachable status endpoint). */
static int configure_fresh_listeners( char * http_url, size_t http_cap,
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

/* Build the mock IP payload and deliver it to the station only after the state
 * machine is parked in WAIT_CONNECT, then wait for the typed CONNECTED event. */
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
                            "GOT_IP after WAIT_CONNECT must complete the connection" );
  TEST_ASSERT_TRUE( wifi_mgmt_is_connected() );
}

/* ------------------------------------------------------------------ */
/* Disconnect endpoint scenarios (event-driven).                      */
/* ------------------------------------------------------------------ */

static void test_disconnect_scenario( void )
{
  char         http_url[64], dns_url[64], resp[2048];
  const char * body;
  int          http_port;

  start_wifi_ready();

  http_port = configure_fresh_listeners( http_url, sizeof( http_url ),
                                         dns_url, sizeof( dns_url ) );

  /* --- start provisioning: RUNNING lifecycle, disconnected link --------- */
  TEST_ASSERT_TRUE_MESSAGE( wifi_http_provisioning_start(),
                            "provisioning must start with Wi-Fi management running" );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );

  /* --- already-disconnected: accepted, no credentials, portal reachable -- */
  TEST_ASSERT_TRUE( http_request( http_port, "DELETE", DISCONNECT_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_accepted( resp );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );

  /* --- connect a station so the real disconnect path is observable ------- */
  TEST_ASSERT_TRUE( wifi_mgmt_set_ap_name( "testnet", (size_t) 7 ) );
  TEST_ASSERT_TRUE( wifi_mgmt_set_password( "pw", (size_t) 2 ) );
  TEST_ASSERT_TRUE( wifi_mgmt_connect() );

  /* Wait for the exact WAIT_CONNECT milestone (mock connect completed) before
   * injecting GOT_IP; an event before this is dropped by the state machine. */
  TEST_ASSERT_TRUE_MESSAGE(
    wifi_hal_mock_wait_connect_completed_level( 1, CONNECT_WAIT_MS ),
    "station must reach WAIT_CONNECT before GOT_IP injection" );

  complete_station_connection( "10.170.0.50" );

  TEST_ASSERT_TRUE_MESSAGE(
    wait_for_wifi_state( http_port, "connected", STATUS_WAIT_MS ),
    "status must report connected before disconnect" );

  /* --- connected: delete requests disconnect, portal stays up ------------ */
  TEST_ASSERT_TRUE( http_request( http_port, "DELETE", DISCONNECT_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_accepted( resp );

  /* The disconnect is asynchronous: complete on the typed DISCONNECTED event. */
  TEST_ASSERT_TRUE_MESSAGE( wait_semaphore( s_disconnected_sem, EVENT_WAIT_MS ),
                            "accepted delete must take the station to disconnected" );
  TEST_ASSERT_FALSE( wifi_mgmt_is_connected() );

  /* Provisioning must remain reachable and RUNNING after the disconnect. */
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );
  TEST_ASSERT_TRUE_MESSAGE(
    wait_for_wifi_state( http_port, "disconnected", STATUS_WAIT_MS ),
    "status must remain reachable and report disconnected" );

  /* --- already-disconnected again: idempotent, still accepted ------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "DELETE", DISCONNECT_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_accepted( resp );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );

  /* --- disconnect cannot be requested (Wi-Fi stopped): stable 503 --------- */
  TEST_ASSERT_TRUE( wifi_mgmt_stop() );
  TEST_ASSERT_FALSE( wifi_mgmt_is_running() );
  TEST_ASSERT_TRUE( http_request( http_port, "DELETE", DISCONNECT_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 503 );
  assert_api_headers( resp );
  body = body_of( resp );
  TEST_ASSERT_TRUE_MESSAGE(
    strstr( body, "\"error\":\"service_unavailable\"" ) != NULL,
    "disconnect must report 503 only when Wi-Fi is stopped" );
  assert_no_credential_data( resp );

  /* --- method/route stability -------------------------------------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "POST", DISCONNECT_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 405 );
  assert_api_headers( resp );

  TEST_ASSERT_TRUE( http_request( http_port, "GET", DISCONNECT_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 405 );
  assert_api_headers( resp );
}

/*
 * Regression: an early WIFI_HAL_EVT_STA_GOT_IP injected while Wi-Fi
 * initialization is deliberately held must be rejected, then releasing the
 * initialization completes the connection.  This is the deterministic
 * counterpart of the former flaky GOT_IP-before-ready race.
 */
static void test_early_got_ip_before_wifi_ready_is_rejected( void )
{
  wifi_hal_event_data_t ev_data;
  uint32_t              delivered_before;

  /* Hold the worker inside wifi_hal_init(); the event callback is not yet
   * installed, so a GOT_IP cannot be delivered to the state machine. */
  wifi_hal_mock_set_init_hold( true );
  wifi_mgmt_start();

  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_wait_init_entered( HOLD_WAIT_MS ),
                            "worker must park inside HAL init at the barrier" );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_wait_ready( 150 ),
                             "readiness must NOT be reported while init is held" );
  TEST_ASSERT_FALSE( wifi_mgmt_is_running() );

  /* An early GOT_IP is dropped / not handled: no callback, no delivery. */
  delivered_before = wifi_hal_mock_get_got_ip_delivered_count();
  memset( &ev_data, 0, sizeof( ev_data ) );
  strncpy( ev_data.ip_info.ip, "10.170.0.50", sizeof( ev_data.ip_info.ip ) - 1 );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &ev_data );
  TEST_ASSERT_FALSE_MESSAGE( wifi_mgmt_is_connected(),
                             "early GOT_IP must not connect the station" );
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
    delivered_before, wifi_hal_mock_get_got_ip_delivered_count(),
    "early GOT_IP must not be delivered before HAL readiness" );

  /* Release initialization: startup completes and readiness is reported. */
  wifi_hal_mock_set_init_hold( false );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_wait_ready( READY_WAIT_MS ),
                            "Wi-Fi must become ready once init completes" );
  TEST_ASSERT_TRUE( wifi_mgmt_is_running() );

  /* Now a normal connect completes once the WAIT_CONNECT milestone is reached. */
  TEST_ASSERT_TRUE( wifi_mgmt_set_ap_name( "testnet", (size_t) 7 ) );
  TEST_ASSERT_TRUE( wifi_mgmt_set_password( "pw", (size_t) 2 ) );
  TEST_ASSERT_TRUE( wifi_mgmt_connect() );
  TEST_ASSERT_TRUE_MESSAGE(
    wifi_hal_mock_wait_connect_completed_level( 1, CONNECT_WAIT_MS ),
    "station must reach WAIT_CONNECT before GOT_IP injection" );
  complete_station_connection( "10.170.0.50" );
}

/* ------------------------------------------------------------------ */
/* Runner.                                                             */
/* ------------------------------------------------------------------ */

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  int rc = 0;

  setvbuf( stdout, NULL, _IONBF, 0 );
  UNITY_BEGIN();

  RUN_TEST( test_disconnect_scenario );
  RUN_TEST( test_early_got_ip_before_wifi_ready_is_rejected );

  rc = UNITY_END();
  return rc;
}