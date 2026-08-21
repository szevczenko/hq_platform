/*
 * Wi-Fi provisioning automatic fallback controller - implementation.
 *
 * See wifi_provisioning_controller.h for the public contract. The controller
 * is a small policy layer: it decides when the provisioning application
 * should be opened automatically, and it delegates all listener work to the
 * explicit wifi_http_provisioning API.
 *
 * Automatic fallback applied here:
 *   - no saved credential exists           -> start immediately on init;
 *   - saved credentials are exhausted      -> start on CONNECT_FAILED;
 *   - a single transient disconnect        -> never reopen (DISCONNECTED only);
 *   - successful saved-credential connect  -> no portal, go ONLINE.
 *
 * Success grace-period behavior:
 *   - a submitted credential connects (station IP acquisition) while the
 *     portal is active -> keep AP, HTTP and DNS up and start the configured
 *     success grace timer;
 *   - the connection fails before expiry  -> cancel the timer and keep the
 *     portal open for another, later attempt;
 *   - the grace timer expires or an explicit stop is issued -> stop the
 *     listeners, then request a STA-only transition, so the temporary AP is
 *     retired only after the portal listeners shut down.
 *
 * The controller subscribes once per lifetime and starts provisioning at most
 * once per qualifying fallback (guarded by a per-session flag). Deinit
 * cancels the grace timer, unsubscribes every registered callback, and
 * returns to DISABLED.
 */

#include "hq_config.h"
#include "osal_timer.h"
#include "wifi_managment.h"
#include "wifi_http_provisioning.h"
#include "wifi_provisioning_controller.h"

#ifdef CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK

/* Default success grace period when the Kconfig option is not wired into the
 * build configuration (the portal then shuts down immediately on success). */
#ifndef CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS
#define CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS 0u
#endif

/* Private state --------------------------------------------------------- */

typedef struct
{
  wifi_provisioning_controller_state_t state;            /* Current policy state. */
  bool                                 enabled;          /* Init gate (idempotent). */
  bool                                 fallback_started;  /* Start-once guard. */
  uint32_t                             grace_ms;          /* Overridable grace period. */
  osal_timer_id_t                      grace_timer;       /* Success grace timer. */
  bool                                 grace_timer_ready; /* Timer created at init. */
  bool                                 grace_timer_active;/* Timer currently armed. */
} wifi_controller_ctx_t;

static wifi_controller_ctx_t s_ctx = {
  WIFI_PROVISIONING_CONTROLLER_DISABLED,             /* state */
  false,                                             /* enabled */
  false,                                             /* fallback_started */
  CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS,    /* grace_ms */
  NULL,                                              /* grace_timer */
  false,                                             /* grace_timer_ready */
  false                                              /* grace_timer_active */
};

/* Private helpers ------------------------------------------------------- */

/* Shut the portal down. The listeners are closed first and only then is a
 * call made to go STA-only, so the temporary AP is never retired while HTTP
 * or DNS is still exposing the portal. Returns to ONLINE on success, or
 * keeps the portal available for another attempt when the shutdown fails. */
static void _shutdown_provisioning( void )
{
  s_ctx.grace_timer_active = false;
  if ( wifi_http_provisioning_stop() )
  {
    (void) wifi_mgmt_request_mode( T_WIFI_TYPE_CLIENT );
    s_ctx.state = WIFI_PROVISIONING_CONTROLLER_ONLINE;
  }
  else
  {
    s_ctx.state = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
  }
}

/* Cancel an armed grace timer (if any). Safe when the timer is not active. */
static void _cancel_grace_timer( void )
{
  if ( s_ctx.grace_timer_active && s_ctx.grace_timer_ready )
  {
    (void) osal_timer_stop( s_ctx.grace_timer, 0u );
  }
  s_ctx.grace_timer_active = false;
}

/* Grace timer expiry callback, delivered on the timer task. Only a portal
 * that is still inside the grace window is shut down; a state change after
 * expiry, a disconnect, or an explicit stop makes this a no-op. */
static void _on_grace_timer_expired( osal_timer_id_t timer_id )
{
  (void) timer_id;
  if ( s_ctx.state != WIFI_PROVISIONING_CONTROLLER_GRACE ) return;
  _shutdown_provisioning();
}

/* Start the success grace window. A zero period is a direct hint: shut the
 * portal down immediately and go ONLINE. When a timer is available it is
 * re-armed for the configured duration and the controller enters GRACE. */
static void _start_grace( void )
{
  if ( s_ctx.grace_ms == 0u )
  {
    _shutdown_provisioning();
    return;
  }

  s_ctx.state = WIFI_PROVISIONING_CONTROLLER_GRACE;
  if ( s_ctx.grace_timer_ready && !s_ctx.grace_timer_active )
  {
    (void) osal_timer_change_period( s_ctx.grace_timer, s_ctx.grace_ms, 0u );
    (void) osal_timer_start( s_ctx.grace_timer, 0u );
    s_ctx.grace_timer_active = true;
  }
}

/* Event callback delivered on the Wi-Fi management event thread. The
 * controller owns a single policy instance; the user_data argument is not
 * used by this module. */
static void _controller_on_event( wifi_mgmt_event_t event, void* user_data )
{
  (void) user_data;

  switch ( event )
  {
    case WIFI_MGMT_EVENT_CONNECTED:
      /* If the station obtained an IP while the portal is active, a
       * submitted credential succeeded: enter the grace window and keep the
       * portal available until the timer expires. Otherwise this is a
       * saved-credential connect and the device is simply ONLINE. */
      if ( s_ctx.state == WIFI_PROVISIONING_CONTROLLER_PROVISIONING )
      {
        _start_grace();
      }
      else
      {
        s_ctx.state = WIFI_PROVISIONING_CONTROLLER_ONLINE;
      }
      break;

    case WIFI_MGMT_EVENT_DISCONNECTED:
      /* A single transient loss of a saved-credential connection must NEVER
       * reopen the portal. Fall back to the waiting state so a later
       * exhausted-credential failure can still trigger it. A disconnect
       * inside the grace window aborts that window and keeps the portal. */
      if ( s_ctx.state == WIFI_PROVISIONING_CONTROLLER_GRACE )
      {
        _cancel_grace_timer();
        s_ctx.state = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
      }
      else if ( s_ctx.state == WIFI_PROVISIONING_CONTROLLER_ONLINE )
      {
        s_ctx.state = WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT;
      }
      break;

    case WIFI_MGMT_EVENT_CONNECT_FAILED:
      /* A saved-credential attempt failed and no saved credentials remain:
       * open the portal exactly once. A failure inside the grace window only
       * cancels the timer and keeps the existing portal for another retry. */
      if ( s_ctx.state == WIFI_PROVISIONING_CONTROLLER_GRACE )
      {
        _cancel_grace_timer();
        s_ctx.state = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
      }
      else if ( s_ctx.state == WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT &&
               !s_ctx.fallback_started )
      {
        s_ctx.fallback_started = true;
        (void)   wifi_http_provisioning_start();
        s_ctx.state = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
      }
      break;

    default:
      /* SCAN_COMPLETED and MODE_CHANGED are not fallback triggers. */
      break;
  }
}

/* Public API ------------------------------------------------------------ */

bool wifi_provisioning_controller_init( void )
{
  /* Idempotent: repeated init does not re-subscribe and does not start the
   * provisioning application a second time. */
  if ( s_ctx.enabled )
  {
    return true;
  }

  s_ctx.enabled            = true;
  s_ctx.fallback_started   = false;
  s_ctx.grace_timer_active = false;
  s_ctx.grace_ms           = CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS;
  s_ctx.state              = WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT;

  /* The grace timer is created once and reused across sessions. A failure to
   * create it leaves the portal up after success (grace never expires). */
  s_ctx.grace_timer_ready = false;
  if ( osal_timer_create( &s_ctx.grace_timer, "wifi_prov_grace", 1u, false,
                        _on_grace_timer_expired, NULL, NULL, 0u ) == OSAL_SUCCESS )
  {
    s_ctx.grace_timer_ready = true;
  }

  (void)    wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECTED,      _controller_on_event, NULL );
  (void)    wifi_mgmt_subscribe( WIFI_MGMT_EVENT_DISCONNECTED,   _controller_on_event, NULL );
  (void)    wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECT_FAILED, _controller_on_event, NULL );

  /* No saved credential exists: the device cannot join a network on its own,
   * so open the provisioning application at once. */
  if ( !wifi_mgmt_is_read_data() )
  {
    s_ctx.fallback_started = true;
    (void) wifi_http_provisioning_start();
    s_ctx.state = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
  }

  return true;
}

void wifi_provisioning_controller_deinit( void )
{
  /* Idempotent: deinit while already inactive is a safe no-op. */
  if ( !s_ctx.enabled )
  {
    return;
  }

  (void)    wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_CONNECTED,      _controller_on_event, NULL );
  (void)    wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_DISCONNECTED,   _controller_on_event, NULL );
  (void)    wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_CONNECT_FAILED, _controller_on_event, NULL );

  _cancel_grace_timer();
  if ( s_ctx.grace_timer_ready )
  {
    (void) osal_timer_delete( s_ctx.grace_timer, 0u );
    s_ctx.grace_timer_ready = false;
  }

  s_ctx.enabled          = false;
  s_ctx.fallback_started = false;
  s_ctx.state             = WIFI_PROVISIONING_CONTROLLER_DISABLED;
}

bool wifi_provisioning_controller_stop( void )
{
  if ( !s_ctx.enabled ) return false;

  _cancel_grace_timer();
  if ( wifi_http_provisioning_stop() )
  {
    (void) wifi_mgmt_request_mode( T_WIFI_TYPE_CLIENT );
    s_ctx.state = WIFI_PROVISIONING_CONTROLLER_DISABLED;
    return true;
  }

  if ( s_ctx.state == WIFI_PROVISIONING_CONTROLLER_GRACE )
  {
    s_ctx.state = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
  }
  return false;
}

void wifi_provisioning_controller_set_success_grace_ms( uint32_t grace_ms )
{
  s_ctx.grace_ms = grace_ms;
}

bool wifi_provisioning_controller_is_provisioning( void )
{
  return s_ctx.state == WIFI_PROVISIONING_CONTROLLER_PROVISIONING ||
         s_ctx.state == WIFI_PROVISIONING_CONTROLLER_GRACE;
}

wifi_provisioning_controller_state_t wifi_provisioning_controller_get_state( void )
{
  return s_ctx.state;
}

#endif    /* CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK */