/*
 * Wi-Fi HTTP provisioning scan endpoint unit tests (POSIX).
 *
 * Exercises POST /api/v1/wifi/scans and GET /api/v1/wifi/networks over a real
 * local HTTP loopback socket against the shared Mongoose process and the mock
 * Wi-Fi HAL:
 *  - /api/v1/wifi/networks reports an empty snapshot before any scan,
 *  - POST /api/v1/wifi/scans returns 202 Accepted and starts a background
 *    scan,
 *  - while a scan is active, /api/v1/wifi/networks reports "scanning" and the
 *    scan generation is stable,
 *  - repeated scan requests are debounced: they do not start a duplicate HAL
 *    scan (verified through the mock HAL scan counter),
 *  - after SCAN_DONE, /api/v1/wifi/networks reports "idle", the generation
 *    advanced by one, and the returned network objects match the Wi-Fi
 *    snapshot (SSID, channel, RSSI, authentication mode),
 *  - a scan request that cannot be queued (Wi-Fi stopped) is accepted but
 *    reports "failed",
 *  - wrong HTTP methods are rejected with 405.
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

#ifdef ESP_PLATFORM
#error "wifi_http_provisioning_scan_test.c targets POSIX only"
#endif

#define SCANS_PATH    "/api/v1/wifi/scans"
#define NETWORKS_PATH "/api/v1/wifi/networks"

void setUp( void )
{
}

void tearDown( void )
{
}

/* -- small predicates used by the wait helpers ---------------------------- */

static bool pred_is_scan_completed( void )
{
  return !wifi_mgmt_is_scan_active() &&
         wifi_mgmt_get_scan_generation() == 1u;
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

static void assert_json_has_num( const char * body, const char * key, int value )
{
  char needle[64];
  snprintf( needle, sizeof( needle ), "\"%s\":%d", key, value );
  TEST_ASSERT_TRUE_MESSAGE( strstr( body, needle ) != NULL, needle );
}

/* ------------------------------------------------------------------ */
/* Scan endpoint scenarios.                                              */
/* ------------------------------------------------------------------ */

static void run_scan_tests( void )
{
  int   http_port = reserve_port( SOCK_STREAM );
  int   dns_port  = reserve_port( SOCK_DGRAM );
  char  http_url[64];
  char  dns_url[64];
  char  resp[4096];
  const char * body;
  const wifi_hal_mock_state_t * mock;
  wifi_hal_ap_record_t mock_aps[2];

  TEST_ASSERT_TRUE( http_port > 0 );
  TEST_ASSERT_TRUE( dns_port > 0 );
  snprintf( http_url, sizeof( http_url ), "http://127.0.0.1:%d", http_port );
  snprintf( dns_url, sizeof( dns_url ), "udp://127.0.0.1:%d", dns_port );

  /* --- start provisioning ------------------------------------------- */
  TEST_ASSERT_TRUE( wifi_http_provisioning_set_http_url( http_url ) );
  TEST_ASSERT_TRUE( wifi_http_provisioning_set_dns_url( dns_url ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_http_provisioning_start(),
                            "provisioning must start with Wi-Fi management running" );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING, wifi_http_provisioning_get_state() );

  /* --- initial empty results ----------------------------------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "GET", NETWORKS_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 200 );
  assert_api_headers( resp );
  body = body_of( resp );
  assert_json_has( body, "state", "idle" );
  assert_json_has_num( body, "generation", 0 );
  TEST_ASSERT_TRUE_MESSAGE( strstr( body, "\"networks\":[]" ) != NULL,
                            "initial snapshot must be empty" );

  /* --- start a scan (held so the in-flight window is observable) ----- */
  wifi_hal_mock_set_scan_done_hold( true );
  TEST_ASSERT_TRUE( http_request( http_port, "POST", SCANS_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 202 );
  assert_api_headers( resp );
  body = body_of( resp );
  assert_json_has( body, "state", "scanning" );
  assert_json_has_num( body, "generation", 0 );

  mock = wifi_hal_mock_get_state();
  TEST_ASSERT_EQUAL_MESSAGE( 1u, mock->scan_start_count,
                             "the first scan request starts one HAL scan" );

  /* While scanning: networks report scanning with the same generation. */
  TEST_ASSERT_TRUE( http_request( http_port, "GET", NETWORKS_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 200 );
  assert_api_headers( resp );
  body = body_of( resp );
  assert_json_has( body, "state", "scanning" );
  assert_json_has_num( body, "generation", 0 );
  TEST_ASSERT_TRUE_MESSAGE( strstr( body, "\"networks\":[]" ) != NULL,
                            "no records are published until the scan completes" );

  /* --- duplicate requests are debounced (no duplicate HAL scan) ------ */
  TEST_ASSERT_TRUE( http_request( http_port, "POST", SCANS_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 202 );
  assert_api_headers( resp );
  body = body_of( resp );
  assert_json_has( body, "state", "scanning" );
  mock = wifi_hal_mock_get_state();
  TEST_ASSERT_EQUAL_MESSAGE( 1u, mock->scan_start_count,
                             "repeated scan requests must not start duplicate HAL scans" );

  /* --- complete the scan with a known AP snapshot -------------------- */
  memset( mock_aps, 0, sizeof( mock_aps ) );
  strncpy( mock_aps[0].ssid, "NetA", sizeof( mock_aps[0].ssid ) - 1 );
  mock_aps[0].channel  = 1;
  mock_aps[0].rssi     = -41;
  mock_aps[0].authmode = 3;
  strncpy( mock_aps[1].ssid, "NetB", sizeof( mock_aps[1].ssid ) - 1 );
  mock_aps[1].channel  = 6;
  mock_aps[1].rssi     = -55;
  mock_aps[1].authmode = 4;
  wifi_hal_mock_set_scan_list( mock_aps, 2 );
  wifi_hal_mock_set_scan_done_hold( false );
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_SCAN_DONE, NULL );

  TEST_ASSERT_TRUE_MESSAGE( wait_bool( pred_is_scan_completed, 2000 ),
                            "SCAN_DONE must complete the scan and advance the generation" );

  TEST_ASSERT_TRUE( http_request( http_port, "GET", NETWORKS_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 200 );
  assert_api_headers( resp );
  body = body_of( resp );
  assert_json_has( body, "state", "idle" );
  assert_json_has_num( body, "generation", 1 );
  /* The returned JSON must match the Wi-Fi snapshot records exactly. */
  assert_json_has( body, "ssid", "NetA" );
  assert_json_has_num( body, "channel", 1 );
  assert_json_has_num( body, "rssi", -41 );
  assert_json_has_num( body, "auth", 3 );
  assert_json_has( body, "ssid", "NetB" );
  assert_json_has_num( body, "channel", 6 );
  assert_json_has_num( body, "rssi", -55 );
  assert_json_has_num( body, "auth", 4 );

  /* --- scan failure: request accepted but cannot queue a scan -------- */
  wifi_mgmt_stop();
  TEST_ASSERT_TRUE_MESSAGE( wait_bool( pred_is_wifi_stopped, 4000 ),
                            "Wi-Fi management must actually stop" );
  TEST_ASSERT_TRUE( http_request( http_port, "POST", SCANS_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 202 );
  assert_api_headers( resp );
  body = body_of( resp );
  assert_json_has( body, "state", "failed" );

  /* --- method/route stability ---------------------------------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "GET", SCANS_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 405 );
  assert_api_headers( resp );

  TEST_ASSERT_TRUE( http_request( http_port, "POST", NETWORKS_PATH,
                                  resp, sizeof( resp ) ) > 0 );
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
  UNITY_BEGIN();

  (void) remove( "wifi_ap.json" );   /* Avoid a stale auto-connect from a prior run. */

  wifi_hal_mock_reset();
  wifi_hal_mock_set_start_result( OSAL_SUCCESS );
  wifi_hal_mock_set_connect_result( OSAL_SUCCESS );

  wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLI_SER );
  wifi_mgmt_init();
  wifi_mgmt_start();
  {
    int elapsed = 0;
    while ( !wifi_mgmt_is_running() && elapsed < 3000 )
    {
      (void) osal_task_delay_ms( 10 );
      elapsed += 10;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_running(),
                            "Wi-Fi management must be running before provisioning" );

  MongooseProcess_Init();
  TEST_ASSERT_TRUE_MESSAGE( MongooseProcess_IsRunning(),
                            "Mongoose process must be running" );

  run_scan_tests();

  /* --- teardown ------------------------------------------------------- */
  wifi_http_provisioning_stop();
  MongooseProcess_Deinit();

  rc = UNITY_END();
  return rc;
}