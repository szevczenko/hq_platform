/*
 * Wi-Fi HTTP provisioning captive portal routing unit tests (POSIX).
 *
 * Exercises the captive-portal fallback routing and captive-check handling
 * over a real local HTTP loopback socket against the shared Mongoose process
 * and the mock Wi-Fi HAL:
 *  - every supported OS connectivity probe is answered with the portal:
 *    Android /generate_204, Apple /hotspot-detect.html, Windows /ncsi.txt
 *    and /connecttest.txt,
 *  - the portal root "/" is served directly,
 *  - unknown browser GET routes are redirected to the portal root (302),
 *  - API routes are not redirected and keep serving their JSON payloads,
 *  - an unknown API route stays a 404 (never redirected),
 *  - unsupported methods on portal/probe routes return 405 with Allow: GET,
 *  - HTTPS is not intercepted: a raw TLS handshake gets no portal response,
 *  - portal responses set content type, no-store cache and basic security
 *    headers (nosniff, frame-denial).
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
#error "wifi_http_provisioning_captive_test.c targets POSIX only"
#endif

#define STATUS_PATH "/api/v1/wifi/status"

/* Dedicated littlefs image so this test never touches another test's state. */
#define TEST_IMAGE_PATH "/tmp/wifi_prov_captive.img"

/* The shared fixture owns the component + filesystem stack around the run. */
void setUp( void )
{
  wifi_provisioning_test_fixture_setup();
}

void tearDown( void )
{
  wifi_provisioning_test_fixture_teardown();
}

/* -- string helpers ------------------------------------------------------- */

static char ascii_lower( char c )
{
  if ( c >= 'A' && c <= 'Z' ) return ( char ) ( c + ( 'a' - 'A' ) );
  return c;
}

static bool contains_ci( const char * haystack, const char * needle )
{
  size_t hay_len = strlen( haystack );
  size_t ned_len = strlen( needle );
  size_t i;

  if ( ned_len == 0 ) return true;
  if ( ned_len > hay_len ) return false;
  for ( i = 0; i + ned_len <= hay_len; i++ )
  {
    size_t j;
    for ( j = 0; j < ned_len; j++ )
    {
      if ( ascii_lower( haystack[i + j] ) != ascii_lower( needle[j] ) ) break;
    }
    if ( j == ned_len ) return true;
  }
  return false;
}

/* Return the value of the first header whose name (up to ':') equals @p name,
 * case-insensitively, trimmed of leading whitespace, or NULL when absent. */
static const char * header_of( const char * resp, const char * name )
{
  const char * end  = strstr( resp, "\r\n\r\n" );
  const char * block;
  size_t       blen;
  size_t       name_len;
  size_t       i;

  if ( !end ) return NULL;
  block = resp;
  blen  = ( size_t ) ( end - resp );
  name_len = strlen( name );
  if ( name_len == 0 ) return NULL;

  for ( i = 0; i + name_len + 1 <= blen; i++ )
  {
    size_t j;
    for ( j = 0; j < name_len; j++ )
    {
      if ( ascii_lower( block[i + j] ) != ascii_lower( name[j] ) ) break;
    }
    if ( j == name_len && block[i + j] == ':' )
    {
      const char * v = &block[i + j + 1];
      while ( *v == ' ' || *v == '\t' ) v++;
      return v;
    }
  }
  return NULL;
}

/* -- socket helpers ----------------------------------------------------- */

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

/* Send @p raw_len bytes verbatim (no HTTP framing) and read whatever the
 * server sends back. Returns the number of bytes read. */
static int raw_exchange( int port, const unsigned char * raw, size_t raw_len,
                         char * out, size_t outcap )
{
  struct sockaddr_in addr;
  struct timeval     tv;
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

  (void) send( fd, raw, (int) raw_len, 0 );

  tv.tv_sec  = 1;
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

/* -- assertions --------------------------------------------------------- */

static void assert_status_code( const char * resp, int expected )
{
  TEST_ASSERT_EQUAL_INT( expected, parse_status_code( resp ) );
}

/* Assert a 200 HTML captive-portal response with the standard headers. */
static void assert_portal_html_response( const char * resp )
{
  const char * body;

  assert_status_code( resp, 200 );
  TEST_ASSERT_TRUE_MESSAGE( contains_ci( resp, "Content-Type: text/html" ),
                            "portal response must set text/html" );
  TEST_ASSERT_TRUE_MESSAGE( contains_ci( resp, "Cache-Control: no-store" ),
                            "portal response must set Cache-Control: no-store" );
  TEST_ASSERT_TRUE_MESSAGE( contains_ci( resp, "X-Content-Type-Options: nosniff" ),
                            "portal response must set nosniff" );
  TEST_ASSERT_TRUE_MESSAGE( contains_ci( resp, "X-Frame-Options: DENY" ),
                            "portal response must deny frame embedding" );
  body = body_of( resp );
  TEST_ASSERT_TRUE_MESSAGE( strstr( body, "HQ Wi-Fi Provisioning Portal" ) != NULL,
                            "portal response must carry the packed application" );
}

/* Assert a 302 fallback redirect to the portal root. */
static void assert_redirect_to_root( const char * resp )
{
  const char * loc;

  assert_status_code( resp, 302 );
  loc = header_of( resp, "Location" );
  TEST_ASSERT_NOT_NULL_MESSAGE( loc, "redirect must carry a Location header" );
  if ( loc )
  {
    TEST_ASSERT_TRUE_MESSAGE( loc[0] == '/',
                              "redirect must point at the portal root" );
  }
  TEST_ASSERT_TRUE_MESSAGE( contains_ci( resp, "Cache-Control: no-store" ),
                            "redirect must set Cache-Control: no-store" );
}

static void assert_method_not_allowed( const char * resp )
{
  assert_status_code( resp, 405 );
  TEST_ASSERT_TRUE_MESSAGE( contains_ci( resp, "Allow: GET" ),
                            "405 must advertise Allow: GET" );
}

/* ------------------------------------------------------------------ */
/* Captive portal routing scenarios.                                    */
/* ------------------------------------------------------------------ */

static void run_captive_tests( void )
{
  int   http_port = reserve_port( SOCK_STREAM );
  int   dns_port  = reserve_port( SOCK_DGRAM );
  char  http_url[64];
  char  dns_url[64];
  char  resp[4096];

  TEST_ASSERT_TRUE( http_port > 0 );
  TEST_ASSERT_TRUE( dns_port > 0 );
  snprintf( http_url, sizeof( http_url ), "http://127.0.0.1:%d", http_port );
  snprintf( dns_url, sizeof( dns_url ), "udp://127.0.0.1:%d", dns_port );

  /* --- start provisioning (RUNNING lifecycle) ----------------------- */
  TEST_ASSERT_TRUE( wifi_http_provisioning_set_http_url( http_url ) );
  TEST_ASSERT_TRUE( wifi_http_provisioning_set_dns_url( dns_url ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_http_provisioning_start(),
                            "provisioning must start with Wi-Fi management running" );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING, wifi_http_provisioning_get_state() );

  /* --- each supported captive probe is served the portal ------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "GET", "/generate_204",
                                  resp, sizeof( resp ) ) > 0 );
  assert_portal_html_response( resp );

  TEST_ASSERT_TRUE( http_request( http_port, "GET", "/hotspot-detect.html",
                                  resp, sizeof( resp ) ) > 0 );
  assert_portal_html_response( resp );

  TEST_ASSERT_TRUE( http_request( http_port, "GET", "/ncsi.txt",
                                  resp, sizeof( resp ) ) > 0 );
  assert_portal_html_response( resp );

  TEST_ASSERT_TRUE( http_request( http_port, "GET", "/connecttest.txt",
                                  resp, sizeof( resp ) ) > 0 );
  assert_portal_html_response( resp );

  /* --- the portal root is served directly ---------------------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "GET", "/",
                                  resp, sizeof( resp ) ) > 0 );
  assert_portal_html_response( resp );

  TEST_ASSERT_TRUE( http_request( http_port, "GET", "/app.css",
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 200 );
  TEST_ASSERT_TRUE( contains_ci( resp, "Content-Type: text/css" ) );

  TEST_ASSERT_TRUE( http_request( http_port, "GET", "/app.js",
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 200 );
  TEST_ASSERT_TRUE( contains_ci( resp, "Content-Type: text/javascript" ) );

  /* --- unknown browser GET route redirects to the portal root -------- */
  TEST_ASSERT_TRUE( http_request( http_port, "GET", "/favicon.ico",
                                  resp, sizeof( resp ) ) > 0 );
  assert_redirect_to_root( resp );

  TEST_ASSERT_TRUE( http_request( http_port, "GET", "/some/random/deep/path",
                                  resp, sizeof( resp ) ) > 0 );
  assert_redirect_to_root( resp );

  /* --- a known API route is not redirected --------------------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "GET", STATUS_PATH,
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 200 );
  TEST_ASSERT_TRUE_MESSAGE( contains_ci( resp, "Content-Type: application/json" ),
                             "API route must keep serving JSON" );

  /* --- an unknown API route is a 404, never redirected --------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "GET",
                                  "/api/v1/wifi/does_not_exist",
                                  resp, sizeof( resp ) ) > 0 );
  assert_status_code( resp, 404 );
  TEST_ASSERT_NULL_MESSAGE( header_of( resp, "Location" ),
                            "unknown API route must not be redirected" );

  /* --- unsupported methods return 405 with Allow: GET ---------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "POST", "/generate_204",
                                  resp, sizeof( resp ) ) > 0 );
  assert_method_not_allowed( resp );

  TEST_ASSERT_TRUE( http_request( http_port, "PUT", "/some/path",
                                  resp, sizeof( resp ) ) > 0 );
  assert_method_not_allowed( resp );

  /* --- HTTPS is not intercepted: a raw TLS handshake gets no portal -- */
  {
    unsigned char tls_hello[16] = {
      0x16, 0x03, 0x01, 0x00, 0x0b,
      0x01, 0x00, 0x00, 0x07, 0x03, 0x03,
      0x00, 0x00, 0x00, 0x00, 0x00
    };
    char tls_resp[1024];

    (void) raw_exchange( http_port, tls_hello, sizeof( tls_hello ),
                         tls_resp, sizeof( tls_resp ) );
    TEST_ASSERT_FALSE_MESSAGE( contains_ci( tls_resp, "HTTP/" ),
                               "HTTPS probe must not be intercepted over HTTP" );
    TEST_ASSERT_FALSE_MESSAGE( strstr( tls_resp, "HQ Wi-Fi Provisioning Portal" ) != NULL,
                               "HTTPS probe must not receive the portal page" );
  }
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
  RUN_TEST( run_captive_tests );

  rc = UNITY_END();
  return rc;
}