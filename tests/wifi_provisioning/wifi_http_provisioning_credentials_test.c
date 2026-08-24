/*
 * Wi-Fi HTTP provisioning credential submission endpoint unit tests (POSIX).
 *
 * Exercises POST /api/v1/wifi/credentials over a real local HTTP loopback
 * socket against the shared Mongoose process and the mock Wi-Fi HAL:
 *  - visible, hidden, secured, and open-network requests return 202 Accepted,
 *  - the submitted SSID reaches Wi-Fi management (verified via
 *    wifi_mgmt_get_ap_name),
 *  - invalid requests return stable 400, 413, or 415 JSON errors (missing
 *    fields, malformed JSON, embedded NUL, empty/oversized SSID, oversized
 *    password, missing/incorrect Content-Type, and oversized bodies),
 *  - unsupported HTTP methods are rejected with 405,
 *  - every response is JSON with Cache-Control: no-store and never contains a
 *    submitted password or a "password" field.
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
#error "wifi_http_provisioning_credentials_test.c targets POSIX only"
#endif

#define CREDENTIALS_PATH "/api/v1/wifi/credentials"

/* Dedicated littlefs image so this test never touches another test's state. */
#define TEST_IMAGE_PATH "/tmp/wifi_prov_credentials.img"

/* The shared fixture owns the component + filesystem stack around the run. */
void setUp( void )
{
  wifi_provisioning_test_fixture_setup();
}

void tearDown( void )
{
  wifi_provisioning_test_fixture_teardown();
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

/* Open a loopback TCP connection, send an HTTP request (with an optional
 * Content-Type and raw body), read the full reply (the server closes after
 * responding) and return the number of bytes read. When @p body_len is 0 and
 * @p body is NULL no body is sent. */
static int http_request_full( int port, const char * method, const char * path,
                              const char * ctype, const char * body,
                              size_t body_len, char * out, size_t outcap )
{
  struct sockaddr_in addr;
  struct timeval     tv;
  char               head[512];
  char               buf[1024];
  int                fd;
  int                hlen;
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

  if ( body != NULL && ctype != NULL )
  {
    hlen = snprintf( head, sizeof( head ),
                     "%s %s HTTP/1.1\r\nHost: localhost\r\n"
                     "Content-Type: %s\r\nContent-Length: %zu\r\n\r\n",
                     method, path, ctype, body_len );
  }
  else if ( body != NULL )
  {
    hlen = snprintf( head, sizeof( head ),
                     "%s %s HTTP/1.1\r\nHost: localhost\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     method, path, body_len );
  }
  else
  {
    hlen = snprintf( head, sizeof( head ),
                     "%s %s HTTP/1.1\r\nHost: localhost\r\n\r\n",
                     method, path );
  }
  (void) send( fd, head, (int) strlen( head ), 0 );
  if ( body != NULL && body_len > 0 )
  {
    (void) send( fd, body, (int) body_len, 0 );
  }

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

static int http_request( int port, const char * method, const char * path,
                         const char * ctype, const char * body,
                         char * out, size_t outcap )
{
  size_t len = body ? strlen( body ) : 0u;
  return http_request_full( port, method, path, ctype, body, len, out, outcap );
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

static void assert_json_error( const char * resp, const char * error )
{
  char needle[64];
  const char * body = body_of( resp );
  assert_api_headers( resp );
  snprintf( needle, sizeof( needle ), "\"error\":\"%s\"", error );
  TEST_ASSERT_TRUE_MESSAGE( strstr( body, needle ) != NULL, needle );
}

static void assert_no_password( const char * resp, const char * password )
{
  const char * body = body_of( resp );
  TEST_ASSERT_NULL_MESSAGE( strstr( body, password ),
                            "response must not contain a submitted password" );
  TEST_ASSERT_NULL_MESSAGE( strstr( body, "\"password\"" ),
                            "response must not expose a password field" );
}

/* Submit @p body (already JSON) with the given Content-Type and assert the
 * expected HTTP status along with a JSON API shape. */
static void submit( int port, const char * ctype, const char * body,
                    int expected_status, char * resp, size_t respcap )
{
  TEST_ASSERT_TRUE( http_request( port, "POST", CREDENTIALS_PATH, ctype, body,
                                  resp, respcap ) > 0 );
  assert_status_code( resp, expected_status );
}

/* ------------------------------------------------------------------ */
/* Credential submission scenarios.                                     */
/* ------------------------------------------------------------------ */

static void run_credentials_tests( void )
{
  int   http_port = reserve_port( SOCK_STREAM );
  int   dns_port  = reserve_port( SOCK_DGRAM );
  char  http_url[64];
  char  dns_url[64];
  char  resp[8192];
  char  got_ssid[64];
  char  nul_body[]    = "{\"ssid\":\"Net\",\"password\":\"a\0b\"}";
  char  empty_ssid[]  = "{\"ssid\":\"\",\"password\":\"x\"}";
  char  long_ssid[96];
  char  long_pass[128];
  char  big_body[1200];

  static const char k_a40[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";   /* 40 = > SSID max (32) */
  static const char k_p80[] =
    "pppppppppppppppppppppppppppppppppppppppp"
    "pppppppppppppppppppppppppppppppppppppppp";   /* 80 = > password max */

  /* Build an oversized valid-JSON body (> 1024 bytes) deterministically. */
  snprintf( big_body, sizeof( big_body ), "{\"ssid\":\"Net\",\"password\":\"" );
  {
    size_t off = strlen( big_body );                 /* after the opening quote */
    size_t cap = sizeof( big_body ) - off;
    memset( big_body + off, 'p', cap - 2u );         /* fill the password area */
    big_body[off + cap - 2u] = '"';
    big_body[off + cap - 1u] = '}';
    big_body[off + cap]      = '\0';
  }

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

  /* --- valid secured network ----------------------------------------- */
  submit( http_port, "application/json",
          "{\"ssid\":\"HomeNet\",\"password\":\"secret123\"}", 202,
          resp, sizeof( resp ) );
  assert_api_headers( resp );
  assert_no_password( resp, "secret123" );
  /* The submitted SSID must have reached Wi-Fi management. */
  memset( got_ssid, 0, sizeof( got_ssid ) );
  TEST_ASSERT_TRUE( wifi_mgmt_get_ap_name( got_ssid ) );
  TEST_ASSERT_EQUAL_STRING( "HomeNet", got_ssid );

  /* --- valid hidden network ------------------------------------------ */
  submit( http_port, "application/json",
          "{\"ssid\":\"HiddenNet\",\"password\":\"wpa2secret\"}", 202,
          resp, sizeof( resp ) );
  assert_api_headers( resp );
  assert_no_password( resp, "wpa2secret" );

  /* --- valid open network (empty password) --------------------------- */
  submit( http_port, "application/json",
          "{\"ssid\":\"PublicCafe\",\"password\":\"\"}", 202,
          resp, sizeof( resp ) );
  assert_api_headers( resp );
  assert_no_password( resp, "PublicCafe" );

  /* --- valid visible network with long password (within limit) ------- */
  submit( http_port, "application/json",
          "{\"ssid\":\"WiFi5GHz\",\"password\":\"correct horse battery staple\"}", 202,
          resp, sizeof( resp ) );
  assert_api_headers( resp );

  /* --- missing field: no password ------------------------------------ */
  submit( http_port, "application/json", "{\"ssid\":\"HomeNet\"}", 400,
          resp, sizeof( resp ) );
  assert_json_error( resp, "missing_field" );

  /* --- missing field: no ssid ---------------------------------------- */
  submit( http_port, "application/json", "{\"password\":\"secret123\"}", 400,
          resp, sizeof( resp ) );
  assert_json_error( resp, "missing_field" );

  /* --- malformed JSON ------------------------------------------------- */
  submit( http_port, "application/json", "{\"ssid\":", 400,
          resp, sizeof( resp ) );
  assert_json_error( resp, "invalid_json" );

  /* --- embedded NUL data ---------------------------------------------- */
  TEST_ASSERT_TRUE( http_request_full( http_port, "POST", CREDENTIALS_PATH,
                                       "application/json", nul_body,
                                       sizeof( nul_body ) - 1,
                                       resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 400 );
  assert_json_error( resp, "invalid_json" );

  /* --- non-string ssid (numeric) -> missing/type 400 ------------------ */
  submit( http_port, "application/json", "{\"ssid\":123,\"password\":\"x\"}", 400,
          resp, sizeof( resp ) );
  assert_json_error( resp, "missing_field" );

  /* --- empty SSID ------------------------------------------------------ */
  submit( http_port, "application/json", empty_ssid, 400,
          resp, sizeof( resp ) );
  assert_json_error( resp, "invalid_ssid" );

  /* --- too-long SSID (> 32 bytes) -------------------------------------- */
  snprintf( long_ssid, sizeof( long_ssid ),
            "{\"ssid\":\"%s\",\"password\":\"x\"}", k_a40 );
  submit( http_port, "application/json", long_ssid, 400,
          resp, sizeof( resp ) );
  assert_json_error( resp, "invalid_ssid" );

  /* --- too-long password (> 64 bytes) ---------------------------------- */
  snprintf( long_pass, sizeof( long_pass ),
            "{\"ssid\":\"Net\",\"password\":\"%s\"}", k_p80 );
  submit( http_port, "application/json", long_pass, 400,
          resp, sizeof( resp ) );
  assert_json_error( resp, "invalid_password" );

  /* --- missing Content-Type -------------------------------------------- */
  submit( http_port, NULL, "{\"ssid\":\"Net\",\"password\":\"x\"}", 415,
          resp, sizeof( resp ) );
  assert_json_error( resp, "unsupported_media_type" );

  /* --- incorrect Content-Type ------------------------------------------ */
  submit( http_port, "text/plain", "{\"ssid\":\"Net\",\"password\":\"x\"}", 415,
          resp, sizeof( resp ) );
  assert_json_error( resp, "unsupported_media_type" );

  /* --- oversized request body (> 1024 bytes) --------------------------- */
  TEST_ASSERT_TRUE( strlen( big_body ) > 1024u );
  submit( http_port, "application/json", big_body, 413,
          resp, sizeof( resp ) );
  assert_json_error( resp, "payload_too_large" );

  /* --- method/route stability ------------------------------------------ */
  TEST_ASSERT_TRUE( http_request( http_port, "GET", CREDENTIALS_PATH, NULL, NULL,
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
  wifi_provisioning_test_fixture_configure( TEST_IMAGE_PATH );
  UNITY_BEGIN();

  /* RUN_TEST installs Unity's protected frame so an assertion failure is a
   * clean test failure; setUp()/tearDown() own the component + filesystem
   * lifecycle around the scenario. */
  RUN_TEST( run_credentials_tests );

  rc = UNITY_END();
  return rc;
}
