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
 *     portal open for another later attempt;
 *   - the grace timer expires or an explicit stop is issued -> stop the
 *     listeners, request a STA-only transition, enter RETIRING_AP and only
 *     report ONLINE/DISABLED after WIFI_MGMT_EVENT_MODE_CHANGED confirms the
 *     requested STA-only mode. A listener-stop or mode-transition failure drops
 *     into a recoverable state without falsely reporting ONLINE.
 *
 * Thread-safety. All controller state (state, enabled, fallback flag, grace
 * configuration, timer bookkeeping and session generation) is guarded by one
 * OSAL mutex shared by the API, the Wi-Fi event callback and the timer
 * callback paths. The mutex is never held while calling out to the
 * provisioning, Wi-Fi or OSAL timer APIs, because those may block or re-enter
 * a controller callback; the helper functions validate the session under the
 * lock, release it, issue the call, then re-validate before committing the
 * result. A session/generation token lets a stale grace expiry from a
 * cancelled or prior window be ignored.
 */

#include "hq_config.h"
#include "osal_mutex.h"
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
  bool                                 fallback_started; /* Start-once guard. */
  uint32_t                             grace_ms;         /* Overridable grace period. */
  osal_timer_id_t                      grace_timer;      /* Success grace timer. */
  bool                                 grace_timer_ready;/* Timer created at init. */
  bool                                 grace_timer_active;/* Timer currently armed. */
  uint32_t                             session;            /* Lifecycle generation. */
  uint32_t                             grace_armed_session;/* Session that armed the timer. */
  bool                                 retire_online;      /* Retire target: ONLINE (grace) vs DISABLED (stop). */
} wifi_controller_ctx_t;

static wifi_controller_ctx_t s_ctx = {
  WIFI_PROVISIONING_CONTROLLER_DISABLED,             /* state */
  false,                                             /* enabled */
  false,                                             /* fallback_started */
  CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS,    /* grace_ms */
  NULL,                                              /* grace_timer */
  false,                                             /* grace_timer_ready */
  false,                                             /* grace_timer_active */
  0u,                                                /* session */
  0u,                                                /* grace_armed_session */
  false                                              /* retire_online */
};

/* Latched when an explicit success-grace override is installed.  It lets an
 * application configure the grace period before wifi_provisioning_controller_
 * init() runs; without it init() would overwrite the override with the build
 * default.  The value is intentionally not cleared by init()/deinit(). */
static bool s_grace_override = false;

/* Shared controller lock: one mutex guards every controller field accessed
 * from the API, the Wi-Fi callback and the timer callback paths. The lock is
 * created on the first init and kept for the process (never deleted), so no
 * concurrent public call can ever hold or touch a released mutex. */
static osal_mutex_id_t  s_lock       = NULL;
static osal_mutex_id_t  s_lifecycle_lock = NULL;
static volatile uint8_t s_lock_init  = 0u;

static bool _ensure_lock( void )
{
  osal_mutex_id_t lock = NULL;

  if ( s_lock_init == 2u ) return true;
  if ( __sync_bool_compare_and_swap( &s_lock_init, 0u, 1u ) )
  {
    if ( osal_mutex_create( &lock, "wifi_prov_ctrl" ) != OSAL_SUCCESS )
    {
      s_lock_init = 0u;
      return false;
    }
    s_lock = lock;
    if ( osal_mutex_create( &s_lifecycle_lock, "wifi_prov_ctrl_lc" ) != OSAL_SUCCESS )
    {
      (void) osal_mutex_delete( s_lock );
      s_lock = NULL;
      s_lock_init = 0u;
      return false;
    }
    __sync_synchronize();
    s_lock_init  = 2u;
    return true;
  }

  while ( s_lock_init == 1u ) __sync_synchronize();
  return s_lock_init == 2u;
}

static void _lock( void )
{
  (void) osal_mutex_take( s_lock );
}

static void _unlock( void )
{
  (void) osal_mutex_give( s_lock );
}

/* Private helpers ------------------------------------------------------- */

/* Forward declaration: defined below, used by _start_grace and the expiry
 * callback to retire the access point once listeners are stopped. */
static bool _retire_provisioning( bool as_online );

/* Cancel an armed grace timer and move the controller to a recoverable state
 * (used when a fresh connection fails or a disconnect ends a grace window).
 * The OSAL timer stop runs outside the lock. */
static void _abort_grace_window( void )
{
  bool do_stop = false;

  _lock();
  if ( s_ctx.state == WIFI_PROVISIONING_CONTROLLER_GRACE )
  {
    do_stop = s_ctx.grace_timer_active && s_ctx.grace_timer_ready;
    s_ctx.grace_timer_active      = false;
    s_ctx.state                   = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
  }
  _unlock();

  if ( do_stop ) (void) osal_timer_stop( s_ctx.grace_timer, 0u );
}

/* Start the success grace window. A zero period shuts the portal down
 * immediately (grace never actually opens). When a usable timer exists it is
 * re-armed for the configured duration under the current session/generation
 * and the controller enters GRACE. The controller lock is taken internally. */
static void _start_grace( void )
{
  bool arm = false;
  uint32_t session = 0u;
  uint32_t grace_ms = 0u;
  osal_timer_id_t timer = NULL;

  _lock();
  if ( !s_ctx.enabled || s_ctx.state != WIFI_PROVISIONING_CONTROLLER_PROVISIONING )
  {
    _unlock();
    return;
  }

  if ( s_ctx.grace_ms == 0u )
  {
    /* Zero grace: retire immediately. Keep the state in GRACE so the retire
     * helper recognises a live window and refuses a spurious later restart. */
    s_ctx.state = WIFI_PROVISIONING_CONTROLLER_GRACE;
    _unlock();
    (void) _retire_provisioning( true );
    return;
  }

  if ( s_ctx.grace_timer_ready && !s_ctx.grace_timer_active )
  {
    s_ctx.state                   = WIFI_PROVISIONING_CONTROLLER_GRACE;
    s_ctx.grace_armed_session     = s_ctx.session;
    s_ctx.grace_timer_active      = true;
    session                       = s_ctx.session;
    grace_ms                      = s_ctx.grace_ms;
    timer                         = s_ctx.grace_timer;
    arm                           = true;
  }
  _unlock();

  if ( arm )
  {
    const bool timer_started =
      osal_timer_change_period( timer, grace_ms, 0u ) == OSAL_SUCCESS &&
      osal_timer_start( timer, 0u ) == OSAL_SUCCESS;

    _lock();
    const bool current = s_ctx.enabled &&
                         s_ctx.session == session &&
                         s_ctx.state == WIFI_PROVISIONING_CONTROLLER_GRACE &&
                         s_ctx.grace_armed_session == session &&
                         s_ctx.grace_timer_active;
    if ( !timer_started && current ) s_ctx.grace_timer_active = false;
    _unlock();

    if ( timer_started && current ) return;
    if ( timer_started ) (void) osal_timer_stop( timer, 0u );
    if ( current ) (void) _retire_provisioning( true );
    return;
  }

  /* No usable timer: fall through to an immediate (no-wait) retirement. */
  _lock();
  s_ctx.state = WIFI_PROVISIONING_CONTROLLER_GRACE;
  _unlock();
  (void) _retire_provisioning( true );
}

/* Retire the temporary access point. If @p as_online is true the target state
 * after the STA-only confirmation is ONLINE (grace expiry); otherwise it is
 * DISABLED (explicit stop). The graceful retirement is asynchronous: listeners
 * are stopped and the STA-only transition is requested, and only a later
 * WIFI_MGMT_EVENT_MODE_CHANGED moves the controller out of RETIRING_AP. A
 * listener-stop or mode-request failure, also when the transition is not
 * accepted, keeps the controller recoverable and never reports ONLINE.
 * Returns true when the provisioning listeners are (or already were) stopped
 * and the retire was requested; false when a step failed. */
static bool _retire_provisioning( bool as_online )
{
  bool       do_timer_stop;
  uint32_t   sess;
  bool       listeners_stopped;
  bool       current;

  _lock();
  if ( !s_ctx.enabled )
  {
    _unlock();
    return false;
  }

  switch ( s_ctx.state )
  {
    case WIFI_PROVISIONING_CONTROLLER_GRACE:
    case WIFI_PROVISIONING_CONTROLLER_PROVISIONING:
      break;
    default:
      /* No active portal. Only an explicit stop turns this into DISABLED; a
       * tokenised grace-expiry retire from here is not meaningful. */
      if ( !as_online )
      {
        s_ctx.state         = WIFI_PROVISIONING_CONTROLLER_DISABLED;
        s_ctx.retire_online = false;
      }
      _unlock();
      return true;
  }

  sess                         = s_ctx.session;
  do_timer_stop                = s_ctx.grace_timer_active && s_ctx.grace_timer_ready;
  s_ctx.grace_timer_active     = false;
  s_ctx.state                  = WIFI_PROVISIONING_CONTROLLER_RETIRING_AP;
  s_ctx.retire_online          = as_online;
  _unlock();

  if ( do_timer_stop ) (void) osal_timer_stop( s_ctx.grace_timer, 0u );

  /* Stop the listeners outside the lock: wifi_http_provisioning_stop() may
   * join an in-flight lifecycle and re-enter our callbacks. */
  listeners_stopped = wifi_http_provisioning_stop();

  _lock();
  current = s_ctx.enabled &&
            s_ctx.session == sess &&
            s_ctx.state   == WIFI_PROVISIONING_CONTROLLER_RETIRING_AP;
  _unlock();

  if ( !current )
  {
    /* A deinit or a fresh session superseded the retirement. */
    return listeners_stopped;
  }

  if ( !listeners_stopped )
  {
    /* Listener shutdown failed: keep the portal available and recoverable. */
    _lock();
    s_ctx.state         = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
    s_ctx.retire_online = false;
    _unlock();
    return false;
  }

  /* Listeners are confirmed stopped. Ask the Wi-Fi manager for STA-only. */
  if ( !wifi_mgmt_request_mode( T_WIFI_TYPE_CLIENT ) )
  {
    /* The mode transition was not accepted. Reopen the portal so the device
     * remains provisionable, keep the session recoverable and never claim
     * ONLINE without the confirmed STA-only state. */
    _lock();
    s_ctx.state         = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
    s_ctx.retire_online = false;
    _unlock();
    (void) wifi_http_provisioning_start();
    return false;
  }

  /* The request was accepted. RETIRING_AP remains until MODE_CHANGED. */
  return true;
}

/* Grace timer expiry callback, delivered on the timer task. Only a portal
 * that is still inside the current session's grace window is shut down. A
 * stale expiry (a cancelled/prior window, a disconnect, an explicit stop or a
 * deinit oversome) is a no-op thanks to the session/generation token. */
static void _on_grace_timer_expired( osal_timer_id_t timer_id )
{
  (void) timer_id;

  _lock();
  const bool valid =
    s_ctx.enabled &&
    s_ctx.state == WIFI_PROVISIONING_CONTROLLER_GRACE &&
    s_ctx.grace_armed_session == s_ctx.session;
  _unlock();

  if ( !valid ) return;
  (void) _retire_provisioning( true );
}

/* Event callback delivered on the Wi-Fi management event thread. */
static void _controller_on_event( wifi_mgmt_event_t event, void* user_data )
{
  (void) user_data;

  switch ( event )
  {
    case WIFI_MGMT_EVENT_CONNECTED:
      /* If the station obtained an IP while the portal is active, the
       * submitted credential succeeded: enter the grace window. Otherwise
       * this is a saved-credential connect and the device is simply ONLINE --
       * except while a STA-only retirement is in progress (that must settle
       * first). */
      _lock();
      if ( !s_ctx.enabled ) { _unlock(); return; }
      if ( s_ctx.state == WIFI_PROVISIONING_CONTROLLER_PROVISIONING )
      {
        _unlock();
        _start_grace();
      }
      else
      {
        if ( s_ctx.state != WIFI_PROVISIONING_CONTROLLER_RETIRING_AP &&
             s_ctx.state != WIFI_PROVISIONING_CONTROLLER_GRACE )
        {
          s_ctx.state = WIFI_PROVISIONING_CONTROLLER_ONLINE;
        }
        _unlock();
      }
      break;

    case WIFI_MGMT_EVENT_DISCONNECTED:
      /* A single transient loss of a saved-credential connection must NEVER
       * reopen the portal. Fall back to the waiting state so a later
       * exhausted-credential failure can still open it. A disconnect inside
       * the grace window aborts it and keeps the portal. The prior state is
       * read under the lock; the grace timer stop itself runs outside it. */
      {
        wifi_provisioning_controller_state_t state_before;
        _lock();
        state_before = s_ctx.state;
        _unlock();

        if ( state_before == WIFI_PROVISIONING_CONTROLLER_GRACE )
        {
          _abort_grace_window();
        }
        else if ( state_before == WIFI_PROVISIONING_CONTROLLER_ONLINE )
        {
          _lock();
          s_ctx.state = WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT;
          _unlock();
        }
      }
      break;

    case WIFI_MGMT_EVENT_CONNECT_FAILED:
      /* A saved-credential attempt failed and no saved credentials remain:
       * open the portal exactly once. A failed attempt inside the grace
       * window only cancels the timer and keeps the existing portal. */
      {
        wifi_provisioning_controller_state_t state_before;
        _lock();
        state_before = s_ctx.state;
        _unlock();

        if ( state_before == WIFI_PROVISIONING_CONTROLLER_GRACE )
        {
          _abort_grace_window();
        }
        else if ( state_before == WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT )
        {
          _lock();
          if ( !s_ctx.fallback_started )
          {
            s_ctx.fallback_started = true;
            _unlock();
            (void) wifi_http_provisioning_start();
            _lock();
            if ( s_ctx.enabled )
            {
              s_ctx.state = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
            }
          }
          _unlock();
        }
      }
      break;

    case WIFI_MGMT_EVENT_MODE_CHANGED:
      /* The asynchronous completion of a STA-only retirement: only now may
       * the controller report ONLINE (or, for an explicit stop, DISABLED).
       * A stale confirmation after a newer session is ignored. */
      _lock();
      if ( s_ctx.enabled && s_ctx.state == WIFI_PROVISIONING_CONTROLLER_RETIRING_AP )
      {
        if ( s_ctx.retire_online )
        {
          s_ctx.state = WIFI_PROVISIONING_CONTROLLER_ONLINE;
        }
        else
        {
          s_ctx.state = WIFI_PROVISIONING_CONTROLLER_DISABLED;
        }
        s_ctx.retire_online = false;
      }
      _unlock();
      break;

    default:
      /* SCAN_COMPLETED is not a fallback trigger. */
      break;
  }
}

/* Public API ------------------------------------------------------------ */

bool wifi_provisioning_controller_init( void )
{
  bool timer_ready = false;

  if ( !_ensure_lock() ) return false;

  (void) osal_mutex_take( s_lifecycle_lock );
  _lock();
  if ( s_ctx.enabled )
  {
    _unlock();
    (void) osal_mutex_give( s_lifecycle_lock );
    return true;
  }
  _unlock();

  _lock();
  s_ctx.enabled            = true;
  s_ctx.fallback_started   = false;
  s_ctx.grace_timer_active = false;
  s_ctx.retire_online      = false;
  ++s_ctx.session;                       /* New lifecycle generation. */
  /* The configured default applies only until the first explicit override. */
  if ( !s_grace_override )
  {
    s_ctx.grace_ms = CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS;
  }
  s_ctx.state = WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT;
  _unlock();

  /* The grace timer is created once and reused across sessions. A failure to
   * create it leaves the portal up after success (grace never expires). */
  timer_ready =
    osal_timer_create( &s_ctx.grace_timer, "wifi_prov_grace", 1u, false,
                       _on_grace_timer_expired, NULL, NULL, 0u ) == OSAL_SUCCESS;

  _lock();
  if ( s_ctx.enabled )
  {
    s_ctx.grace_timer_ready = timer_ready;
  }
  _unlock();

  (void) wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECTED,      _controller_on_event, NULL );
  (void) wifi_mgmt_subscribe( WIFI_MGMT_EVENT_DISCONNECTED,   _controller_on_event, NULL );
  (void) wifi_mgmt_subscribe( WIFI_MGMT_EVENT_CONNECT_FAILED, _controller_on_event, NULL );
  (void) wifi_mgmt_subscribe( WIFI_MGMT_EVENT_MODE_CHANGED,   _controller_on_event, NULL );

  /* No saved credential exists: the device cannot join a network on its own,
   * so open the provisioning application at once. */
  if ( !wifi_mgmt_is_read_data() )
  {
    _lock();
    s_ctx.fallback_started = true;
    _unlock();
    (void) wifi_http_provisioning_start();
    _lock();
    if ( s_ctx.enabled && s_ctx.fallback_started )
    {
      s_ctx.state = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
    }
    _unlock();
  }

  (void) osal_mutex_give( s_lifecycle_lock );
  return true;
}

void wifi_provisioning_controller_deinit( void )
{
  if ( !_ensure_lock() ) return;

  (void) osal_mutex_take( s_lifecycle_lock );
  _lock();
  if ( !s_ctx.enabled )
  {
    _unlock();
    (void) osal_mutex_give( s_lifecycle_lock );
    return;
  }
  _unlock();

  /* Unsubscribe first so no new event callback can re-enter a torn-down
   * controller. */
  (void) wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_CONNECTED,      _controller_on_event, NULL );
  (void) wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_DISCONNECTED,   _controller_on_event, NULL );
  (void) wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_CONNECT_FAILED, _controller_on_event, NULL );
  (void) wifi_mgmt_unsubscribe( WIFI_MGMT_EVENT_MODE_CHANGED,   _controller_on_event, NULL );

  /* Cancel and delete the grace timer while not holding the controller lock.
   * osal_timer_delete joins the timer thread on POSIX, so any in-flight expiry
   * callback completes before controller state is touched below. */
  _lock();
  const bool do_stop   = s_ctx.grace_timer_active && s_ctx.grace_timer_ready;
  const bool do_delete = s_ctx.grace_timer_ready;
  s_ctx.grace_timer_active = false;
  s_ctx.grace_timer_ready  = false;
  _unlock();
  if ( do_stop )   (void) osal_timer_stop( s_ctx.grace_timer, 0u );
  if ( do_delete ) (void) osal_timer_delete( s_ctx.grace_timer, 0u );

  /* Clear controller state only after the timer has been dismissed. The
   * session generation bump invalidates any late callback. */
  _lock();
  s_ctx.enabled          = false;
  s_ctx.fallback_started = false;
  s_ctx.retire_online    = false;
  s_ctx.state             = WIFI_PROVISIONING_CONTROLLER_DISABLED;
  ++s_ctx.session;
  _unlock();
  (void) osal_mutex_give( s_lifecycle_lock );
}

bool wifi_provisioning_controller_stop( void )
{
  if ( !_ensure_lock() ) return false;

  (void) osal_mutex_take( s_lifecycle_lock );
  _lock();
  const bool active = s_ctx.enabled;
  _unlock();
  if ( !active )
  {
    (void) osal_mutex_give( s_lifecycle_lock );
    return false;
  }

  const bool stopped = _retire_provisioning( false );
  (void) osal_mutex_give( s_lifecycle_lock );
  return stopped;
}

void wifi_provisioning_controller_set_success_grace_ms( uint32_t grace_ms )
{
  if ( !_ensure_lock() ) return;

  _lock();
  s_ctx.grace_ms   = grace_ms;
  s_grace_override = true;
  _unlock();
}

bool wifi_provisioning_controller_is_provisioning( void )
{
  bool result;
  if ( !_ensure_lock() ) return false;

  _lock();
  result = s_ctx.state == WIFI_PROVISIONING_CONTROLLER_PROVISIONING ||
           s_ctx.state == WIFI_PROVISIONING_CONTROLLER_GRACE;
  _unlock();
  return result;
}

wifi_provisioning_controller_state_t wifi_provisioning_controller_get_state( void )
{
  wifi_provisioning_controller_state_t result;
  if ( !_ensure_lock() ) return WIFI_PROVISIONING_CONTROLLER_DISABLED;

  _lock();
  result = s_ctx.state;
  _unlock();
  return result;
}

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
void wifi_provisioning_controller_test_fire_grace_expiry( void )
{
  if ( !_ensure_lock() ) return;
  _on_grace_timer_expired( s_ctx.grace_timer );
}
#endif

#endif    /* CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK */