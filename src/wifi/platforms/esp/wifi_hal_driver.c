#include "wifi_hal_driver.h"

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "osal_bin_sem.h"
#include "osal_log.h"
#include "osal_task.h"
#include <string.h>

typedef struct
{
  bool initialized;
  bool started;
  /* Separate tracking per handler registration: a single bit cannot tell which
   * of the two registrations succeeded.  Each flag is set only after the
   * matching esp_event_handler_register() returned ESP_OK and cleared only
   * when its unregister actually succeeded (or the handler proved absent), so
   * an unwind/deinit retry always knows exactly which handler to re-attempt. */
  bool handler_wifi_registered;  /**< True once the WIFI_EVENT handler is installed.    */
  bool handler_ip_registered;    /**< True once the IP_EVENT STA_GOT_IP handler is installed. */
  bool wifi_inited;           /**< True once esp_wifi_init() succeeded.         */
  bool netif_sta_created;     /**< True while netif_sta must be destroyed.      */
  bool netif_ap_created;      /**< True while netif_ap must be destroyed.       */
  bool cb_delivery_disabled;  /**< Deinit disabled new callback deliveries.     */
  uint32_t cb_in_flight;      /**< Number of callbacks currently executing.     */
  wifi_hal_event_cb_t cb;
  void* cb_user_data;
  esp_netif_t* netif_sta;
  esp_netif_t* netif_ap;
  wifi_hal_sta_config_t sta_cfg;
  wifi_hal_ap_config_t ap_cfg;
  uint32_t client_count;
  /* Guards cb, cb_user_data, cb_delivery_disabled and cb_in_flight.  The
   * callback itself is always invoked outside this lock so callback code may
   * re-enter management APIs without deadlocking. */
  osal_bin_sem_id_t cb_lock;
} wifi_hal_ctx_t;

static wifi_hal_ctx_t g_wifi_hal_ctx = { 0 };
static wifi_ap_record_t scan_ap_records[64] = { 0 };

static osal_status_t _esp_to_status( esp_err_t err )
{
  if ( err == ESP_OK )
  {
    return OSAL_SUCCESS;
  }

  return OSAL_ERROR;
}

bool wifi_hal_is_valid_ipv4( const char* str )
{
  if ( !str || strlen( str ) == 0 || strlen( str ) > 15 )
  {
    return false;
  }

  int  octets = 0;
  int  value  = 0;
  int  digits = 0;
  for ( size_t i = 0; i < strlen( str ); ++i )
  {
    char c = str[i];
    if ( c >= '0' && c <= '9' )
    {
      value = value * 10 + ( c - '0' );
      if ( value > 255 )
      {
        return false;
      }
      digits++;
      if ( digits > 3 )
      {
        return false;
      }
    }
    else if ( c == '.' )
    {
      if ( digits == 0 )
      {
        return false; /* empty octet */
      }
      octets++;
      value  = 0;
      digits = 0;
    }
    else
    {
      return false; /* invalid character */
    }
  }

  if ( digits == 0 )
  {
    return false;
  }
  return octets == 3;
}

static void _emit_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data )
{
  wifi_hal_event_cb_t cb;
  void*               user_data;

  (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_lock );

  /* Deinit either disabled delivery or will do so under this lock; a delivery
   * that starts after the disabled flag is seen drops the event. */
  if ( g_wifi_hal_ctx.cb_delivery_disabled || g_wifi_hal_ctx.cb == NULL )
  {
    (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );
    return;
  }

  cb        = g_wifi_hal_ctx.cb;
  user_data = g_wifi_hal_ctx.cb_user_data;
  g_wifi_hal_ctx.cb_in_flight++;
  (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );

  /* Invoke outside the callback lock so the callback may re-enter management
   * APIs (including HAL calls that emit further events) without deadlocking. */
  cb( event, data, user_data );

  (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_lock );
  g_wifi_hal_ctx.cb_in_flight--;
  (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );
}

/* Disable callback delivery, wait for every in-flight callback to return and
 * clear the callback/user-data pointers.  The event handlers must already be
 * unregistered (so no new handler can start) and cb_lock must still exist. */
static void _quiesce_callbacks( void )
{
  (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_lock );
  g_wifi_hal_ctx.cb_delivery_disabled = true;
  (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );

  /* Wait for every callback that was already in flight to return.  A callback
   * that started before the disabled flag was set always decrements the
   * counter; a callback that starts after it drops the event and returns. */
  for ( ;; )
  {
    uint32_t in_flight = 0;
    (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_lock );
    in_flight = g_wifi_hal_ctx.cb_in_flight;
    (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );
    if ( in_flight == 0 )
    {
      break;
    }
    (void) osal_task_delay_ms( 1 );
  }

  (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_lock );
  g_wifi_hal_ctx.cb           = NULL;
  g_wifi_hal_ctx.cb_user_data = NULL;
  (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );
}

static void _wifi_event_handler( void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data )
{
  (void) arg;

  if ( event_base == WIFI_EVENT )
  {
    if ( event_id == WIFI_EVENT_STA_DISCONNECTED )
    {
      wifi_hal_event_data_t event = { 0 };
      wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
      event.disconnect_reason = disconnected->reason;
      _emit_event( WIFI_HAL_EVT_STA_DISCONNECTED, &event );
      return;
    }

    if ( event_id == WIFI_EVENT_SCAN_DONE )
    {
      _emit_event( WIFI_HAL_EVT_SCAN_DONE, NULL );
      return;
    }

    if ( event_id == WIFI_EVENT_AP_STACONNECTED )
    {
      g_wifi_hal_ctx.client_count++;
      _emit_event( WIFI_HAL_EVT_AP_CLIENT_CONNECTED, NULL );
      return;
    }

    if ( event_id == WIFI_EVENT_AP_STADISCONNECTED )
    {
      if ( g_wifi_hal_ctx.client_count > 0 )
      {
        g_wifi_hal_ctx.client_count--;
      }
      _emit_event( WIFI_HAL_EVT_AP_CLIENT_DISCONNECTED, NULL );
      return;
    }
  }

  if ( event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP )
  {
    wifi_hal_event_data_t event = { 0 };
    ip_event_got_ip_t* got_ip = (ip_event_got_ip_t*) event_data;
    esp_ip4addr_ntoa( &got_ip->ip_info.ip, event.ip_info.ip, sizeof( event.ip_info.ip ) );
    esp_ip4addr_ntoa( &got_ip->ip_info.netmask, event.ip_info.netmask, sizeof( event.ip_info.netmask ) );
    esp_ip4addr_ntoa( &got_ip->ip_info.gw, event.ip_info.gw, sizeof( event.ip_info.gw ) );
    _emit_event( WIFI_HAL_EVT_STA_GOT_IP, &event );
  }
}

static void _copy_sta_config( wifi_config_t* out, const wifi_hal_sta_config_t* in )
{
  memset( out, 0, sizeof( *out ) );
  strncpy( (char*) out->sta.ssid, in->ssid, sizeof( out->sta.ssid ) - 1 );
  strncpy( (char*) out->sta.password, in->password, sizeof( out->sta.password ) - 1 );
  /* Accept open APs when password is empty; otherwise allow WPA2 and stronger. */
  out->sta.threshold.authmode = ( in->password[0] == '\0' ) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  out->sta.pmf_cfg.capable = true;
}

static void _copy_ap_config( wifi_config_t* out, const wifi_hal_ap_config_t* in )
{
  memset( out, 0, sizeof( *out ) );
  strncpy( (char*) out->ap.ssid, in->ssid, sizeof( out->ap.ssid ) - 1 );
  strncpy( (char*) out->ap.password, in->password, sizeof( out->ap.password ) - 1 );
  out->ap.ssid_len = strlen( in->ssid );
  out->ap.max_connection = in->max_connection;
  out->ap.authmode = (wifi_auth_mode_t) in->authmode;
}

/**
 * @brief   Configure the DNS address advertised by the soft-AP DHCP server.
 *
 *          Points the AP interface's own resolver at the captive DNS address
 *          and enables the ESP DHCP server's @c DOMAIN_NAME_SERVER offer so it
 *          advertises that DNS to DHCP clients.  Must be called while the AP
 *          DHCP server is stopped so the DHCP options are applied before the
 *          server is restarted.  All ESP-IDF calls stay inside this ESP HAL.
 *
 * @param   [in] dns_str - dotted-decimal IPv4 DNS address to advertise
 * @return  OSAL_SUCCESS on success, OSAL_ERR_INVALID_ARGUMENT for a malformed
 *          address, or OSAL_ERROR when the DHCP server rejects the option.
 */
static osal_status_t _configure_ap_dns( const char* dns_str )
{
  if ( !wifi_hal_is_valid_ipv4( dns_str ) )
  {
    return OSAL_ERR_INVALID_ARGUMENT;
  }

  esp_ip4_addr_t dns_ip = { 0 };
  if ( esp_netif_str_to_ip4( dns_str, &dns_ip ) != ESP_OK )
  {
    return OSAL_ERR_INVALID_ARGUMENT;
  }

  /* Point the soft-AP interface's own DNS resolver at the captive DNS.  For
   * an interface with a DHCP server this stores the address the server will
   * advertise to clients. */
  esp_netif_dns_info_t dns_info = { 0 };
  dns_info.ip.type = ESP_IPADDR_TYPE_V4;
  dns_info.ip.u_addr.ip4 = dns_ip;
  if ( esp_netif_set_dns_info( g_wifi_hal_ctx.netif_ap, ESP_NETIF_DNS_MAIN,
                               &dns_info ) != ESP_OK )
  {
    return OSAL_ERROR;
  }

  /* Enable the DHCP server's DNS option (OFFER_DNS) so it actually advertises
   * the DNS address configured above. */
  uint8_t offer = 1;
  if ( esp_netif_dhcps_option( g_wifi_hal_ctx.netif_ap, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER, &offer,
                               sizeof( offer ) ) != ESP_OK )
  {
    return OSAL_ERROR;
  }

  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_init( const wifi_hal_init_t* init )
{
  esp_err_t     err;
  osal_status_t st = OSAL_SUCCESS;

  if ( !init || !init->event_cb )
  {
    return OSAL_INVALID_POINTER;
  }

  if ( g_wifi_hal_ctx.initialized )
  {
    /* Idempotent: a repeated init of an already initialized session must not
     * recreate live resources (netifs, stack, handlers, callback gate). */
    return OSAL_SUCCESS;
  }

  memset( &g_wifi_hal_ctx, 0, sizeof( g_wifi_hal_ctx ) );

  /* Callback delivery gate — created first so every unwind path can delete it
   * and every delivery is protected from the very beginning. */
  if ( osal_bin_sem_create( &g_wifi_hal_ctx.cb_lock, "wifi_hal_cb", OSAL_SEM_FULL ) != OSAL_SUCCESS )
  {
    return OSAL_ERROR;
  }

  g_wifi_hal_ctx.cb           = init->event_cb;
  g_wifi_hal_ctx.cb_user_data = init->user_data;

  err = esp_netif_init();
  if ( err != ESP_OK && err != ESP_ERR_INVALID_STATE )
  {
    st = _esp_to_status( err );
    goto unwind;
  }

  err = esp_event_loop_create_default();
  if ( err != ESP_OK && err != ESP_ERR_INVALID_STATE )
  {
    st = _esp_to_status( err );
    goto unwind;
  }

  g_wifi_hal_ctx.netif_ap = esp_netif_create_default_wifi_ap();
  if ( !g_wifi_hal_ctx.netif_ap )
  {
    /* A missing default netif is an initialization failure: unwinding below
     * releases whatever was created so far, and netif_ap is never dereferenced
     * by the AP configuration code. */
    st = OSAL_ERROR;
    goto unwind;
  }
  g_wifi_hal_ctx.netif_ap_created = true;

  g_wifi_hal_ctx.netif_sta = esp_netif_create_default_wifi_sta();
  if ( !g_wifi_hal_ctx.netif_sta )
  {
    st = OSAL_ERROR;
    goto unwind;
  }
  g_wifi_hal_ctx.netif_sta_created = true;

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.nvs_enable = false;
  err = esp_wifi_init( &cfg );
  if ( err != ESP_OK )
  {
    st = _esp_to_status( err );
    goto unwind;
  }
  g_wifi_hal_ctx.wifi_inited = true;

  if ( init->ap_ip && init->ap_gateway && init->ap_netmask )
  {
    /* Stop any already-running AP DHCP server first so the AP network options
     * (including the optional captive DNS advertisement) are applied before
     * the server is restarted below. */
    esp_err_t dhcp_err = esp_netif_dhcps_stop( g_wifi_hal_ctx.netif_ap );
    if ( dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED )
    {
      st = OSAL_ERROR;
      goto unwind;
    }

    esp_netif_ip_info_t ap_ip_info = { 0 };
    inet_pton( AF_INET, init->ap_ip, &ap_ip_info.ip );
    inet_pton( AF_INET, init->ap_gateway, &ap_ip_info.gw );
    inet_pton( AF_INET, init->ap_netmask, &ap_ip_info.netmask );

    if ( esp_netif_set_ip_info( g_wifi_hal_ctx.netif_ap, &ap_ip_info ) != ESP_OK )
    {
      st = OSAL_ERROR;
      goto unwind;
    }

    /* Optional captive DNS override advertised by the AP DHCP server.  When
     * omitted (non-captive use), the platform keeps its default DNS behaviour.
     * The DNS requirement is applied while the DHCP server is stopped. */
    if ( init->ap_dns && init->ap_dns[0] != '\0' )
    {
      st = _configure_ap_dns( init->ap_dns );
      if ( st != OSAL_SUCCESS )
      {
        goto unwind;
      }
    }

    if ( esp_netif_dhcps_start( g_wifi_hal_ctx.netif_ap ) != ESP_OK )
    {
      st = OSAL_ERROR;
      goto unwind;
    }
  }
  else if ( init->ap_dns && init->ap_dns[0] != '\0' )
  {
    /* Advertising a captive DNS requires the AP IP/DHCP configuration, so
     * reject the combination rather than silently dropping the request. */
    st = OSAL_ERR_INVALID_ARGUMENT;
    goto unwind;
  }

  err = esp_event_handler_register( WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler, NULL );
  if ( err != ESP_OK )
  {
    /* Only a real registration success counts: ESP_ERR_INVALID_ARG here means
     * the handler was not registered, which must fail this init, not silently
     * produce an "initialized" HAL with a missing event source. */
    st = _esp_to_status( err );
    goto unwind;
  }
  g_wifi_hal_ctx.handler_wifi_registered = true;

  err = esp_event_handler_register( IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler, NULL );
  if ( err != ESP_OK )
  {
    st = _esp_to_status( err );
    goto unwind;
  }
  g_wifi_hal_ctx.handler_ip_registered = true;

  g_wifi_hal_ctx.initialized = true;
  return OSAL_SUCCESS;

unwind:
  /* Unwind every partial-init failure: unregister the handlers (if any were
   * registered) so no callback can be delivered after we return, disable and
   * drain callback delivery, clear the callback state, then release the
   * Wi-Fi stack and the default netifs that were actually created.  The
   * session is left fully uninitialized so a later wifi_hal_deinit() is a
   * safe no-op and a retried init starts from clean state.  If any release
   * cannot be completed (for example a handler that is still registered
   * because the event loop is busy), the session flag and the callback gate
   * are kept so a later wifi_hal_deinit() retries the release; callback
   * delivery is disabled either way so no stale callback can reach the user.
   */
  {
    bool unwind_incomplete = false;

  /* Unregister each handler individually, keyed by its own registration flag.
   * A handler that was never registered (flag clear, e.g. the second
   * registration failed) is skipped entirely instead of producing a bogus
   * ESP_ERR_NOT_FOUND failure on every retry.  A handler whose unregister
   * reports it as already absent (ESP_ERR_NOT_FOUND) or as un-registrable
   * (ESP_ERR_INVALID_ARG) is treated as gone and its flag is cleared. */
  if ( g_wifi_hal_ctx.handler_wifi_registered )
  {
    esp_err_t uerr = esp_event_handler_unregister( WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler );
    if ( uerr == ESP_OK || uerr == ESP_ERR_NOT_FOUND || uerr == ESP_ERR_INVALID_ARG )
    {
      g_wifi_hal_ctx.handler_wifi_registered = false;
    }
    else
    {
      /* The WIFI handler may still be registered; it must stay tracked so a
       * later deinit retries the unregistration before any new init runs. */
      unwind_incomplete = true;
    }
  }
  if ( g_wifi_hal_ctx.handler_ip_registered )
  {
    esp_err_t uerr = esp_event_handler_unregister( IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler );
    if ( uerr == ESP_OK || uerr == ESP_ERR_NOT_FOUND || uerr == ESP_ERR_INVALID_ARG )
    {
      g_wifi_hal_ctx.handler_ip_registered = false;
    }
    else
    {
      unwind_incomplete = true;
    }
  }

  /* Disable new deliveries, wait for any already-running callback and clear the
   * pointers while the callback gate still exists.  Even if a handler could not
   * be unregistered, it can no longer reach the user through this gate. */
  _quiesce_callbacks();

  /* Release the Wi-Fi stack.  esp_wifi_stop() is only meaningful after
   * esp_wifi_init() succeeded: calling it on an uninitialized stack returns
   * ESP_ERR_WIFI_NOT_INIT, which must not be misread as an incomplete unwind
   * that keeps the session alive.  The init path never starts the stack, so
   * this is normally an ESP_ERR_WIFI_NOT_STARTED no-op — kept as a defensive
   * stop for any stack that turned itself on during init. */
  if ( g_wifi_hal_ctx.wifi_inited )
  {
    esp_err_t serr = esp_wifi_stop();
    if ( serr != ESP_OK && serr != ESP_ERR_WIFI_NOT_STARTED )
    {
      unwind_incomplete = true;
    }
  }
  if ( g_wifi_hal_ctx.wifi_inited )
  {
    esp_err_t derr = esp_wifi_deinit();
    if ( derr != ESP_OK && derr != ESP_ERR_WIFI_NOT_INIT )
    {
      unwind_incomplete = true;
    }
    else
    {
      g_wifi_hal_ctx.wifi_inited = false;
    }
  }

  if ( g_wifi_hal_ctx.netif_sta_created && g_wifi_hal_ctx.netif_sta )
  {
    esp_netif_destroy_default_wifi( g_wifi_hal_ctx.netif_sta );
    g_wifi_hal_ctx.netif_sta = NULL;
    g_wifi_hal_ctx.netif_sta_created = false;
  }

  if ( g_wifi_hal_ctx.netif_ap_created && g_wifi_hal_ctx.netif_ap )
  {
    esp_netif_destroy_default_wifi( g_wifi_hal_ctx.netif_ap );
    g_wifi_hal_ctx.netif_ap = NULL;
    g_wifi_hal_ctx.netif_ap_created = false;
  }

  if ( unwind_incomplete )
  {
    /* Safe-retry state: keep the session flag and the callback gate so a later
     * wifi_hal_deinit() completes the release.  Delivery is disabled and the
     * callback pointers are cleared, so callback delivery is quiescent. */
    g_wifi_hal_ctx.initialized = true;
    osal_log_error( "[wifi-hal] init unwind incomplete, retry via deinit" );
    return st;
  }

  if ( g_wifi_hal_ctx.cb_lock )
  {
    if ( osal_bin_sem_delete( g_wifi_hal_ctx.cb_lock ) != OSAL_SUCCESS )
    {
      /* The callback gate could not be released: keep it (and the session
       * flag) so a later wifi_hal_deinit() re-attempts the deletion.  Delivery
       * is already disabled and the callback pointers are cleared, so the
       * retained gate is quiescent. */
      g_wifi_hal_ctx.initialized = true;
      osal_log_error( "[wifi-hal] init unwind incomplete (callback gate retained), "
                      "retry via deinit" );
      return st;
    }
    g_wifi_hal_ctx.cb_lock = NULL;
  }

  memset( &g_wifi_hal_ctx, 0, sizeof( g_wifi_hal_ctx ) );
  return st;
  }
}

osal_status_t wifi_hal_deinit( void )
{
  osal_status_t result = OSAL_SUCCESS;
  esp_err_t     err;

  if ( !g_wifi_hal_ctx.initialized )
  {
    /* Uninitialized and repeated deinit are successful no-ops. */
    return OSAL_SUCCESS;
  }

  /* 1. Unregister the event handlers before callback quiescence.  On top of
   *    removing the event source, the event loop dispatches handlers while
   *    holding its own loop lock, so unregistering also waits for a handler
   *    that is currently running to return.  Each handler is unregistered
   *    individually, keyed by its own registration flag: skipping a handler
   *    whose flag is already clear keeps a deinit retry from reporting a bogus
   *    ESP_ERR_NOT_FOUND for a handler that never existed (or was already
   *    removed) and therefore never returning success.  A flag is cleared
   *    only when the matching handler is confirmed gone; otherwise it stays
   *    set so no later init can run while the handler is still registered. */
  if ( g_wifi_hal_ctx.handler_wifi_registered )
  {
    err = esp_event_handler_unregister( WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler );
    if ( err != ESP_OK && err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_ARG )
    {
      result = OSAL_ERROR;
    }
    else
    {
      g_wifi_hal_ctx.handler_wifi_registered = false;
    }
  }
  if ( g_wifi_hal_ctx.handler_ip_registered )
  {
    err = esp_event_handler_unregister( IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler );
    if ( err != ESP_OK && err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_ARG )
    {
      result = OSAL_ERROR;
    }
    else
    {
      g_wifi_hal_ctx.handler_ip_registered = false;
    }
  }

  /* 2. Callback quiescence: disable new deliveries, wait for every callback
   *    already in flight to return, and clear the callback/user-data
   *    pointers.  From this point on no later callback can begin. */
  _quiesce_callbacks();

  /* 3. Release the Wi-Fi stack.  The state flags are cleared only when the
   *    release actually happened (or was already done), so that when a release
   *    fails the retained lifecycle state lets a retry of wifi_hal_deinit()
   *    attempt it again instead of skipping it. */
  if ( g_wifi_hal_ctx.started )
  {
    err = esp_wifi_stop();
    if ( err == ESP_OK || err == ESP_ERR_WIFI_NOT_STARTED )
    {
      g_wifi_hal_ctx.started = false;
    }
    else
    {
      result = OSAL_ERROR;
    }
  }

  if ( g_wifi_hal_ctx.wifi_inited )
  {
    err = esp_wifi_deinit();
    if ( err == ESP_OK || err == ESP_ERR_WIFI_NOT_INIT )
    {
      g_wifi_hal_ctx.wifi_inited = false;
    }
    else
    {
      result = OSAL_ERROR;
    }
  }

  /* 4. Destroy the default Wi-Fi netifs exactly once. */
  if ( g_wifi_hal_ctx.netif_sta_created && g_wifi_hal_ctx.netif_sta )
  {
    esp_netif_destroy_default_wifi( g_wifi_hal_ctx.netif_sta );
    g_wifi_hal_ctx.netif_sta = NULL;
    g_wifi_hal_ctx.netif_sta_created = false;
  }

  if ( g_wifi_hal_ctx.netif_ap_created && g_wifi_hal_ctx.netif_ap )
  {
    esp_netif_destroy_default_wifi( g_wifi_hal_ctx.netif_ap );
    g_wifi_hal_ctx.netif_ap = NULL;
    g_wifi_hal_ctx.netif_ap_created = false;
  }

  if ( result != OSAL_SUCCESS )
  {
    /* Teardown could not release a platform resource.  Callback delivery is
     * already disabled and the callback pointers are cleared; keep the
     * session flag and the callback gate so a retry of wifi_hal_deinit()
     * may attempt the release again. */
    g_wifi_hal_ctx.initialized = true;
    osal_log_error( "[wifi-hal] deinit: partial teardown, retry required" );
    return OSAL_ERROR;
  }

  if ( g_wifi_hal_ctx.cb_lock )
  {
    if ( osal_bin_sem_delete( g_wifi_hal_ctx.cb_lock ) != OSAL_SUCCESS )
    {
      /* The callback gate could not be released.  Do NOT zero the context:
       * keep the session flag and the cb_lock handle so a retry of
       * wifi_hal_deinit() can attempt the deletion again.  Callback delivery
       * is already disabled and the pointers are cleared, so the retained
       * gate stays quiescent. */
      g_wifi_hal_ctx.initialized = true;
      osal_log_error( "[wifi-hal] deinit: failed to delete callback gate, "
                      "retry required" );
      return OSAL_ERROR;
    }
    g_wifi_hal_ctx.cb_lock = NULL;
  }

  g_wifi_hal_ctx.initialized = false;
  memset( &g_wifi_hal_ctx, 0, sizeof( g_wifi_hal_ctx ) );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_start( wifi_hal_mode_t mode )
{
  wifi_mode_t esp_mode = WIFI_MODE_STA;

  if ( mode == WIFI_HAL_MODE_AP )
  {
    esp_mode = WIFI_MODE_AP;
  }
  else if ( mode == WIFI_HAL_MODE_APSTA )
  {
    esp_mode = WIFI_MODE_APSTA;
  }

  if ( esp_wifi_set_mode( esp_mode ) != ESP_OK )
  {
    return OSAL_ERROR;
  }

  if ( mode == WIFI_HAL_MODE_AP || mode == WIFI_HAL_MODE_APSTA )
  {
    wifi_config_t ap_cfg;
    _copy_ap_config( &ap_cfg, &g_wifi_hal_ctx.ap_cfg );
    if ( esp_wifi_set_config( WIFI_IF_AP, &ap_cfg ) != ESP_OK )
    {
      return OSAL_ERROR;
    }
  }

  if ( mode == WIFI_HAL_MODE_STA || mode == WIFI_HAL_MODE_APSTA )
  {
    wifi_config_t sta_cfg;
    _copy_sta_config( &sta_cfg, &g_wifi_hal_ctx.sta_cfg );
    if ( esp_wifi_set_config( WIFI_IF_STA, &sta_cfg ) != ESP_OK )
    {
      return OSAL_ERROR;
    }
  }

  if ( esp_wifi_start() != ESP_OK )
  {
    return OSAL_ERROR;
  }

  g_wifi_hal_ctx.started = true;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_stop( void )
{
  if ( !g_wifi_hal_ctx.started )
  {
    return OSAL_SUCCESS;
  }

  if ( esp_wifi_stop() != ESP_OK )
  {
    return OSAL_ERROR;
  }

  g_wifi_hal_ctx.started = false;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_sta_config( const wifi_hal_sta_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  memset( &g_wifi_hal_ctx.sta_cfg, 0, sizeof( g_wifi_hal_ctx.sta_cfg ) );
  strncpy( g_wifi_hal_ctx.sta_cfg.ssid, config->ssid, WIFI_HAL_SSID_MAX_LEN );
  strncpy( g_wifi_hal_ctx.sta_cfg.password, config->password, WIFI_HAL_PASSWORD_MAX_LEN );
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_ap_config( const wifi_hal_ap_config_t* config )
{
  if ( !config )
  {
    return OSAL_INVALID_POINTER;
  }

  memset( &g_wifi_hal_ctx.ap_cfg, 0, sizeof( g_wifi_hal_ctx.ap_cfg ) );
  strncpy( g_wifi_hal_ctx.ap_cfg.ssid, config->ssid, WIFI_HAL_SSID_MAX_LEN );
  strncpy( g_wifi_hal_ctx.ap_cfg.password, config->password, WIFI_HAL_PASSWORD_MAX_LEN );
  g_wifi_hal_ctx.ap_cfg.max_connection = config->max_connection;
  g_wifi_hal_ctx.ap_cfg.authmode = config->authmode;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_connect( void )
{
  size_t pass_len = strlen( g_wifi_hal_ctx.sta_cfg.password );
  osal_log_debug( "[wifi-hal] connect requested: ssid='%s', pass_len=%u",
                  g_wifi_hal_ctx.sta_cfg.ssid,
                  (unsigned) pass_len );

  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_err_t err = esp_wifi_get_mode( &mode );
  if ( err != ESP_OK )
  {
    osal_log_error( "[wifi-hal] esp_wifi_get_mode failed: %s (0x%x)",
                    esp_err_to_name( err ),
                    (unsigned) err );
    return OSAL_ERROR;
  }

  osal_log_debug( "[wifi-hal] current mode=%d", (int) mode );

  if ( mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA )
  {
    osal_log_error( "[wifi-hal] invalid mode for connect: %d", (int) mode );
    return OSAL_ERROR;
  }

  /* Always apply the latest credentials before attempting a connection. */
  wifi_config_t sta_cfg;
  _copy_sta_config( &sta_cfg, &g_wifi_hal_ctx.sta_cfg );
  err = esp_wifi_set_config( WIFI_IF_STA, &sta_cfg );
  if ( err != ESP_OK )
  {
    osal_log_error( "[wifi-hal] esp_wifi_set_config failed for ssid='%s': %s (0x%x)",
                    g_wifi_hal_ctx.sta_cfg.ssid,
                    esp_err_to_name( err ),
                    (unsigned) err );
    return OSAL_ERROR;
  }

  osal_log_debug( "[wifi-hal] STA config applied, connecting..." );

  err = esp_wifi_connect();
  if ( err != ESP_OK )
  {
    osal_log_error( "[wifi-hal] esp_wifi_connect failed: %s (0x%x)",
                    esp_err_to_name( err ),
                    (unsigned) err );
    return OSAL_ERROR;
  }

  osal_log_debug( "[wifi-hal] esp_wifi_connect accepted" );

  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_disconnect( void )
{
  return _esp_to_status( esp_wifi_disconnect() );
}

osal_status_t wifi_hal_start_scan( bool block )
{
  wifi_scan_config_t config = { 0 };
  return _esp_to_status( esp_wifi_scan_start( &config, block ) );
}

osal_status_t wifi_hal_get_scanned_ap( wifi_hal_ap_record_t* records, uint16_t* in_out_count )
{
  if ( !records || !in_out_count )
  {
    return OSAL_INVALID_POINTER;
  }

  uint16_t count = *in_out_count;
  if ( count > 64 )
  {
    count = 64;
  }

  esp_err_t err = esp_wifi_scan_get_ap_records( &count, scan_ap_records );
  if ( err != ESP_OK )
  {
    return OSAL_ERROR;
  }

  for ( uint16_t i = 0; i < count; ++i )
  {
    memset( &records[i], 0, sizeof( records[i] ) );
    strncpy( records[i].ssid, (const char*) scan_ap_records[i].ssid, WIFI_HAL_SSID_MAX_LEN );
    records[i].channel = scan_ap_records[i].primary;
    records[i].rssi = scan_ap_records[i].rssi;
    records[i].authmode = scan_ap_records[i].authmode;
  }

  *in_out_count = count;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_ip_info( wifi_hal_ip_info_t* out_info )
{
  if ( !out_info )
  {
    return OSAL_INVALID_POINTER;
  }

  esp_netif_ip_info_t ip_info = { 0 };
  esp_err_t err = esp_netif_get_ip_info( g_wifi_hal_ctx.netif_sta, &ip_info );
  if ( err != ESP_OK )
  {
    return OSAL_ERROR;
  }

  esp_ip4addr_ntoa( &ip_info.ip, out_info->ip, sizeof( out_info->ip ) );
  esp_ip4addr_ntoa( &ip_info.netmask, out_info->netmask, sizeof( out_info->netmask ) );
  esp_ip4addr_ntoa( &ip_info.gw, out_info->gw, sizeof( out_info->gw ) );

  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_get_sta_rssi( int* out_rssi )
{
  if ( !out_rssi )
  {
    return OSAL_INVALID_POINTER;
  }

  wifi_ap_record_t ap_info = { 0 };
  esp_err_t err = esp_wifi_sta_get_ap_info( &ap_info );
  if ( err != ESP_OK )
  {
    return OSAL_ERROR;
  }

  *out_rssi = ap_info.rssi;
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_set_power_save( bool enabled )
{
  return _esp_to_status( esp_wifi_set_ps( enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE ) );
}

osal_status_t wifi_hal_get_default_mac( uint8_t mac[6] )
{
  if ( !mac )
  {
    return OSAL_INVALID_POINTER;
  }

  return _esp_to_status( esp_efuse_mac_get_default( mac ) );
}

osal_status_t wifi_hal_get_client_count( uint32_t* out_client_count )
{
  if ( !out_client_count )
  {
    return OSAL_INVALID_POINTER;
  }

  *out_client_count = g_wifi_hal_ctx.client_count;
  return OSAL_SUCCESS;
}
