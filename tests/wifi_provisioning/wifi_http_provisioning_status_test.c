/*
 * Wi-Fi HTTP provisioning status endpoint unit tests (POSIX).
 *
 * Exercises GET /api/v1/wifi/status over a real local HTTP loopback socket
 * against the shared Mongoose process and the mock Wi-Fi HAL:
 *  - disconnected, connecting, connected, failed, and stopped responses are
 *    all verified,
 *  - the JSON field names, HTTP status code, and the stable API response
 *    headers (Content-Type: application/json, Cache-Control: no-store) are
 *    asserted,
 *  - the response never contains a Wi-Fi credential.
 *
 * The mock HAL drives the Wi-Fi link deterministically (connect results and
 * injected GOT_IP / DISCONNECTED events) so each reported wifi_state is
 * exercised without timing-dependent races.
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
#include "osal_task.h"
#include "unity.h"
#include "wifi_hal_mock.h"
#include "wifi_http_provisioning.h"
#include "wifi_managment.h"
#include "wifi_provisioning_test_fixture.h"

#ifdef ESP_PLATFORM
#error "wifi_http_provisioning_status_test.c targets POSIX only"
#endif

#define STATUS_PATH "/api/v1/wifi/status"
#define PASSWORD    "supersecretpw"

/* Dedicated littlefs image so this test never touches another test's state. */
#define TEST_IMAGE_PATH "/tmp/wifi_prov_status.img"

/* The shared fixture owns the component + filesystem stack around the run. */
void setUp( void )
{
  wifi_provisioning_test_fixture_setup();
}

void tearDown( void )
{
  wifi_provisioning_test_fixture_teardown();
}

/* -- small predicates used by the wait helpers ---------------------------- */

static bool pred_is_connecting( void )
{
  return wifi_mgmt_trying_connect();
}

static bool pred_is_connected( void )
{
  return wifi_mgmt_is_connected();
}

static bool pred_is_idle( void )
{
  return wifi_mgmt_is_idle();
}

static bool pred_is_wifi_stopped( void )
{
  return !wifi_mgmt_is_running();
}

static bool wait_bool( bool ( *pred )( void ), int timeout_ms )
{
  int elapsed = 0;
  while ( elapsed < timeout_ms )
  {
    if ( pred() ) return true;
    (void) osal_task_delay_ms( 20 );
    elapsed += 20;
  }
  return false;
}

/* -- socket helpers ------------------------------------------------------- */

static int reserve_port( int type )
{
  struct sockaddr_in addr;
  socklen_t          len = sizeof(addr);
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

static void assert_json_has( const char * body, const char * key,
                             const char * value )
{
  char needle[128];
  snprintf( needle, sizeof( needle ), "\"%s\":\"%s\"", key, value );
  TEST_ASSERT_TRUE_MESSAGE( strstr( body, needle ) != NULL, needle );
}

static void assert_no_password( const char * body )
{
  TEST_ASSERT_NULL_MESSAGE( strstr( body, PASSWORD ),
                            "response must not contain a credential string" );
  TEST_ASSERT_NULL_MESSAGE( strstr( body, "\"password\"" ),
                            "response must not expose a password field" );
}

/* Repeatedly query the endpoint until wifi_state equals @p state. Returns
 * true on success, false on timeout. */
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

static void fetch_body( int port, char * out, size_t cap,
                        const char ** body_ptr )
{
  int n = http_request( port, "GET", STATUS_PATH, out, cap );
  TEST_ASSERT_TRUE_MESSAGE( n > 0, "must receive an HTTP response" );
  *body_ptr = body_of( out );
}

/* ------------------------------------------------------------------ */
/* Status endpoint scenarios.                                          */
/* ------------------------------------------------------------------ */

static void run_status_tests( void )
{
  int   http_port = reserve_port( SOCK_STREAM );
  int   dns_port  = reserve_port( SOCK_DGRAM );
  char  http_url[64];
  char  dns_url[64];
  char  resp[2048];
  const char * body;
  wifi_hal_ip_info_t    ip_info;
  wifi_hal_event_data_t evt;

  TEST_ASSERT_TRUE( http_port > 0 );
  TEST_ASSERT_TRUE( dns_port > 0 );
  snprintf( http_url, sizeof( http_url ), "http://127.0.0.1:%d", http_port );
  snprintf( dns_url, sizeof( dns_url ), "udp://127.0.0.1:%d", dns_port );

  /* --- start provisioning: RUNNING lifecycle, disconnected link --------- */
  TEST_ASSERT_TRUE( wifi_http_provisioning_set_http_url( http_url ) );
  TEST_ASSERT_TRUE( wifi_http_provisioning_set_dns_url( dns_url ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_http_provisioning_start(),
                            "provisioning must start with Wi-Fi management running" );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING, wifi_http_provisioning_get_state() );

  TEST_ASSERT_TRUE_MESSAGE( wait_for_wifi_state( http_port, "disconnected", 4000 ),
                            "initial WiFi link must report disconnected" );
  fetch_body( http_port, resp, sizeof( resp ), &body );
  assert_status_code( resp, 200 );
  assert_api_headers( resp );
  assert_json_has( body, "provisioning", "running" );
  assert_json_has( body, "state", "disconnected" );
  assert_json_has( body, "last_failure", "no_failure" );
  assert_no_password( body );

  /* --- connecting ------------------------------------------------------ */
  TEST_ASSERT_TRUE( wifi_mgmt_set_ap_name( "testnet", (size_t) 7 ) );
  TEST_ASSERT_TRUE( wifi_mgmt_set_password( PASSWORD, (size_t) strlen( PASSWORD ) ) );
  TEST_ASSERT_TRUE( wifi_mgmt_connect() );

  TEST_ASSERT_TRUE_MESSAGE( wait_bool( pred_is_connecting, 1000 ),
                            "connect request must reach the connecting window" );
  TEST_ASSERT_TRUE_MESSAGE( wait_for_wifi_state( http_port, "connecting", 4000 ),
                            "status must report connecting" );
  fetch_body( http_port, resp, sizeof( resp ), &body );
  assert_status_code( resp, 200 );
  assert_api_headers( resp );
  assert_json_has( body, "state", "connecting" );
  assert_json_has( body, "provisioning", "running" );
  assert_json_has( body, "ssid", "" );
  assert_no_password( body );

  /* --- connected ------------------------------------------------------- */
  memset( &ip_info, 0, sizeof( ip_info ) );
  strncpy( ip_info.ip,      "10.170.0.50", sizeof( ip_info.ip ) - 1 );
  strncpy( ip_info.netmask, "255.255.255.0", sizeof( ip_info.netmask ) - 1 );
  strncpy( ip_info.gw,      "10.170.0.1", sizeof( ip_info.gw ) - 1 );
  wifi_hal_mock_set_ip_info( &ip_info );
  memset( &evt, 0, sizeof( evt ) );
  evt.ip_info = ip_info;
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );

  TEST_ASSERT_TRUE_MESSAGE( wait_bool( pred_is_connected, 1000 ),
                            "GOT_IP event must complete the station connection" );
  TEST_ASSERT_TRUE_MESSAGE( wait_for_wifi_state( http_port, "connected", 4000 ),
                            "status must report connected after GOT_IP" );
  fetch_body( http_port, resp, sizeof( resp ), &body );
  assert_status_code( resp, 200 );
  assert_api_headers( resp );
  assert_json_has( body, "state", "connected" );
  assert_json_has( body, "provisioning", "running" );
  assert_json_has( body, "ssid", "testnet" );
  assert_json_has( body, "ip", "10.170.0.50" );
  assert_json_has( body, "netmask", "255.255.255.0" );
  assert_json_has( body, "gateway", "10.170.0.1" );
  assert_json_has( body, "last_failure", "no_failure" );
  assert_no_password( body );

  /* --- disconnected (user initiated) ----------------------------------- */
  TEST_ASSERT_TRUE( wifi_mgmt_disconnect() );
  TEST_ASSERT_TRUE_MESSAGE( wait_bool( pred_is_idle, 1500 ),
                            "disconnect must return the machine to idle" );
  TEST_ASSERT_TRUE_MESSAGE( wait_for_wifi_state( http_port, "disconnected", 4000 ),
                            "status must report disconnected after disconnect" );
  fetch_body( http_port, resp, sizeof( resp ), &body );
  assert_status_code( resp, 200 );
  assert_api_headers( resp );
  assert_json_has( body, "state", "disconnected" );
  assert_json_has( body, "provisioning", "running" );
  assert_no_password( body );

  /* --- failed ---------------------------------------------------------- */
  wifi_hal_mock_set_connect_result( OSAL_ERROR );
  TEST_ASSERT_TRUE( wifi_mgmt_connect() );
  TEST_ASSERT_TRUE_MESSAGE( wait_for_wifi_state( http_port, "failed", 8000 ),
                            "status must report failed after CONNECT_FAILED" );
  fetch_body( http_port, resp, sizeof( resp ), &body );
  assert_status_code( resp, 200 );
  assert_api_headers( resp );
  assert_json_has( body, "state", "failed" );
  assert_json_has( body, "provisioning", "running" );
  assert_json_has( body, "last_failure", "connect_failed" );
  assert_no_password( body );

  /* --- stopped (Wi-Fi management stopped beneath a live portal) --------- */
  wifi_mgmt_stop();
  TEST_ASSERT_TRUE_MESSAGE( wait_bool( pred_is_wifi_stopped, 4000 ),
                            "Wi-Fi management must actually stop" );
  TEST_ASSERT_TRUE_MESSAGE( wait_for_wifi_state( http_port, "stopped", 4000 ),
                            "status must report stopped when Wi-Fi is stopped" );
  fetch_body( http_port, resp, sizeof( resp ), &body );
  assert_status_code( resp, 200 );
  assert_api_headers( resp );
  assert_json_has( body, "state", "stopped" );
  assert_json_has( body, "provisioning", "running" );
  assert_no_password( body );

  /* --- method/route stability ------------------------------------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "POST", STATUS_PATH, resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 405 );
  assert_api_headers( resp );
}

#ifdef ESP_PLATFORM
void app_main( void )
#else
int main( void )
#endif
{
  int rc = 0;

  setvbuf( stdout, NULL, _IONBF, 0 );
  wifi_provisioning_test_fixture_configure( TEST_IMAGE_PATH );
  UNITY_BEGIN();

  /* RUN_TEST installs Unity's protected frame so an assertion failure is a
   * clean test failure; setUp()/tearDown() own the component + filesystem
   * lifecycle around the scenario. */
  RUN_TEST( run_status_tests );

  rc = UNITY_END();
  return rc;
}