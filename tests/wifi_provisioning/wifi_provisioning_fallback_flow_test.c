/*
 * TASK-141 - Wi-Fi provisioning automatic fallback end-to-end test (POSIX).
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
 *                               and requests the STA-only mode.
 *
 * The scenario mirrors the automatic fallback of the
 * examples/esp/wifi_provisioning_demo example, only with non-privileged
 * loopback high ports instead of ports 80/53. No physical radio is needed.
 *
 * TASK-141 makes the scenario event-driven instead of boolean-poll driven
 * (builds on the TASK-138 controller grace synchronization and the TASK-139
 * HAL snapshot conversion):
 *  - each scenario runs through RUN_TEST() so Unity installs its setjmp frame
 *    and a failing assertion reports a test failure instead of a SEGFAULT,
 *  - ownership setup and reverse-order teardown live in the guarded
 *    setUp()/tearDown() fixture, so cleanup always runs after an assertion,
 *  - Wi-Fi startup waits on wifi_mgmt_wait_ready() rather than polling
 *    wifi_mgmt_is_running(),
 *  - the success grace value is installed through the corrected controller
 *    override path before init so it cannot be replaced by the Kconfig
 *    default, and the elapsed time between GRACE and the transition is
 *    asserted to prove the 700 ms value was actually armed,
 *  - the pre-GOT_IP wait uses the mock connect-call completion channel, and
 *    the persisted-configuration check waits for the typed CONNECTED event,
 *  - the former polling loop is replaced by the authoritative typed
 *    MODE_CHANGED completion; controller state, HTTP/DNS closure, and HAL
 *    mode are then asserted from snapshots,
 *  - teardown stops the controller before Wi-Fi, stops/deinitializes Wi-Fi
 *    before Mongoose, and cleans the filesystem only after every writer that
 *    owns the shared image is joined,
 *  - a deterministic stale-timer regression cancels one grace session and
 *    verifies that its expiry cannot retire the next session.
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

/* Success grace so the STA-only transition is observable in-test. */
#define TEST_GRACE_MS 700u

/* Dedicated littlefs image so the test never touches another test's state. */
#define TEST_IMAGE_PATH  "/tmp/wifi_prov_fallback_flow.img"
#define TEST_MOUNT_POINT "/"

/* Event/timeout budgets. The transition itself is synchronized on completion
 * semaphores; the timeouts are only upper bounds, never fixed waits. */
#define READY_WAIT_MS        4000u
#define CONNECT_WAIT_MS      6000u
#define EVENT_WAIT_MS        6000u
#define TRANSITION_WAIT_MS   ( TEST_GRACE_MS + 4000u )

/* -- typed-event completion semaphores ------------------------------------- */

static osal_bin_sem_id_t s_connected_sem    = NULL;
static osal_bin_sem_id_t s_mode_changed_sem = NULL;

static void on_connected_event( wifi_mgmt_event_t event, void * user_data )
{
  ( void ) event;
  ( void ) user_data;
  if ( s_connected_sem != NULL ) ( void ) osal_bin_sem_give( s_connected_sem );
}

/* Notify the STA-only transition. Only a MODE_CHANGED with the temporary AP
 * actually retired (HAL mode STA) may release the transition semaphore; the
 * earlier AP+STA transitions that happen while the portal is starting are
 * excluded, so a stale early token can never satisfy the retire wait. */
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

static bool wait_semaphore( osal_bin_sem_id_t sem, uint32_t timeout_ms )
{
  if ( sem == NULL ) return false;
  return osal_bin_sem_timed_wait( sem, timeout_ms ) == OSAL_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Guarded ownership / filesystem fixture                            */
/* ------------------------------------------------------------------ */

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
  /* Fresh device: no saved credentials (and no writable state from an earlier
   * scenario). */
  setup_fs();

  TEST_ASSERT_TRUE_MESSAGE( wifi_hal_mock_reset(), "mock must reset in setUp" );
  wifi_hal_mock_set_start_result( OSAL_SUCCESS );
  wifi_hal_mock_set_connect_result( OSAL_SUCCESS );

  /* Fresh completion semaphores (no token from a prior scenario). */
  if ( s_connected_sem == NULL )
    (void) osal_bin_sem_create( &s_connected_sem, "t_conn", OSAL_SEM_EMPTY );
  if ( s_mode_changed_sem == NULL )
    (void) osal_bin_sem_create( &s_mode_changed_sem, "t_mode", OSAL_SEM_EMPTY );

  /* Shared Mongoose process hosts the controller HTTP/DNS listeners. */
  MongooseProcess_Deinit();
  MongooseProcess_Init();
  TEST_ASSERT_TRUE( MongooseProcess_IsRunning() );

  /* Initialize Wi-Fi management once; each scenario brings it up and waits
   * for readiness before any provisioning component depends on it. */
  wifi_mgmt_init();
}

void tearDown( void )
{
  /* Release any deliberately held mock invocation so the controller, Wi-Fi
   * worker and Mongoose callbacks can drain before teardown. */
  wifi_hal_mock_set_init_hold( false );
  wifi_hal_mock_set_start_hold( false );
  wifi_hal_mock_set_connect_hold( false );
  wifi_hal_mock_set_deinit_hold( false );
  wifi_hal_mock_set_got_ip_hold( false );
  wifi_hal_mock_set_scan_done_hold( false );

  /* Pair the controller-owned reverse-order cleanup with the guarded fixture:
   * provisioning listeners close first (Mongoose is still alive), then the
   * controller is unsubscribed/deinitialized before Wi-Fi, Wi-Fi is stopped
   * and deinitialized before the shared Mongoose process, and the filesystem
   * is only cleaned once every writer behind the shared image is joined. */
  ( void ) wifi_http_provisioning_stop();
  wifi_provisioning_controller_deinit();
  (void) wifi_mgmt_stop();
  (void) wifi_mgmt_deinit();
  MongooseProcess_Deinit();

  cleanup_fs();

  if ( s_connected_sem != NULL )
  {
    ( void ) osal_bin_sem_delete( s_connected_sem );
    s_connected_sem = NULL;
  }
  if ( s_mode_changed_sem != NULL )
  {
    ( void ) osal_bin_sem_delete( s_mode_changed_sem );
    s_mode_changed_sem = NULL;
  }
}

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

/* Reserve a free loopback port by binding to port 0, then close it. */
static int reserve_port( int type )
{
  struct sockaddr_in addr;
  socklen_t          len = sizeof( addr );
  int                fd  = ( int ) socket( AF_INET, type, 0 );

  if ( fd < 0 ) return -1;
  memset( &addr, 0, sizeof(addr) );
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  addr.sin_port        = 0;
  if ( bind( fd, ( struct sockaddr * ) &addr, sizeof(addr) ) != 0 )
  {
    close( fd );
    return -1;
  }
  if ( getsockname( fd, (struct sockaddr * ) &addr, &len ) != 0 )
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

  memset( &addr, 0, sizeof(addr) );
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
  addr.sin_port        = htons( ( uint16_t ) port );
  if ( connect( fd, (struct sockaddr * ) &addr, sizeof(addr) ) != 0 )
  {
    close( fd );
    return -1;
  }

  if ( body != NULL && ctype != NULL )
  {
    int hlen = snprintf( req, sizeof(req),
                         "%s %s HTTP/1.1\r\nHost: localhost\r\n"
                         "Content-Type: %s\r\nContent-Length: %zu\r\n\r\n",
                         method, path, ctype, strlen( body ) );
    ( void ) send( fd, req, hlen, 0 );
    ( void ) send( fd, body, ( int ) strlen( body ), 0 );
  }
  else
  {
    int hlen = snprintf( req, sizeof(req),
                         "%s %s HTTP/1.1\r\nHost: localhost\r\n\r\n",
                         method, path );
    ( void ) send( fd, req, hlen, 0 );
  }

  tv.tv_sec  = 2;
  tv.tv_usec = 0;
  ( void ) setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv) );

  while ( total < outcap )
  {
    int n = ( int ) recv( fd, buf, ( int ) sizeof(buf), 0 );
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
  buf[p++] = 0;
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
  q[1] = 0x34;
  q[2] = 0x01;
  q[3] = 0x00;
  q[5] = 0x01;
  qlen = 12 + put_qname( q + 12, name );
  q[qlen++] = 0;
  q[qlen++] = 1;
  q[qlen++] = 0;
  q[qlen++] = 1;

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

  if ( !( r[2] & 0x80 ) || ( ( r[6] << 8 ) | r[7] ) < 1 ) return false;

  if ( snprintf( out_ip, cap, "%u.%u.%u.%u",
                 r[n - 4], r[n - 3], r[n - 2], r[n - 1] ) >= ( int ) cap )
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

/* Bring Wi-Fi management up and wait for readiness (never polls is_running). */
static void start_wifi_ready( void )
{
  wifi_mgmt_start();
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_wait_ready( READY_WAIT_MS ),
                            "Wi-Fi management must become ready" );
  TEST_ASSERT_TRUE( wifi_mgmt_is_running() );
}

/* Reserve fresh HTTP/DNS ports and pin them on the provisioning module. */
static void reserve_listeners( int * http_port_out, int * dns_port_out,
                               char * http_url, size_t http_cap,
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
  *http_port_out = http_port;
  *dns_port_out  = dns_port;
}

static void subscribe_test_events( void )
{
  TEST_ASSERT_TRUE_MESSAGE(
    wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECTED, on_connected_event, NULL ),
    "must subscribe to CONNECTED" );
  TEST_ASSERT_TRUE_MESSAGE(
    wifi_mgmt_subscribe( WIFI_MGMT_EVENT_MODE_CHANGED, on_mode_changed_event, NULL ),
    "must subscribe to MODE_CHANGED" );
}

/* Deliver the GOT_IP that completes the posted/direct connect and wait for the
 * typed CONNECTED event (the completion path that also arms the controller's
 * grace timer). */
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
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_GRACE,
                         wifi_provisioning_controller_get_state() );
}

/* ------------------------------------------------------------------ */
/*  The complete automatic-fallback flow (event-driven)               */
/* ------------------------------------------------------------------ */

static void test_automatic_fallback_flow( void )
{
  char   http_url[64];
  char   dns_url[64];
  char   resp[2048];
  char   ip[16];
  int    http_port;
  int    dns_port;
  uint32_t t_start;
  bool   ok;

  start_wifi_ready();
  reserve_listeners( &http_port, &dns_port, http_url, sizeof( http_url ),
                     dns_url, sizeof( dns_url ) );

  /* The grace override MUST be installed before init: otherwise the Kconfig
   * default (0 on POSIX) would replace it and the portal would shut down
   * immediately. The transition below proves the 700 ms value was armed. */
  wifi_provisioning_controller_set_success_grace_ms( TEST_GRACE_MS );

  /* --- 1. automatic fallback without saved credentials --------------------
   * No wifi_ap.json exists: the controller starts the portal immediately. */
  TEST_ASSERT_TRUE_MESSAGE( wifi_provisioning_controller_init(),
                            "controller init must start provisioning without credentials" );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                         wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );

  subscribe_test_events();

  /* --- 2. DNS responses --------------------------------------------------- */
  TEST_ASSERT_TRUE_MESSAGE( dns_query_a( dns_port, DNS_NAME, ip, sizeof( ip ) ),
                            "captive DNS must answer an A query" );
  TEST_ASSERT_EQUAL_STRING( DNS_ANSWER, ip );
  printf( "PASS: captive DNS answered %s -> %s\n", DNS_NAME, ip );

  /* --- 3. portal status ---------------------------------------------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "GET", STATUS_PATH, NULL, NULL,
                                  resp, sizeof( resp ) ) > 0 );
  TEST_ASSERT_TRUE( parse_status_code( resp ) == 200 );

  /* --- 4. submit credentials through the portal ---------------------------- */
  TEST_ASSERT_TRUE( http_request( http_port, "POST", CREDENTIALS_PATH,
                                  "application/json",
                                  "{\"ssid\":\"" TEST_SSID
                                  "\",\"password\":\"" TEST_PASSWORD "\"}",
                                  resp, sizeof( resp ) ) > 0 );
  TEST_ASSERT_TRUE( parse_status_code( resp ) == 202 );

  /* --- 5. connect / GOT_IP --------------------------------------------------
   * Wait for the Wi-Fi connect-call notification before injecting GOT_IP, so
   * the state machine is already parked in WAIT_CONNECT. */
  TEST_ASSERT_TRUE_MESSAGE(
    wifi_hal_mock_wait_connect_call_level( 1, CONNECT_WAIT_MS ),
    "submitted credentials must reach the Wi-Fi connect call" );

  complete_station_connection( "10.170.0.50" );
  printf( "PASS: station connected, controller entered grace period\n" );

  /* --- 6. credential persistence --------------------------------------------
     * The typed CONNECTED above completed the write; load it back. */
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

  /* --- 7. STA-only transition after the grace period ------------------------
   * Synchronize to the two real completions instead of the former 5000 ms
  * polling loop: the typed MODE_CHANGED event of the STA-only retire. The
  * elapsed time since the grace
   * window opened is then compared against TEST_GRACE_MS to prove the test's
   * 700 ms override (and not the Kconfig default) was actually armed. */
  t_start = osal_task_get_time_ms();
  TEST_ASSERT_TRUE_MESSAGE( wait_semaphore( s_mode_changed_sem, TRANSITION_WAIT_MS ),
                            "grace expiry must confirm the STA-only mode change" );
  {
    const uint32_t elapsed = osal_task_get_time_ms() - t_start;

    TEST_ASSERT_TRUE_MESSAGE( elapsed >= TEST_GRACE_MS,
                              "grace period must have actually been 700 ms (not the default)" );
    TEST_ASSERT_TRUE_MESSAGE( elapsed <= TRANSITION_WAIT_MS,
                              "transition must complete within the grace budget" );
  }

  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_ONLINE,
                         wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_STOPPED,
                         wifi_http_provisioning_get_state() );
  TEST_ASSERT_TRUE_MESSAGE( wifi_mgmt_is_connected(),
                            "STA connection must survive SoftAP retirement" );
  TEST_ASSERT_FALSE_MESSAGE( http_port_accepts( http_port ),
                             "HTTP listener must be released after the transition" );
  TEST_ASSERT_FALSE( dns_query_a( dns_port, DNS_NAME, ip, sizeof( ip ) ) );

  /* HAL STA snapshot: the STA-only retirement is complete and observable. */
  {
    wifi_hal_mock_state_t mock = { 0 };

    TEST_ASSERT_TRUE( wifi_hal_mock_get_state( &mock ) );
    TEST_ASSERT_EQUAL_INT( WIFI_HAL_MODE_STA, mock.mode );
    TEST_ASSERT_TRUE( mock.connected );
  }
  printf( "PASS: portal stopped, HAL mode = STA (STA-only transition)\n" );
}

/* ------------------------------------------------------------------ */
/*  Deterministic stale-timer regression                              */
/* ------------------------------------------------------------------ */

/*
 * A grace session is cancelled and replaced by a fresh session. The cancelled
 * session's real grace timer was cancelled/joined by controller deinit, so its
 * (never-armed-again) expiry cannot retire the next session. We leave the new
 * session past the original grace window and confirm that it remains
 * PROVISIONING with the portal still reachable and still in AP+STA mode.
 */
static void test_stale_grace_expiry_cannot_retire_next_session( void )
{
  int    http_port, dns_port;
  char   http_url[64], dns_url[64];
  wifi_hal_mock_state_t mock;

  start_wifi_ready();
  reserve_listeners( &http_port, &dns_port, http_url, sizeof( http_url ),
                     dns_url, sizeof( dns_url ) );
  wifi_provisioning_controller_set_success_grace_ms( TEST_GRACE_MS );

  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                         wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );
  subscribe_test_events();

  /* Arm a genuine grace session (a station connects through the portal). */
  TEST_ASSERT_TRUE( wifi_mgmt_set_ap_name( "testnet", ( size_t ) 7 ) );
  TEST_ASSERT_TRUE( wifi_mgmt_set_password( "pw", ( size_t ) 2 ) );
  TEST_ASSERT_TRUE( wifi_mgmt_connect() );
  TEST_ASSERT_TRUE_MESSAGE(
    wifi_hal_mock_wait_connect_call_level( 1, CONNECT_WAIT_MS ),
    "station must reach the Wi-Fi connect call" );
  complete_station_connection( "10.170.0.50" );

  /* Cancel that grace session: controller_deinit cancels and joins the real
   * grace timer and bumps the session generation. */
  wifi_provisioning_controller_deinit();
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_DISABLED,
                         wifi_provisioning_controller_get_state() );

  /* A fresh session starts provisioning (no saved credentials), with no grace
   * armed - the session that owned the stale timer is gone. */
  TEST_ASSERT_TRUE( wifi_provisioning_controller_init() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                         wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );

  /* Inject the cancelled generation's expiry synchronously. Generation and
   * state validation must reject it without retiring the fresh session. */
  wifi_provisioning_controller_test_fire_grace_expiry();

  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                         wifi_provisioning_controller_get_state() );
  TEST_ASSERT_EQUAL_INT( WIFI_PROVISIONING_RUNNING,
                         wifi_http_provisioning_get_state() );
  TEST_ASSERT_TRUE_MESSAGE( http_port_accepts( http_port ),
                            "new session portal must stay reachable past the cancelled grace" );
  TEST_ASSERT_TRUE( wifi_hal_mock_get_state( &mock ) );
  TEST_ASSERT_EQUAL_INT( WIFI_HAL_MODE_APSTA, mock.mode );
  printf( "PASS: stale grace expiry could not retire the next session\n" );
}

/* ------------------------------------------------------------------ */
/*  Runner                                                             */
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

  RUN_TEST( test_automatic_fallback_flow );
  RUN_TEST( test_stale_grace_expiry_cannot_retire_next_session );

  rc = UNITY_END();
  return rc;
}