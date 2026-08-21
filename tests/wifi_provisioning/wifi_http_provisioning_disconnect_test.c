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
#error "wifi_http_provisioning_disconnect_test.c targets POSIX only"
#endif

#define DISCONNECT_PATH  "/api/v1/wifi/connection"
#define STATUS_PATH      "/api/v1/wifi/status"

void setUp( void )
{
}

void tearDown( void )
{
}

/* -- small predicates used by the wait helpers ---------------------------- */

static bool pred_is_connected( void )
{
  return wifi_mgmt_is_connected();
}

static bool pred_is_disconnected( void )
{
  return !wifi_mgmt_is_connected() && wifi_mgmt_is_running();
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

/* ------------------------------------------------------------------ */
/* Disconnect endpoint scenarios.                                       */
/* ------------------------------------------------------------------ */

static void run_disconnect_tests( void )
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

  /* --- already-disconnected: accepted, no credentials, portal reachable -- */
  TEST_ASSERT_TRUE( http_request( http_port, "DELETE", DISCONNECT_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 202 );
  assert_api_headers( resp );
  TEST_ASSERT_TRUE_MESSAGE( strstr( body_of( resp ), "\"state\":\"accepted\"" ) != NULL,
                            "disconnected delete must report accepted" );
  assert_no_credential_data( resp );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING, wifi_http_provisioning_get_state() );

  /* --- connect a station so the real disconnect path is observable ------- */
  TEST_ASSERT_TRUE( wifi_mgmt_set_ap_name( "testnet", (size_t) 7 ) );
  TEST_ASSERT_TRUE( wifi_mgmt_set_password( "pw", (size_t) 2 ) );
  TEST_ASSERT_TRUE( wifi_mgmt_connect() );
  memset( &ip_info, 0, sizeof( ip_info ) );
  strncpy( ip_info.ip,      "10.170.0.50", sizeof( ip_info.ip ) - 1 );
  strncpy( ip_info.netmask, "255.255.255.0", sizeof( ip_info.netmask ) - 1 );
  strncpy( ip_info.gw,      "10.170.0.1", sizeof( ip_info.gw ) - 1 );
  wifi_hal_mock_set_ip_info( &ip_info );
  memset( &evt, 0, sizeof( evt ) );
  evt.ip_info = ip_info;
  wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );

  TEST_ASSERT_TRUE_MESSAGE( wait_bool( pred_is_connected, 4000 ),
                            "GOT_IP event must complete the station connection" );
  TEST_ASSERT_TRUE_MESSAGE( wait_for_wifi_state( http_port, "connected", 4000 ),
                            "status must report connected before disconnect" );

  /* --- connected: delete requests disconnect, portal stays up ------------ */
  TEST_ASSERT_TRUE( http_request( http_port, "DELETE", DISCONNECT_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 202 );
  assert_api_headers( resp );
  TEST_ASSERT_TRUE_MESSAGE( strstr( body_of( resp ), "\"state\":\"accepted\"" ) != NULL,
                            "connected delete must report accepted" );
  assert_no_credential_data( resp );

  /* The disconnect is asynchronous: wait until the station link drops. */
  TEST_ASSERT_TRUE_MESSAGE( wait_bool( pred_is_disconnected, 4000 ),
                            "accepted delete must take the station to disconnected" );

  /* Provisioning must remain reachable and RUNNING after the disconnect. */
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING, wifi_http_provisioning_get_state() );
  TEST_ASSERT_TRUE_MESSAGE( wait_for_wifi_state( http_port, "disconnected", 4000 ),
                            "status must remain reachable and report disconnected" );

  /* --- already-disconnected again: idempotent, still accepted ------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "DELETE", DISCONNECT_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 202 );
  assert_api_headers( resp );
  TEST_ASSERT_TRUE_MESSAGE( strstr( body_of( resp ), "\"state\":\"accepted\"" ) != NULL,
                            "second delete while disconnected must stay accepted" );
  assert_no_credential_data( resp );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING, wifi_http_provisioning_get_state() );

  /* --- disconnect cannot be requested (Wi-Fi stopped): stable 503 --------- */
  wifi_mgmt_stop();
  TEST_ASSERT_TRUE_MESSAGE( wait_bool( pred_is_wifi_stopped, 4000 ),
                            "Wi-Fi management must actually stop" );
  TEST_ASSERT_TRUE( http_request( http_port, "DELETE", DISCONNECT_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 503 );
  assert_api_headers( resp );
  body = body_of( resp );
  TEST_ASSERT_TRUE_MESSAGE( strstr( body, "\"error\":\"service_unavailable\"" ) != NULL,
                            "unavailable disconnect must report service_unavailable" );
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

  run_disconnect_tests();

  /* --- teardown ------------------------------------------------------- */
  wifi_http_provisioning_stop();
  MongooseProcess_Deinit();

  rc = UNITY_END();
  return rc;
}