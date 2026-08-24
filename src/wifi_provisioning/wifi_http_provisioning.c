/*
 * Wi-Fi HTTP provisioning application - lifecycle implementation.
 *
 * See wifi_http_provisioning.h for the public contract. While running the
 * application owns exactly two network listeners:
 *   - the captive DNS responder, delegated to captive_dns_server;
 *   - the provisioning HTTP listener, created on the shared Mongoose thread.
 *
 * Start/stop lifecycle are serialized so repeated calls are safe. Start
 * requires the shared Mongoose process and the Wi-Fi management module to be
 * initialized, requests AP+STA mode before opening listeners, and reaches
 * RUNNING only after both listeners are bound. Partial startup failures are
 * rolled back. Stop closes only the owned listeners and never deinitializes
 * Mongoose.
 *
 * While running, the HTTP listener serves provisioning routes as well as the
 * captive portal. Provisioning API routes are:
 *   - GET  /api/v1/wifi/status      - lifecycle state, stack coordinates, IP details;
 *   - POST /api/v1/wifi/scans       - start an asynchronous scan (202 Accepted);
 *   - GET  /api/v1/wifi/networks    - scan generation, state, and AP records;
 *   - POST /api/v1/wifi/credentials - validate and submit Wi-Fi credentials
 *                                     (202 Accepted) and connect asynchronously;
 *   - DELETE /api/v1/wifi/connection - request an asynchronous station
 *                                      disconnect (202 Accepted) without
 *                                      stopping the provisioning AP or service.
 *
 * API responses use Content-Type: application/json with Cache-Control:
 * no-store. Routes never include or log a Wi-Fi credential.
 *
 * Captive portal routing complements the API. Well-known OS connectivity
 * probes (Android /generate_204, Apple /hotspot-detect.html, Windows
 * /ncsi.txt and /connecttest.txt) and the portal root "/" are answered with
 * the portal landing page, and every other unknown browser GET route is
 * redirected to the portal root. API routes are never redirected. HTTPS is
 * never intercepted: the listener is plain HTTP, so TLS sessions do not
 * produce HTTP messages. All portal responses set content type, no-store
 * cache, and basic security headers.
 */

#include "wifi_http_provisioning.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "captive_dns_server.h"
#include "hq_config.h"
#include "mongoose.h"
#include "mongoose_process.h"
#include "osal_mutex.h"
#include "wifi_managment.h"

/* Default listen URLs from Kconfig. Overridable at runtime per-listener so
 * that tests and POSIX simulations can bind non-privileged high ports. */
#ifndef CONFIG_WIFI_HTTP_PROVISIONING_HTTP_URL
#define CONFIG_WIFI_HTTP_PROVISIONING_HTTP_URL "http://0.0.0.0:80"
#endif
#ifndef CONFIG_WIFI_HTTP_PROVISIONING_DNS_URL
#define CONFIG_WIFI_HTTP_PROVISIONING_DNS_URL "udp://0.0.0.0:53"
#endif

/* Maximum length of a configured listen URL. */
#define WIFI_PROVISIONING_URL_MAX_LEN      128u
/* How long to wait for a listener create/close callback on the poll thread. */
#define WIFI_PROVISIONING_INVOKE_TIMEOUT_MS 2000u

/* Upper bound (bytes) on the provisioning request body. Derived from Kconfig
 * with a safe default so the portal runs even when the option is not wired
 * into the POSIX test configuration. Larger payloads are rejected with 413. */
#ifndef CONFIG_WIFI_HTTP_PROVISIONING_BODY_MAX
#define CONFIG_WIFI_HTTP_PROVISIONING_BODY_MAX 1024u
#endif

/* Reference AP address used as the default captive DNS A-record answer. */
#define WIFI_PROVISIONING_DEFAULT_AP_IP "10.10.0.1"

/* Owned HTTP listener and serialized provisioning lifecycle state.
 *
 * s_http_nc holds the owned HTTP listener and is read and written ONLY inside
 * the Mongoose poll-thread callbacks (http_listener_start_cb /
 * http_listener_stop_cb), so no listener pointer is ever read or modified from
 * another thread.  Other threads observe only locked boolean lifecycle
 * snapshots (s_http_bound, s_dns_started, s_state) and never the pointer.
 *
 * start() and stop() are single-owner operations: each takes
 * s_lifecycle_mutex for the entire operation, so concurrent start/stop calls
 * are fully serialized and can never interleave on s_http_bound,
 * s_dns_started, the Wi-Fi event subscriptions, or the lifecycle state.  A
 * stop caller arriving during WIFI_PROVISIONING_STOPPING simply waits on
 * s_lifecycle_mutex and joins the in-flight stop, observing its completed
 * result. */
static struct mg_connection  *s_http_nc;      /* Poll thread only. */
static bool   s_http_bound;                   /* Lifecycle snapshot.         */
#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
static void ( *s_stop_boundary_hook )( void );
#endif
static wifi_http_provisioning_state_t s_state;
static char   s_http_url[WIFI_PROVISIONING_URL_MAX_LEN];
static bool   s_http_configured;
static char   s_dns_url[WIFI_PROVISIONING_URL_MAX_LEN];
static bool   s_dns_configured;
static uint8_t s_ap_ip[4];
static bool   s_ap_ip_set;
static bool   s_dns_started;                  /* Lifecycle snapshot.         */
static osal_mutex_id_t s_mutex;
static bool   s_mutex_ready;
/* Dedicated lifecycle mutex. Serializes the whole start()/stop() operations so
 * concurrent callers cannot both operate on owned listeners and lifecycle
 * state, and a stop caller arriving during WIFI_PROVISIONING_STOPPING waits
 * for the in-flight stop instead of returning success before cleanup. */
static osal_mutex_id_t s_lifecycle_mutex;
static bool   s_lifecycle_ready;

/* Wi-Fi link failure tracking for the status route. s_wifi_failed latches the
 * most recent CONNECT_FAILED event so the reported "failed" state is stable
 * while WiFi is idle/retrying; s_last_failure is the stable failure category
 * string. Both are protected by s_mutex and touched only by the WiFi event
 * callback (WiFi worker thread) and the status route (Mongoose thread). */
static bool  s_wifi_failed;
static char  s_last_failure[32];

/* Lazily create the state and lifecycle mutexes. Idempotent; safe if the
 * module is reused after a reset. Returns false only if an OSAL call fails. */
static bool prov_ensure_mutex( void )
{
  if ( s_mutex_ready && s_lifecycle_ready ) return true;

  if ( !s_mutex_ready )
  {
    osal_mutex_id_t mtx;
    if ( osal_mutex_create( &mtx, "wifi_prov" ) != OSAL_SUCCESS ) return false;
    s_mutex       = mtx;
    s_mutex_ready = true;
  }

  if ( !s_lifecycle_ready )
  {
    osal_mutex_id_t mtx;
    if ( osal_mutex_create( &mtx, "wifi_prov_lc" ) != OSAL_SUCCESS ) return false;
    s_lifecycle_mutex = mtx;
    s_lifecycle_ready = true;
  }

  return true;
}

/* Set the default captive portal IPv4 answer address (parsed once). */
static void prov_ensure_default_ap_ip( void )
{
  char   buf[] = WIFI_PROVISIONING_DEFAULT_AP_IP;
  char * tok;
  unsigned values[4] = {0, 0, 0, 0};
  int      i = 0;

  if ( s_ap_ip_set ) return;

  tok = strtok( buf, "." );
  while ( tok != NULL && i < 4 )
  {
    char * end;
    long   v = strtol( tok, &end, 10 );
    if ( end != tok && v >= 0 && v <= 255 ) values[i++] = (unsigned) v;
    tok = strtok( NULL, "." );
  }

  if ( i == 4 )
  {
    for ( i = 0; i < 4; i++ ) s_ap_ip[i] = (uint8_t) values[i];
  }
  else
  {
    s_ap_ip[0] = 10; s_ap_ip[1] = 10; s_ap_ip[2] = 0; s_ap_ip[3] = 1;
  }
  s_ap_ip_set = true;
}

/* Extra headers returned on every provisioning API response. */
static const char *s_json_headers =
  "Content-Type: application/json\r\nCache-Control: no-store\r\n";

/* Stable JSON error body returned when a Wi-Fi snapshot cannot be read. */
static const char *s_snapshot_unavailable =
  "{\"error\":\"snapshot_unavailable\"}";

/* Stable JSON error bodies for invalid credential submissions. */
static const char *s_err_media_type = "{\"error\":\"unsupported_media_type\"}";
static const char *s_err_too_large  = "{\"error\":\"payload_too_large\"}";
static const char *s_err_bad_json   = "{\"error\":\"invalid_json\"}";
static const char *s_err_missing    = "{\"error\":\"missing_field\"}";
static const char *s_err_bad_ssid   = "{\"error\":\"invalid_ssid\"}";
static const char *s_err_bad_pass   = "{\"error\":\"invalid_password\"}";
static const char *s_err_service_unavailable =
  "{\"error\":\"service_unavailable\"}";
static const char *s_err_cannot_disconnect =
  "{\"error\":\"cannot_disconnect\"}";

/* Headers returned on every captive portal HTML response. They pin the media
 * type, forbid caching (the portal is always live), and add basic security
 * headers: nosniff prevents MIME-based content sniffing and X-Frame-Options
 * denies browser frame embedding (clickjacking defence). */
static const char *s_html_headers =
  "Content-Type: text/html; charset=utf-8\r\n"
  "Cache-Control: no-store\r\n"
  "X-Content-Type-Options: nosniff\r\n"
  "X-Frame-Options: DENY\r\n";

/* Full headers returned on a portal fallback redirect. Carries the Location
 * (to the portal root), a no-store cache policy, and plain text content so the
 * redirect body stays tiny. */
static const char *s_redirect_headers =
  "Location: /\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\n";

/* Full headers returned when a portal/probe/redirect route receives an
 * unsupported method: the allowed method plus cache and content headers. */
static const char *s_method_headers =
  "Allow: GET\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\n";

/* Minimal, self-contained captive portal landing page. Served directly for the
 * portal root ("/") and for every recognised OS connectivity probe so the
 * device detects the captive network and opens its assistant UI. Keeping the
 * page static and dependency-free makes the portal fast and deterministic. */
static const char *s_portal_html =
  "<!DOCTYPE html>\r\n"
  "<html><head><title>Network assistant</title></head>\r\n"
  "<body><h1>Network assistant</h1>\r\n"
  "<p>This network needs setup. Please choose a Wi-Fi network and "
  "connect.</p>\r\n"
  "</body></html>\r\n";

/* Number of SSID / password bytes accepted by the credentials route. These
 * mirror the Wi-Fi HAL limits (excluding the NUL terminator). */
#define CRED_SSID_MAX  MAX_SSID_SIZE
#define CRED_PASS_MAX  MAX_PASSWORD_SIZE

/* Lower-case an ASCII character (used for case-insensitive header compare). */
static char prov_lower( char c )
{
  if ( c >= 'A' && c <= 'Z' ) return ( char ) ( c + ( 'a' - 'A' ) );
  return c;
}

/* Case-insensitive substring search over a length-bounded byte buffer. */
static bool prov_buf_contains_ci( const char * data, size_t len,
                                  const char * needle )
{
  size_t nl = strlen( needle );
  size_t i;

  if ( data == NULL ) return false;
  if ( nl == 0 ) return true;
  if ( nl > len ) return false;

  for ( i = 0; i + nl <= len; i++ )
  {
    size_t j;
    for ( j = 0; j < nl; j++ )
    {
      if ( prov_lower( data[i + j] ) != prov_lower( needle[j] ) ) break;
    }
    if ( j == nl ) return true;
  }
  return false;
}

/* Return true when @p data contains a NUL byte within its first @p len bytes. */
static bool prov_buf_has_nul( const char * data, size_t len )
{
  size_t i;

  for ( i = 0; i < len; i++ )
  {
    if ( data[i] == '\0' ) return true;
  }
  return false;
}

/* Best-effort wipe of a temporary buffer that held a secret. A volatile
 * pointer guarantees the compiler cannot elide the wipe as dead-store. */
static void prov_wipe( void * p, size_t n )
{
  volatile unsigned char * v = ( volatile unsigned char * ) p;
  size_t i;

  if ( p == NULL ) return;
  for ( i = 0u; i < n; i++ ) v[i] = 0u;
}

/* Convert a provisioning lifecycle state to a stable JSON string. */
static const char * prov_provisioning_state_name( wifi_http_provisioning_state_t st )
{
  switch ( st )
  {
    case WIFI_PROVISIONING_STOPPED:  return "stopped";
    case WIFI_PROVISIONING_STARTING: return "starting";
    case WIFI_PROVISIONING_RUNNING:  return "running";
    case WIFI_PROVISIONING_STOPPING: return "stopping";
    case WIFI_PROVISIONING_ERROR:    return "error";
    default:                         return "unknown";
  }
}

/* Copy the tracked Wi-Fi failure state out under the state mutex. */
static void prov_snapshot_wifi_failure( bool * failed, char * category,
                                        size_t category_size )
{
  if ( failed )   *failed   = false;
  if ( category ) category[0] = '\0';
  if ( !s_mutex_ready ) return;

  (void) osal_mutex_take( s_mutex );
  if ( failed )   *failed   = s_wifi_failed;
  if ( category && category_size > 0 )
  {
    strncpy( category, s_last_failure, category_size - 1 );
    category[category_size - 1] = '\0';
  }
  (void) osal_mutex_give( s_mutex );
}

/* Derive the reported Wi-Fi link state from live snapshots plus the latched
 * failure state. The order matters: "stopped", "connecting", and "connected"
 * are read live, then the failure latch distinguishes "failed" from
 * "disconnected". */
static const char * prov_wifi_state_name( void )
{
  bool failed = false;

  if ( !wifi_mgmt_is_running() )    return "stopped";
  if ( wifi_mgmt_trying_connect() ) return "connecting";
  if ( wifi_mgmt_is_connected() )   return "connected";
  prov_snapshot_wifi_failure( &failed, NULL, 0 );
  if ( failed )                     return "failed";
  return "disconnected";
}

/* Relay WiFi management events into the tracked Wi-Fi failure state. These
 * callbacks run on the WiFi worker thread; the status route (Mongoose thread)
 * reads the same fields under the same state mutex. */
static void prov_wifi_event_cb( wifi_mgmt_event_t event, void * user_data )
{
  ( void ) user_data;
  if ( !s_mutex_ready ) return;

  (void) osal_mutex_take( s_mutex );
  switch ( event )
  {
    case WIFI_MGMT_EVENT_CONNECTED:
      s_wifi_failed = false;
      strncpy( s_last_failure, "no_failure", sizeof( s_last_failure ) - 1 );
      break;
    case WIFI_MGMT_EVENT_DISCONNECTED:
      s_wifi_failed = false;      /* A clean/user disconnect is not a failure. */
      break;
    case WIFI_MGMT_EVENT_CONNECT_FAILED:
      s_wifi_failed = true;
      strncpy( s_last_failure, "connect_failed", sizeof( s_last_failure ) - 1 );
      break;
    case WIFI_MGMT_EVENT_MODE_CHANGED:
      s_wifi_failed = false;
      strncpy( s_last_failure, "no_failure", sizeof( s_last_failure ) - 1 );
      break;
    default:
      break;
  }
  s_last_failure[sizeof( s_last_failure ) - 1] = '\0';
  (void) osal_mutex_give( s_mutex );
}

/* Subscribe the WiFi events the status route depends on. */
static void prov_subscribe_wifi_events( void )
{
  (void) wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECTED, prov_wifi_event_cb, NULL );
  (void) wifi_mgmt_subscribe( WIFI_MGMT_EVENT_DISCONNECTED, prov_wifi_event_cb, NULL );
  (void) wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECT_FAILED, prov_wifi_event_cb, NULL );
  (void) wifi_mgmt_subscribe( WIFI_MGMT_EVENT_MODE_CHANGED, prov_wifi_event_cb, NULL );
}

/* Drop every WiFi event subscription acquired when provisioning started. */
static void prov_unsubscribe_wifi_events( void )
{
  (void) wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_CONNECTED, prov_wifi_event_cb, NULL );
  (void) wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_DISCONNECTED, prov_wifi_event_cb, NULL );
  (void) wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_CONNECT_FAILED, prov_wifi_event_cb, NULL );
  (void) wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_MODE_CHANGED, prov_wifi_event_cb, NULL );
}

/* Build and send the GET /api/v1/wifi/status JSON response. The payload never
 * contains or derives a password; only the SSID, IP details, states, and the
 * last failure category are reported. */
static void prov_respond_status( struct mg_connection * nc )
{
  wifi_mgmt_ip_info_t info;
  char   category[32];
  char * json;
  cJSON * root;

  /* Stable JSON error whenever a snapshot cannot be read. */
  if ( !wifi_mgmt_get_ip_info( &info ) )
  {
    mg_http_reply( nc, 500, s_json_headers, "%s", s_snapshot_unavailable );
    return;
  }

  prov_snapshot_wifi_failure( NULL, category, sizeof( category ) );

  root = cJSON_CreateObject();
  if ( !root )
  {
    mg_http_reply( nc, 500, s_json_headers, "%s", s_snapshot_unavailable );
    return;
  }

  (void) cJSON_AddStringToObject( root, "provisioning",
           prov_provisioning_state_name( wifi_http_provisioning_get_state() ) );
  (void) cJSON_AddStringToObject( root, "state", prov_wifi_state_name() );
  (void) cJSON_AddStringToObject( root, "ssid", info.ssid );
  (void) cJSON_AddStringToObject( root, "ip", info.ip );
  (void) cJSON_AddStringToObject( root, "netmask", info.netmask );
  (void) cJSON_AddStringToObject( root, "gateway", info.gw );
  (void) cJSON_AddStringToObject( root, "last_failure", category );

  json = cJSON_PrintUnformatted( root );
  cJSON_Delete( root );
  if ( !json )
  {
    mg_http_reply( nc, 500, s_json_headers, "%s", s_snapshot_unavailable );
    return;
  }
  mg_http_reply( nc, 200, s_json_headers, "%s", json );
  cJSON_free( json );
}

/* Start a background scan in response to POST /api/v1/wifi/scans. A scan
 * request is accepted with 202 Accepted regardless of the outcome. If a scan
 * is already running the request is debounced (no duplicate HAL scan is
 * started); otherwise a non-blocking scan is requested and the response
 * reports whether it started ("scanning") or could not be queued ("failed").
 * The JSON also carries the current scan generation so callers can correlate
 * completion against a later network snapshot. */
static void prov_handle_scan_request( struct mg_connection * nc )
{
  uint32_t generation = wifi_mgmt_get_scan_generation();
  char   * json;
  cJSON  * root = cJSON_CreateObject();

  if ( !root )
  {
    mg_http_reply( nc, 500, s_json_headers, "%s", s_snapshot_unavailable );
    return;
  }

  if ( wifi_mgmt_is_scan_active() )
  {
    /* A scan is in flight: debounce, do not start a duplicate. */
    (void) cJSON_AddStringToObject( root, "state", "scanning" );
  }
  else if ( wifi_mgmt_start_scan_no_block() )
  {
    (void) cJSON_AddStringToObject( root, "state", "scanning" );
  }
  else
  {
    (void) cJSON_AddStringToObject( root, "state", "failed" );
  }
  (void) cJSON_AddNumberToObject( root, "generation", (double) generation );

  json = cJSON_PrintUnformatted( root );
  cJSON_Delete( root );
  if ( !json )
  {
    mg_http_reply( nc, 500, s_json_headers, "%s", s_snapshot_unavailable );
    return;
  }
  mg_http_reply( nc, 202, s_json_headers, "%s", json );
  cJSON_free( json );
}

/* Build and send the GET /api/v1/wifi/networks JSON response. The payload
 * reports the current scan generation and scan state ("scanning" while a scan
 * is active, otherwise "idle") together with the recorded access points. Each
 * network object carries the SSID, channel, RSSI (dBm), and authentication
 * mode exactly as published by the Wi-Fi management snapshot. */
static void prov_respond_networks( struct mg_connection * nc )
{
  wifi_mgmt_ap_list_t list;
  uint32_t            generation = wifi_mgmt_get_scan_generation();
  char   * json;
  cJSON * root;
  cJSON * arr;
  int     i;

  if ( !wifi_mgmt_get_access_points( &list ) )
  {
    mg_http_reply( nc, 500, s_json_headers, "%s", s_snapshot_unavailable );
    return;
  }

  root = cJSON_CreateObject();
  arr  = cJSON_CreateArray();
  if ( !root || !arr )
  {
    if ( root ) cJSON_Delete( root );
    if ( arr )  cJSON_Delete( arr );
    mg_http_reply( nc, 500, s_json_headers, "%s", s_snapshot_unavailable );
    return;
  }

  for ( i = 0; i < (int) list.count; ++i )
  {
    cJSON * ap = cJSON_CreateObject();
    if ( !ap ) continue;
    (void) cJSON_AddStringToObject( ap, "ssid", list.items[i].ssid );
    (void) cJSON_AddNumberToObject( ap, "channel", list.items[i].chan );
    (void) cJSON_AddNumberToObject( ap, "rssi", list.items[i].rssi );
    (void) cJSON_AddNumberToObject( ap, "auth", list.items[i].auth );
    (void) cJSON_AddItemToArray( arr, ap );
  }

  (void) cJSON_AddItemToObject( root, "networks", arr );
  (void) cJSON_AddNumberToObject( root, "generation", (double) generation );
  (void) cJSON_AddStringToObject( root, "state",
           wifi_mgmt_is_scan_active() ? "scanning" : "idle" );

  json = cJSON_PrintUnformatted( root );
  cJSON_Delete( root );
  if ( !json )
  {
    mg_http_reply( nc, 500, s_json_headers, "%s", s_snapshot_unavailable );
    return;
  }
  mg_http_reply( nc, 200, s_json_headers, "%s", json );
  cJSON_free( json );
}

/* Handle POST /api/v1/wifi/credentials.
 *
 * The request body must be JSON with "ssid" and "password" string fields. The
 * handler validates the Content-Type, applies a small configured body-size
 * limit, rejects embedded NULs and malformed JSON, validates the SSID and
 * password byte lengths (an empty password is permitted for open networks),
 * then pushes the credentials into Wi-Fi management and requests an
 * asynchronous connect. Any temporary buffer that held a password is wiped
 * before the handler returns, and no response or log ever contains one. */
static void prov_handle_credentials_request( struct mg_connection * nc,
                                             struct mg_http_message * hm )
{
  struct mg_str * ctype = mg_http_get_header( hm, "Content-Type" );
  size_t          body_size = hm->body.len;
  char          * json_copy = NULL;
  cJSON         * root = NULL;
  cJSON         * field;
  const char    * ssid_text = NULL;
  const char    * pass_text = NULL;
  size_t          ssid_len = 0u;
  size_t          pass_len = 0u;
  bool            ssid_present = false;
  bool            pass_present = false;
  char            ssid[ CRED_SSID_MAX + 1 ];
  char            password[ CRED_PASS_MAX + 1 ];

  prov_wipe( ssid, sizeof( ssid ) );
  prov_wipe( password, sizeof( password ) );

  /* 415: the route only accepts application/json bodies. */
  if ( ctype == NULL ||
       !prov_buf_contains_ci( ctype->buf, ctype->len, "application/json" ) )
  {
    mg_http_reply( nc, 415, s_json_headers, "%s", s_err_media_type );
    return;
  }

  /* 413: reject oversized request bodies before buffering them. */
  if ( body_size > CONFIG_WIFI_HTTP_PROVISIONING_BODY_MAX )
  {
    mg_http_reply( nc, 413, s_json_headers, "%s", s_err_too_large );
    return;
  }

  /* Reject embedded NUL data before handing the body to cJSON. */
  if ( prov_buf_has_nul( hm->body.buf, body_size ) )
  {
    mg_http_reply( nc, 400, s_json_headers, "%s", s_err_bad_json );
    return;
  }

  /* Make a NUL-terminated copy (the only buffer that holds the password). */
  json_copy = ( char * ) malloc( body_size + 1u );
  if ( json_copy == NULL )
  {
    mg_http_reply( nc, 500, s_json_headers, "%s", s_snapshot_unavailable );
    return;
  }
  memcpy( json_copy, hm->body.buf, body_size );
  json_copy[body_size] = '\0';

  root = cJSON_Parse( json_copy );
  if ( root == NULL )   /* malformed JSON */
  {
    prov_wipe( json_copy, body_size + 1u );
    free( json_copy );
    mg_http_reply( nc, 400, s_json_headers, "%s", s_err_bad_json );
    return;
  }

  field = cJSON_GetObjectItem( root, "ssid" );
  if ( field != NULL && cJSON_IsString( field ) )
  {
    ssid_text    = cJSON_GetStringValue( field );
    ssid_present = ( ssid_text != NULL );
    ssid_len     = ssid_present ? ( size_t ) strlen( ssid_text ) : 0u;
    if ( ssid_len <= CRED_SSID_MAX ) memcpy( ssid, ssid_text, ssid_len );
  }

  field = cJSON_GetObjectItem( root, "password" );
  if ( field != NULL && cJSON_IsString( field ) )
  {
    pass_text    = cJSON_GetStringValue( field );
    pass_present = ( pass_text != NULL );
    pass_len     = pass_present ? ( size_t ) strlen( pass_text ) : 0u;
    if ( pass_len <= CRED_PASS_MAX ) memcpy( password, pass_text, pass_len );
  }

  /* Release the parsed tree and wipe the body copy that held the password. */
  cJSON_Delete( root );
  root = NULL;
  prov_wipe( json_copy, body_size + 1u );
  free( json_copy );
  json_copy = NULL;

  /* 400: both fields are required. */
  if ( !ssid_present || !pass_present )
  {
    prov_wipe( password, sizeof( password ) );
    mg_http_reply( nc, 400, s_json_headers, "%s", s_err_missing );
    return;
  }

  /* 400: SSID must be 1..CRED_SSID_MAX bytes. */
  if ( ssid_len == 0u || ssid_len > CRED_SSID_MAX )
  {
    prov_wipe( password, sizeof( password ) );
    mg_http_reply( nc, 400, s_json_headers, "%s", s_err_bad_ssid );
    return;
  }

  /* 400: password must not exceed CRED_PASS_MAX bytes (empty is allowed). */
  if ( pass_len > CRED_PASS_MAX )
  {
    prov_wipe( password, sizeof( password ) );
    mg_http_reply( nc, 400, s_json_headers, "%s", s_err_bad_pass );
    return;
  }

  /* Push credentials and request an asynchronous connect. */
  ( void ) wifi_mgmt_set_ap_name( ssid, ssid_len );
  ( void ) wifi_mgmt_set_password( password, pass_len );
  ( void ) wifi_mgmt_connect();

  /* Never retain a password in a scratch buffer. */
  prov_wipe( password, sizeof( password ) );
  prov_wipe( ssid, sizeof( ssid ) );

  mg_http_reply( nc, 202, s_json_headers, "%s", "{\"state\":\"accepted\"}" );
}

/* Handle DELETE /api/v1/wifi/connection.
 *
 * Requests an asynchronous station disconnect through Wi-Fi management. The
 * request is accepted with 202 Accepted whenever a disconnect can be queued,
 * matching the asynchronous behaviour of the credentials and scan routes. The
 * route never stops the provisioning AP or the HTTP listener, so the portal
 * remains reachable after a station disconnect. When a disconnect cannot be
 * requested the handler answers with a stable conflict (409) or service error
 * (503) JSON body. The reply contains no credential data. */
static void prov_handle_disconnect_request( struct mg_connection * nc )
{
  /* If Wi-Fi management is not running the disconnect cannot be requested:
   * report a stable service error. */
  if ( !wifi_mgmt_is_running() )
  {
    mg_http_reply( nc, 503, s_json_headers, "%s", s_err_service_unavailable );
    return;
  }

  /* wifi_mgmt_disconnect() is refused only when the driver is in AP-only mode,
   * so the active provisioning portal (AP+STA) cannot normally hit this. It is
   * still guarded so an unexpected refusal returns a stable conflict. */
  if ( !wifi_mgmt_disconnect() )
  {
    mg_http_reply( nc, 409, s_json_headers, "%s", s_err_cannot_disconnect );
    return;
  }

  mg_http_reply( nc, 202, s_json_headers, "%s", "{\"state\":\"accepted\"}" );
}

/* Serve the captive portal landing page as a 200 OK HTML document. */
static void prov_respond_portal( struct mg_connection * nc )
{
  mg_http_reply( nc, 200, s_html_headers, "%s", s_portal_html );
}

/* Redirect a browser GET route to the portal root ("/"). The Location header
 * drives the client to the landing page, which is served on the next request.
 * The body is tiny and carries no secrets. */
static void prov_redirect_to_portal( struct mg_connection * nc )
{
  mg_http_reply( nc, 302, s_redirect_headers,
                 "%s", "Redirecting to the network assistant" );
}

/* Return true when @p uri is one of the well-known captive portal connectivity
 * probes used by mobile and desktop network awareness checks:
 *   - Android : /generate_204
 *   - Apple   : /hotspot-detect.html
 *   - Windows : /ncsi.txt and /connecttest.txt
 * These paths are answered directly with the portal page so the client detects
 * the captive network instead of receiving a vendor "success" payload. */
static bool prov_is_captive_probe( struct mg_str uri )
{
  return mg_match( uri, mg_str( "/generate_204" ), NULL ) ||
         mg_match( uri, mg_str( "/hotspot-detect.html" ), NULL ) ||
         mg_match( uri, mg_str( "/ncsi.txt" ), NULL ) ||
         mg_match( uri, mg_str( "/connecttest.txt" ), NULL );
}

/* Return true when @p uri belongs to the provisioning API namespace. API
 * routes are served (or answered) by the routes above and are never redirected
 * to the portal. Mongoose '#' matches across slashes, unlike '*', so nested
 * API paths are covered. */
static bool prov_is_api_uri( struct mg_str uri )
{
  return mg_match( uri, mg_str( "/api/#" ), NULL ) ||
         mg_strcmp( uri, mg_str( "/api" ) ) == 0;
}

/* HTTP event handler. Serves the provisioning status, scan, network,
 * credentials, and connection routes and answers everything else with a plain
 * 404. Also implements captive portal routing: well-known OS connectivity
 * probes and the portal root are served with the portal page, and every other
 * unknown browser GET is redirected to the portal root (while API routes are
 * never redirected). HTTPS is not intercepted - this is a plain HTTP listener,
 * so TLS sessions simply do not produce MG_EV_HTTP_MSG events. */
static void http_ev_handler( struct mg_connection * nc, int ev, void * ev_data )
{
  struct mg_http_message * hm;

  if ( ev != MG_EV_HTTP_MSG ) return;
  hm = ( struct mg_http_message * ) ev_data;

  if ( mg_match( hm->uri, mg_str( "/api/v1/wifi/status" ), NULL ) )
  {
    if ( mg_strcasecmp( hm->method, mg_str( "GET" ) ) != 0 )
    {
      mg_http_reply( nc, 405,
        "Allow: GET\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n",
        "%s", "{\"error\":\"method_not_allowed\"}" );
      nc->is_draining = 1;
      return;
    }
    prov_respond_status( nc );
    nc->is_draining = 1;
    return;
  }

  if ( mg_match( hm->uri, mg_str( "/api/v1/wifi/scans" ), NULL ) )
  {
    if ( mg_strcasecmp( hm->method, mg_str( "POST" ) ) != 0 )
    {
      mg_http_reply( nc, 405,
        "Allow: POST\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n",
        "%s", "{\"error\":\"method_not_allowed\"}" );
      nc->is_draining = 1;
      return;
    }
    prov_handle_scan_request( nc );
    nc->is_draining = 1;
    return;
  }

  if ( mg_match( hm->uri, mg_str( "/api/v1/wifi/networks" ), NULL ) )
  {
    if ( mg_strcasecmp( hm->method, mg_str( "GET" ) ) != 0 )
    {
      mg_http_reply( nc, 405,
        "Allow: GET\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n",
        "%s", "{\"error\":\"method_not_allowed\"}" );
      nc->is_draining = 1;
      return;
    }
    prov_respond_networks( nc );
    nc->is_draining = 1;
    return;
  }

  if ( mg_match( hm->uri, mg_str( "/api/v1/wifi/credentials" ), NULL ) )
  {
    if ( mg_strcasecmp( hm->method, mg_str( "POST" ) ) != 0 )
    {
      mg_http_reply( nc, 405,
        "Allow: POST\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n",
        "%s", "{\"error\":\"method_not_allowed\"}" );
      nc->is_draining = 1;
      return;
    }
    prov_handle_credentials_request( nc, hm );
    nc->is_draining = 1;
    return;
  }

  if ( mg_match( hm->uri, mg_str( "/api/v1/wifi/connection" ), NULL ) )
  {
    if ( mg_strcasecmp( hm->method, mg_str( "DELETE" ) ) != 0 )
    {
      mg_http_reply( nc, 405,
        "Allow: DELETE\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n",
        "%s", "{\"error\":\"method_not_allowed\"}" );
      nc->is_draining = 1;
      return;
    }
    prov_handle_disconnect_request( nc );
    nc->is_draining = 1;
    return;
  }

  /* A URI in the API namespace that matched no route above is an API miss.
   * It is answered with a plain 404 and is never redirected to the portal.
   * The checks above have already rejected unsupported methods on known API
   * routes with 405, so an unknown API path is simply Not Found. */
  if ( prov_is_api_uri( hm->uri ) )
  {
    mg_http_reply( nc, 404, "Content-Type: text/plain\r\n", "Not Found" );
    nc->is_draining = 1;
    return;
  }

  /* ---- Captive portal fallback routing ---- */
  /* The browser-fallback router only serves GET. Any other method on a
   * portal/probe/redirect route is unsupported and returns 405 with an
   * Allow header. */
  if ( mg_strcasecmp( hm->method, mg_str( "GET" ) ) != 0 )
  {
    mg_http_reply( nc, 405, s_method_headers,
                   "%s", "Method Not Allowed" );
    nc->is_draining = 1;
    return;
  }

  /* Well-known OS connectivity probes are answered with the portal page so the
   * device detects the captive portal (a vendor "success" payload would make
   * it believe the network is already connected). */
  if ( prov_is_captive_probe( hm->uri ) )
  {
    prov_respond_portal( nc );
    nc->is_draining = 1;
    return;
  }

  /* The portal root serves the landing page directly. */
  if ( mg_strcmp( hm->uri, mg_str( "/" ) ) == 0 )
  {
    prov_respond_portal( nc );
    nc->is_draining = 1;
    return;
  }

  /* Every other unknown browser GET route is redirected to the portal root. */
  prov_redirect_to_portal( nc );
  nc->is_draining = 1;
}

/* Result of an HTTP listener bind carried out on the Mongoose poll thread. The
 * poll thread writes the boolean; the caller reads it only after
 * MongooseProcess_Invoke() returns, so the owned connection pointer itself is
 * never shared across threads. */
typedef struct
{
  bool bound;
} http_listener_bind_result_t;

/* Creates the HTTP listener on the Mongoose poll thread. Runs exclusively on
 * the poll thread, so it is the only place (besides http_listener_stop_cb)
 * that may read or write the owned s_http_nc pointer. */
static void http_listener_start_cb( struct mg_mgr * mgr, void * user )
{
  http_listener_bind_result_t * result = ( http_listener_bind_result_t * ) user;

  if ( result != NULL ) result->bound = false;
  if ( s_http_nc != NULL )
  {
    if ( result != NULL ) result->bound = true;  /* Already listening. */
    return;
  }
  s_http_nc = mg_http_listen( mgr, s_http_url, http_ev_handler, NULL );
  if ( result != NULL ) result->bound = ( s_http_nc != NULL );
}

/* Closes only the HTTP listener owned by this module. Leaves the shared
 * process (and any other listeners) intact. Runs on the Mongoose poll thread
 * and completes before wifi_http_provisioning_stop() returns (the invocation
 * is per-request synchronous). */
static void http_listener_stop_cb( struct mg_mgr * mgr, void * user )
{
  ( void ) mgr;
  ( void ) user;
  if ( s_http_nc != NULL )
  {
    /* Ask the poll thread to release the listener. Mongoose closes the
     * underlying socket only when close_conn() runs in the poll loop after
     * is_closing is set; calling mg_close_conn() here would free the
     * connection record but leak the listening fd. */
    s_http_nc->is_closing = 1;
    s_http_nc = NULL;
  }
}

/* Move the module into ERROR without leaving partial listeners behind. */
static void prov_set_error( void )
{
  ( void ) osal_mutex_take( s_mutex );
  s_state = WIFI_PROVISIONING_ERROR;
  ( void ) osal_mutex_give( s_mutex );
}

bool wifi_http_provisioning_start( void )
{
  bool started = false;

  if ( !prov_ensure_mutex() ) return false;

  /* Serialize the entire start() with stop(): single-owner lifecycle. */
  ( void ) osal_mutex_take( s_lifecycle_mutex );

  /* Idempotent: already running or starting is a safe no-op. */
  ( void ) osal_mutex_take( s_mutex );
  if ( s_state == WIFI_PROVISIONING_RUNNING ||
       s_state == WIFI_PROVISIONING_STARTING )
  {
    ( void ) osal_mutex_give( s_mutex );
    ( void ) osal_mutex_give( s_lifecycle_mutex );
    return true;
  }
  /* A stop cannot be in flight while we hold s_lifecycle_mutex, so the state
   * is STOPPED or ERROR here; both are valid start points. */
  s_state = WIFI_PROVISIONING_STARTING;
  ( void ) osal_mutex_give( s_mutex );

  /* Require the shared Mongoose process to be running. */
  if ( !MongooseProcess_IsRunning() )
  {
    prov_set_error();
    goto out;
  }

  /* Require Wi-Fi management to be initialized/started. */
  if ( !wifi_mgmt_is_running() )
  {
    prov_set_error();
    goto out;
  }

  /* Apply Kconfig defaults when no runtime override was provided. */
  ( void ) osal_mutex_take( s_mutex );
  if ( !s_http_configured )
  {
    strncpy( s_http_url, CONFIG_WIFI_HTTP_PROVISIONING_HTTP_URL,
             sizeof( s_http_url ) - 1 );
    s_http_url[sizeof( s_http_url ) - 1] = '\0';
    s_http_configured = true;
  }
  if ( !s_dns_configured )
  {
    strncpy( s_dns_url, CONFIG_WIFI_HTTP_PROVISIONING_DNS_URL,
             sizeof( s_dns_url ) - 1 );
    s_dns_url[sizeof( s_dns_url ) - 1] = '\0';
    s_dns_configured = true;
  }
  ( void ) osal_mutex_give( s_mutex );

  prov_ensure_default_ap_ip();

  /* Request AP+STA mode before opening listeners. */
  if ( !wifi_mgmt_request_mode( T_WIFI_TYPE_CLI_SER ) )
  {
    prov_set_error();
    goto out;
  }

  /* Start the captive DNS listener first. */
  if ( !captive_dns_server_set_bind_url( s_dns_url ) )
  {
    prov_set_error();
    goto out;
  }
  captive_dns_server_set_ip4( s_ap_ip );
  if ( !captive_dns_server_start() )
  {
    prov_set_error();
    goto out;
  }
  s_dns_started = true;

  /* Start the provisioning HTTP listener on the poll thread. The callback
   * reports the bind outcome through the caller-local result, so start() never
   * reads the owned s_http_nc pointer itself. */
  {
    http_listener_bind_result_t bind_result;

    if ( !MongooseProcess_Invoke( http_listener_start_cb, &bind_result,
                                  WIFI_PROVISIONING_INVOKE_TIMEOUT_MS ) ||
         !bind_result.bound )
    {
      /* Invocation failed or the HTTP bind was refused: roll back the DNS
       * listener that was already started. */
      ( void ) captive_dns_server_stop();
      s_dns_started = false;
      prov_set_error();
      goto out;
    }
  }
  s_http_bound = true;

  /* Track Wi-Fi link state while the portal is active so the status route can
   * report the current link state and the last failure category. */
  ( void ) osal_mutex_take( s_mutex );
  s_wifi_failed = false;
  strncpy( s_last_failure, "no_failure", sizeof( s_last_failure ) - 1 );
  s_last_failure[sizeof( s_last_failure ) - 1] = '\0';
  ( void ) osal_mutex_give( s_mutex );
  prov_subscribe_wifi_events();

  /* Running only after both listeners are bound. */
  ( void ) osal_mutex_take( s_mutex );
  s_state = WIFI_PROVISIONING_RUNNING;
  ( void ) osal_mutex_give( s_mutex );
  started = true;

out:
  ( void ) osal_mutex_give( s_lifecycle_mutex );
  return started;
}

bool wifi_http_provisioning_stop( void )
{
  bool ok = true;

  if ( !prov_ensure_mutex() ) return false;

  /* Serialize the whole stop() with start() (single-owner lifecycle). A caller
   * arriving while another stop is in flight (WIFI_PROVISIONING_STOPPING)
   * blocks here, joins the in-flight stop, and then observes STOPPED below and
   * returns the same completed result. */
  ( void ) osal_mutex_take( s_lifecycle_mutex );

  ( void ) osal_mutex_take( s_mutex );
  if ( s_state == WIFI_PROVISIONING_STOPPED )
  {
    ( void ) osal_mutex_give( s_mutex );
    ( void ) osal_mutex_give( s_lifecycle_mutex );
    return true;  /* Already stopped. */
  }
  s_state = WIFI_PROVISIONING_STOPPING;
  ( void ) osal_mutex_give( s_mutex );

  /* Stop ordering: drop the Wi-Fi event subscriptions first, then close the
   * HTTP listener, then the DNS listener, confirm both closures completed on
   * the poll thread, and only then report STOPPED. */
  prov_unsubscribe_wifi_events();

  /* HTTP listener: close on the poll thread and confirm that the close
   * callback completed. If the owned listener cannot be closed while Mongoose
   * is still running, the stop fails and the module reports ERROR. */
#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
  if ( s_stop_boundary_hook != NULL ) s_stop_boundary_hook();
#endif
  if ( s_http_bound && MongooseProcess_IsRunning() )
  {
    if ( !MongooseProcess_Invoke( http_listener_stop_cb, NULL,
                                  WIFI_PROVISIONING_INVOKE_TIMEOUT_MS ) )
      ok = false;
    else
      s_http_bound = false;
  }
  else if ( s_http_bound )
  {
    s_http_bound = false;
  }

  /* DNS listener: close through the captive DNS service (its stop runs the
   * close callback on the poll thread and reports whether it completed). */
  if ( s_dns_started )
  {
    if ( captive_dns_server_stop() ) s_dns_started = false;
    else ok = false;
  }

  /* Confirm closure succeeded before reporting STOPPED; otherwise ERROR. */
  ( void ) osal_mutex_take( s_mutex );
  s_state = ok ? WIFI_PROVISIONING_STOPPED : WIFI_PROVISIONING_ERROR;
  ( void ) osal_mutex_give( s_mutex );
  ( void ) osal_mutex_give( s_lifecycle_mutex );
  return ok;
}

wifi_http_provisioning_state_t wifi_http_provisioning_get_state( void )
{
  wifi_http_provisioning_state_t st = WIFI_PROVISIONING_STOPPED;

  if ( prov_ensure_mutex() )
  {
    ( void ) osal_mutex_take( s_mutex );
    st = s_state;
    ( void ) osal_mutex_give( s_mutex );
  }
  return st;
}

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
void wifi_http_provisioning_test_set_stop_boundary_hook( void ( *hook )( void ) )
{
  s_stop_boundary_hook = hook;
}
#endif

bool wifi_http_provisioning_set_http_url( const char * url )
{
  size_t len;

  if ( !prov_ensure_mutex() || url == NULL ) return false;
  len = strlen( url );
  if ( len == 0 || len >= WIFI_PROVISIONING_URL_MAX_LEN ) return false;

  ( void ) osal_mutex_take( s_mutex );
  if ( s_state == WIFI_PROVISIONING_RUNNING ||
       s_state == WIFI_PROVISIONING_STARTING ||
       s_state == WIFI_PROVISIONING_STOPPING )
  {
    ( void ) osal_mutex_give( s_mutex );
    return false;
  }
  memcpy( s_http_url, url, len );
  s_http_url[len] = '\0';
  s_http_configured = true;
  ( void ) osal_mutex_give( s_mutex );
  return true;
}

bool wifi_http_provisioning_set_dns_url( const char * url )
{
  size_t len;

  if ( !prov_ensure_mutex() || url == NULL ) return false;
  len = strlen( url );
  if ( len == 0 || len >= WIFI_PROVISIONING_URL_MAX_LEN ) return false;

  ( void ) osal_mutex_take( s_mutex );
  if ( s_state == WIFI_PROVISIONING_RUNNING ||
       s_state == WIFI_PROVISIONING_STARTING ||
       s_state == WIFI_PROVISIONING_STOPPING )
  {
    ( void ) osal_mutex_give( s_mutex );
    return false;
  }
  memcpy( s_dns_url, url, len );
  s_dns_url[len] = '\0';
  s_dns_configured = true;
  ( void ) osal_mutex_give( s_mutex );
  return true;
}