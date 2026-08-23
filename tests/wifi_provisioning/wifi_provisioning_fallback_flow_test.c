/*
 * TASK-131 - Wi-Fi provisioning automatic fallback end-to-end test (POSIX).
 *
 * Drives the complete provisioning flow with the real Mongoose process, the
 * real Wi-Fi management layer (against the mock Wi-Fi HAL) and the real
 * automatic fallback controller. Covers the three host-side behaviors
 * required for the ESP provisioning example:
 *
 *  1. DNS responses           - a raw unicast A query against the captive DNS
 *                               listener is answered with the portal address;
 *  2. credential persistence  - a credential submitted through the portal is
 *                               written to wifi_ap.json and can be loaded back
 *                               from disk afterwards;
 *  3. STA-only transition     - after the posted credential connects (GOT_IP)
 *                               the controller keeps HTTP/DNS up for the
 *                               success grace period, then stops the portal
 *                               and requests the STA-only mode, so the
 *                               temporary AP is retired.
 *
 * The scenario mirrors the automatic fallback of the
 * examples/esp/wifi_provisioning_demo example, only with non-privileged
 * loopback high ports instead of ports 80/53. No physical radio is needed.
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
#error "wifi_provisioning_fallback_flow_test.c targets POSIX only"
#endif

#define CREDENTIALS_PATH "/api/v1/wifi/credentials"
#define STATUS_PATH      "/api/v1/wifi/status"

/* Network submitted through the portal. */
#define TEST_SSID     "properly_ap"
#define TEST_PASSWORD "12345678"
/* Captive DNS A-record answer configured by the portal. */
#define DNS_ANSWER "10.10.0.1"
#define DNS_NAME   "provision.local"

/* Short success grace so the STA-only transition is observable in-test. */
#define TEST_GRACE_MS 700u

/* Dedicated littlefs image so the test never touches another test's state. */
#define TEST_IMAGE_PATH  "/tmp/wifi_prov_fallback_flow.img"
#define TEST_MOUNT_POINT "/"

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
}

void tearDown( void )
{
}

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

static bool pred_is_connecting( void )
{
  return wifi_mgmt_trying_connect();
}

static bool pred_is_connected( void )
{
  return wifi_mgmt_is_connected();
}

static bool wait_bool( bool ( *pred )( void ), int timeout_ms )
{
  int elapsed = 0;
  while ( elapsed < timeout_ms )
  {
    if ( pred() ) return true;
    ( void ) osal_task_delay_ms( 20 );
    elapsed += 20;
  }
  return false;
}

/* Reserve a free loopback port by binding to port 0, then close it. */
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

/* Issue an HTTP request over a loopback socket and return bytes received. */
static int http_request( int port, const char * method, const char * path,
                         const char * ctype, const char * body,
                         char * out, size_t outcap )
{
  struct sockaddr_in addr;
  struct timeval     tv;
  char               req[512];
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

  if ( body != NULL && ctype != NULL )
  {
    int hlen = snprintf( req, sizeof( req ),
                         "%s %s HTTP/1.1\r\nHost: localhost\r\n"
                         "Content-Type: %s\r\nContent-Length: %zu\r\n\r\n",
                         method, path, ctype, strlen( body ) );
    ( void ) send( fd, req, hlen, 0 );
    ( void ) send( fd, body, ( int ) strlen( body ), 0 );
  }
  else
  {
    int hlen = snprintf( req, sizeof( req ),
                         "%s %s HTTP/1.1\r\nHost: localhost\r\n\r\n",
                         method, path );
    ( void ) send( fd, req, hlen, 0 );
  }

  tv.tv_sec  = 2;
  tv.tv_usec = 0;
  ( void ) setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );

  while ( total < outcap )
  {
    int n = ( int ) recv( fd, buf, ( int ) sizeof( buf ), 0 );
    if ( n <= 0 ) break;
    if ( total + ( size_t ) n > outcap ) n = ( int ) ( outcap - total );
    if ( n <= 0 ) break;
    memcpy( out + total, buf, ( size_t ) n );
    total += ( size_t ) n;
  }
  close( fd );
  return ( int ) total;
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

/* Encode a DNS question label sequence into buf; returns bytes written. */
static size_t put_qname( uint8_t * buf, const char * name )
{
  size_t p = 0, start = 0;
  while ( name[start] != '\0' )
  {
    size_t end = start;
    while ( name[end] != '\0' && name[end] != '.' ) end++;
    buf[p++] = ( uint8_t ) ( end - start );
    for ( size_t k = start; k < end; k++ ) buf[p++] = ( uint8_t ) name[k];
    if ( name[end] == '\0' ) break;
    start = end + 1;
  }
  buf[p++] = 0; /* root label */
  return p;
}

/* Send a single-question A query to 127.0.0.1:port and copy the answer's
 * IPv4 address into out_ip. Returns true on a positive answer. */
static bool dns_query_a( int port, const char * name, char * out_ip, size_t cap )
{
  struct sockaddr_in dst;
  struct timeval     tv;
  uint8_t            q[512];
  uint8_t            r[512];
  size_t             qlen;
  int                fd;
  int                n;
  socklen_t          srclen;

  memset( q, 0, sizeof( q ) );
  q[0] = 0x12;
  q[1] = 0x34; /* transaction id */
  q[2] = 0x01;
  q[3] = 0x00; /* RD set, standard query */
  q[5] = 0x01; /* QDCOUNT = 1 */
  qlen = 12 + put_qname( q + 12, name );
  q[qlen++] = 0;
  q[qlen++] = 1;  /* type A */
  q[qlen++] = 0;
  q[qlen++] = 1;  /* class IN */

  fd = ( int ) socket( AF_INET, SOCK_DGRAM, 0 );
  if ( fd < 0 ) return false;

  memset( &dst, 0, sizeof( dst ) );
  dst.sin_family      = AF_INET;
  dst.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  dst.sin_port        = htons( ( uint16_t ) port );

  tv.tv_sec  = 2;
  tv.tv_usec = 0;
  ( void ) setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );

  ( void ) sendto( fd, q, qlen, 0, ( struct sockaddr * ) &dst, sizeof( dst ) );

  srclen = sizeof( dst );
  n      = ( int ) recvfrom( fd, r, sizeof( r ), 0,
                             ( struct sockaddr * ) &dst, &srclen );
  close( fd );
  if ( n < 16 ) return false;

  /* QR must be set and at least one answer present. */
  if ( !( r[2] & 0x80 ) || ( ( r[6] << 8 ) | r[7] ) < 1 ) return false;

  /* The A record payload (RDLENGTH 4) is the trailing 4 bytes. */
  if ( snprintf( out_ip, cap, "%u.%u.%u.%u",
                 r[n - 4], r[n  - 3], r[n - 2], r[n - 1] ) >= ( int ) cap )
  {
    return false;
  }
  return true;
}

/* Probe whether the HTTP listener on `port` still accepts connections. */
static bool http_port_accepts( int port )
{
  struct sockaddr_in addr;
  struct timeval     tv;
  int                fd;
  int                rc;

  fd = ( int ) socket( AF_INET, SOCK_STREAM, 0 );
  if ( fd < 0 ) return false;
  memset( &addr, 0, sizeof( addr ) );
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  addr.sin_port        = htons( ( uint16_t ) port );

  tv.tv_sec  = 0;
  tv.tv_usec = 300000;
  ( void ) setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );
  ( void ) setsockopt( fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );

  rc = connect( fd, ( struct sockaddr * ) &addr, sizeof( addr ) );
  close( fd );
  return rc == 0;
}

/* ------------------------------------------------------------------ */
/*  The complete automatic-fallback flow                               */
/* ------------------------------------------------------------------ */

static void run_fallback_flow( void )
{
  char   http_url[64];
  char   dns_url[64];
  char   resp[2048];
  char   ip[16];
  int    http_port = reserve_port( SOCK_STREAM );
  int    dns_port  = reserve_port( SOCK_DGRAM );
  bool   ok;

  TEST_ASSERT_TRUE( http_port > 0 );
  TEST_ASSERT_TRUE( dns_port > 0 );
  snprintf( http_url, sizeof( http_url ), "http://127.0.0.1:%d", http_port );
  snprintf( dns_url, sizeof( dns_url ), "udp://127.0.0.1:%d", dns_port );

  /* --- 1. automatic fallback without saved credentials -----------------
   * The controller starts the provisioning application immediately because
   * wifi_ap.json does not exist (fresh device). */
  TEST_ASSERT_TRUE( wifi_http_provisioning_set_http_url( http_url ) );
  TEST_ASSERT_TRUE( wifi_http_provisioning_set_dns_url( dns_url ) );
  TEST_ASSERT_TRUE_MESSAGE( wifi_provisioning_controller_init(),
                            "controller init must start provisioning without credentials" );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                         wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );

  /* --- 2. DNS responses ------------------------------------------------
   * A raw unicast A query is answered with the portal address. */
  TEST_ASSERT_TRUE_MESSAGE( dns_query_a( dns_port, DNS_NAME, ip, sizeof( ip ) ),
                            "captive DNS must answer an A query" );
  TEST_ASSERT_EQUAL_STRING( DNS_ANSWER, ip );
  printf( "PASS: captive DNS answered %s -> %s\n", DNS_NAME, ip );

  /* --- 3. portal status ------------------------------------------------ */
  TEST_ASSERT_TRUE( http_request( http_port, "GET", STATUS_PATH, NULL, NULL,
                                  resp, sizeof( resp ) ) > 0 );
  TEST_ASSERT_TRUE( parse_status_code( resp ) == 200 );

  /* --- 4. submit credentials through the portal ------------------------ */
  TEST_ASSERT_TRUE( http_request( http_port, "POST", CREDENTIALS_PATH,
                                  "application/json",
                                  "{\"ssid\":\"" TEST_SSID
                                  "\",\"password\":\"" TEST_PASSWORD "\"}",
                                  resp, sizeof( resp ) ) > 0 );
  TEST_ASSERT_TRUE( parse_status_code( resp ) == 202 );

  /* --- 5. connect completes (GOT_IP) ----------------------------------- */
  TEST_ASSERT_TRUE_MESSAGE( wait_bool( pred_is_connecting, 2000 ),
                            "submitted credentials must reach the connecting window" );
  {
    wifi_hal_ip_info_t    ip_info;
    wifi_hal_event_data_t evt;
    memset( &ip_info, 0, sizeof( ip_info ) );
    strncpy( ip_info.ip,      "10.170.0.50", sizeof( ip_info.ip ) - 1 );
    strncpy( ip_info.netmask, "255.255.255.0", sizeof( ip_info.netmask ) - 1 );
    strncpy( ip_info.gw,      "10.170.0.1", sizeof( ip_info.gw ) - 1 );
    wifi_hal_mock_set_ip_info( &ip_info );
    memset( &evt, 0, sizeof( evt ) );
    evt.ip_info = ip_info;
    wifi_hal_mock_inject_event( WIFI_HAL_EVT_STA_GOT_IP, &evt );
  }

  TEST_ASSERT_TRUE_MESSAGE( wait_bool( pred_is_connected, 2000 ),
                            "GOT_IP must complete the station connection" );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_GRACE,
                         wifi_provisioning_controller_get_state() );
  printf( "PASS: station connected, controller entered grace period\n" );

  /* --- 6. credential persistence ---------------------------------------
   * The submitted credential is saved to wifi_ap.json and can be loaded back
   * from disk, proving it survives a reboot. */
  {
    wifi_config_list_t  list;
    wifi_config_entry_t entry = { 0 };
    memset( &list, 0, sizeof( list ) );
    ok = ( wifi_config_load( &list ) == OSAL_SUCCESS ) &&
         ( list.count > 0 ) &&
         wifi_config_get_by_nb( &list, list.last_use, &entry ) &&
         ( strcmp( entry.ssid, TEST_SSID ) == 0 );
    TEST_ASSERT_TRUE_MESSAGE( ok, "wifi_ap.json must persist the submitted credential" );
    printf( "PASS: credential persisted to wifi_ap.json (ssid=%s, nb=%u)\n",
            entry.ssid, ( unsigned ) list.last_use );
  }

  /* --- 7. STA-only transition after the grace period --------------------
   * The controller stops the portal and requests STA-only mode, so the
   * temporary access point is retired only after the listeners shut down. */
  {
    int elapsed = 0;
    ok = false;
    while ( elapsed < 5000 )
    {
      wifi_hal_mock_state_t mock = { 0 };
      ( void ) wifi_hal_mock_get_state( &mock );
      if ( wifi_http_provisioning_get_state() == WIFI_PROVISIONING_STOPPED &&
           mock.mode == WIFI_HAL_MODE_STA )
      {
        ok = true;
        break;
      }
      ( void ) osal_task_delay_ms( 20 );
      elapsed += 20;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE( ok,
                            "grace expiry must stop the portal and switch to STA-only" );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_ONLINE,
                         wifi_provisioning_controller_get_state() );
  TEST_ASSERT_FALSE_MESSAGE( http_port_accepts( http_port ),
                             "HTTP listener must be released after the transition" );
  printf( "PASS: portal stopped, HAL mode = STA (STA-only transition)\n" );
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

  setup_fs();   /* Fresh device: no saved credentials. */

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
      ( void ) osal_task_delay_ms( 10 );
      elapsed += 10;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_running(),
                            "Wi-Fi management must be running before provisioning" );

  MongooseProcess_Init();
  TEST_ASSERT_TRUE_MESSAGE( MongooseProcess_IsRunning(),
                            "Mongoose process must be running" );

  /* Short success grace so the STA-only transition is observable in-test. */
  wifi_provisioning_controller_set_success_grace_ms( TEST_GRACE_MS );

  run_fallback_flow();

  /* --- teardown in reverse ownership order ----------------------------- */
  ( void ) wifi_http_provisioning_stop();
  wifi_provisioning_controller_deinit();
  wifi_mgmt_stop();
  MongooseProcess_Deinit();

  cleanup_fs();

  rc = UNITY_END();
  return rc;
}