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
 * Deferred policy work. The Wi-Fi management event callback and the grace
 * timer callback never run provisioning or mode-transition calls themselves:
 * they only post a small, self-contained work item (the raw trigger plus the
 * session/generation token in effect when it arrived) to a queue owned by the
 * controller. A dedicated controller-owned worker task applies the items in
 * FIFO order, so the Wi-Fi worker thread never blocks on a listener start/
 * stop or a mode request and event ordering is preserved even when a burst of
 * events arrives while an earlier fallback start is still pending (a CONNECTED
 * queued behind a CONNECT_FAILED is applied after the portal was opened and
 * still produces GRACE instead of a lost race).
 *
 * Thread-safety. All controller state (state, enabled, fallback flag, grace
 * configuration, timer bookkeeping and session generation) is guarded by one
 * OSAL mutex shared by the API, the deferred worker and the timer callback
 * paths. The mutex is never held while calling out to the provisioning,
 * Wi-Fi or OSAL timer APIs, because those may block or re-enter a controller
 * callback; the helper functions validate the session under the lock, release
 * it, issue the call, then re-validate before committing the result. A
 * session/generation token lets a stale grace expiry and any deferred item
 * queued by a cancelled or prior window be ignored: an item that belongs to a
 * superseded session is dropped, never applied.
 */

#include "hq_config.h"
#include "osal_bin_sem.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_queue.h"
#include "osal_task.h"
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

/* Default fallback budget (consecutive CONNECT_FAILED events before the portal
 * opens) when the Kconfig option is not wired into the build configuration.
 * 1 preserves the historical first-failure behavior. */
#ifndef CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS
#define CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS 1u
#endif

/* Private state --------------------------------------------------------- */

/* Deferred policy actions. The Wi-Fi event callback and the grace timer
 * callback only post one of these; the controller-owned worker applies them
 * in FIFO order on its own thread. */
typedef enum
{
  DEFER_ACTION_QUIT = 0,          /* Worker shutdown marker (posted by deinit). */
  DEFER_ACTION_MGMT_EVENT,        /* A wifi_mgmt event to apply. */
  DEFER_ACTION_GRACE_EXPIRED      /* The success grace timer expired. */
} defer_action_t;

typedef struct
{
  defer_action_t    action;       /* What the worker must do. */
  wifi_mgmt_event_t event;        /* Raw event (valid for DEFER_ACTION_MGMT_EVENT). */
  uint32_t          session;      /* Lifecycle generation captured when posted. */
} controller_deferred_t;

#define CTRL_DEFER_QUEUE_DEPTH   16u
#define CTRL_DEFER_STACK_SIZE    (OSAL_TASK_MIN_STACK_SIZE * 4u)
#define CTRL_DEFER_PRIORITY      1u
#define CTRL_DEFER_STOP_WAIT_MS  2000u

typedef struct
{
  wifi_provisioning_controller_state_t state;            /* Current policy state. */
  bool                                 enabled;          /* Init gate (idempotent). */
  bool                                 fallback_started; /* Start-once guard. */
  uint32_t                             grace_ms;         /* Overridable grace period. */
  uint32_t                             fallback_budget;  /* Consecutive CONNECT_FAILED budget (0 = disabled). */
  uint32_t                             fallback_failures;/* Consecutive failures counted in AWAITING_CONNECT. */
  osal_timer_id_t                      grace_timer;      /* Success grace timer. */
  bool                                 grace_timer_ready;/* Timer created at init. */
  bool                                 grace_timer_active;/* Timer currently armed. */
  uint32_t                             session;            /* Lifecycle generation. */
  uint32_t                             grace_armed_session;/* Session that armed the timer. */
  bool                                 retire_online;      /* Retire target: ONLINE (grace) vs DISABLED (stop). */
  wifi_provisioning_controller_state_cb_t notify_cb;      /* Optional state-change hook (init-time). */
  void                                 *notify_user_ctx;  /* Hook user context. */
  /* Deferred policy pipeline (owned by the controller). */
  osal_queue_id_t defer_queue;   /* FIFO queue of deferred actions. */
  osal_task_id_t  defer_task;    /* Deferred worker task. */
  bool            defer_ready;   /* Queue + worker created at init. */
#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
  bool    defer_hold;            /* Test gate: park the worker before applying. */
  wifi_provisioning_controller_test_apply_cb_t apply_hook;   /* Test apply observer. */
  void   *apply_hook_ctx;        /* Observer user context. */
#endif
} wifi_controller_ctx_t;

static wifi_controller_ctx_t s_ctx = {
  WIFI_PROVISIONING_CONTROLLER_DISABLED,             /* state */
  false,                                             /* enabled */
  false,                                             /* fallback_started */
  CONFIG_WIFI_HTTP_PROVISIONING_SUCCESS_GRACE_MS,    /* grace_ms */
  CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS,   /* fallback_budget */
  0u,                                                /* fallback_failures */
  NULL,                                              /* grace_timer */
  false,                                             /* grace_timer_ready */
  false,                                             /* grace_timer_active */
  0u,                                                /* session */
  0u,                                                /* grace_armed_session */
  false,                                             /* retire_online */
  NULL,                                              /* notify_cb */
  NULL,                                              /* notify_user_ctx */
  NULL,                                              /* defer_queue */
  0,                                                 /* defer_task */
  false                                              /* defer_ready */
#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
  ,
  false,                                             /* defer_hold */
  NULL,                                              /* apply_hook */
  NULL                                               /* apply_hook_ctx */
#endif
};

/* Latched when an explicit success-grace override is installed.  It lets an
 * application configure the grace period before wifi_provisioning_controller_
 * init() runs; without it init() would overwrite the override with the build
 * default.  The value is intentionally not cleared by init()/deinit(). */
static bool s_grace_override = false;

/* Shared controller lock: one mutex guards every controller field accessed
 * from the API, the deferred worker and the timer callback paths. The lock is
 * created on the first init and kept for the process (never deleted), so no
 * concurrent public call can ever hold or touch a released mutex. */
static osal_mutex_id_t  s_lock         = NULL;
static osal_mutex_id_t  s_lifecycle_lock = NULL;
static volatile uint8_t s_lock_init    = 0u;

/* Worker-exit signal: the deferred worker gives it just before returning, so
 * deinit can wait for the worker to drop stale items and observe the QUIT
 * marker before joining the task. Process-lifetime like the locks above. */
static osal_bin_sem_id_t s_defer_stopped_sem = NULL;

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
/* Test gate release channel: the deferred worker parks on this semaphore
 * while wifi_provisioning_controller_test_set_defer_hold(true) is in effect. */
static osal_bin_sem_id_t s_defer_hold_sem = NULL;
#endif
/* Deferred items posted but not yet fully applied. Producers increment it
 * before the item becomes visible in the queue (and roll back when the send
 * fails); the worker decrements it after the apply returns. The test
 * wait_idle helper reads a zero count as quiescence, so an item can never be
 * invisible to both the queue and this counter and no false idle is possible
 * while an item is dequeued-but-not-yet-applied. */
static volatile uint32_t s_defer_unprocessed = 0u;

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
    if ( osal_bin_sem_create( &s_defer_stopped_sem, "wifi_prov_ctrl_stop",
                              OSAL_SEM_EMPTY ) != OSAL_SUCCESS )
    {
      (void) osal_mutex_delete( s_lifecycle_lock );
      s_lifecycle_lock = NULL;
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

/* State-change notification plumbing -------------------------------------- */

/* Snapshot of the optional notification hook, captured in the same lock scope
 * as the state commit so the delivered session token always matches the
 * lifecycle generation in which the transition actually occurred. */
typedef struct
{
  wifi_provisioning_controller_state_cb_t cb;
  void                                   *user_ctx;
  uint32_t                                session;
} notify_snapshot_t;

/* Copy the currently registered hook and session token. Must be called while
 * holding the controller lock, in the same scope that commits the transition
 * it will describe. */
static void _capture_notify( notify_snapshot_t *snap )
{
  snap->cb       = s_ctx.notify_cb;
  snap->user_ctx = s_ctx.notify_user_ctx;
  snap->session  = s_ctx.session;
}

/* Deliver a captured notification outside the controller lock. A NULL hook is
 * a safe no-op (notifications are opt-in) and a transition where the state did
 * not actually change is suppressed. The callback is invoked with the lock
 * released so a product may safely query get_state() from inside it; the
 * callback contract (see the header) forbids every other controller call. */
static void _fire_notify( const notify_snapshot_t *snap,
                          wifi_provisioning_controller_state_t previous,
                          wifi_provisioning_controller_state_t current )
{
  if ( snap->cb != NULL && previous != current )
  {
    snap->cb( previous, current, snap->session, snap->user_ctx );
  }
}

/* Private helpers ------------------------------------------------------- */

/* Forward declaration: defined below, used by _start_grace, the grace-expiry
 * apply and the explicit stop path to retire the access point once listeners
 * are stopped. @p session must be the lifecycle generation the caller acted
 * for; the helper re-validates it under the lock. */
static bool _retire_provisioning( bool as_online, uint32_t session );

/* Cancel an armed grace timer and move the controller to a recoverable state
 * (used when a fresh connection fails or a disconnect ends a grace window).
 * Only a window that is still inside the current @p session is aborted. The
 * OSAL timer stop runs outside the lock. */
static void _abort_grace_window( uint32_t session )
{
  bool do_stop = false;
  notify_snapshot_t snap = { NULL, NULL, 0u };

  _lock();
  if ( s_ctx.enabled && s_ctx.session == session &&
       s_ctx.state == WIFI_PROVISIONING_CONTROLLER_GRACE )
  {
    do_stop = s_ctx.grace_timer_active && s_ctx.grace_timer_ready;
    s_ctx.grace_timer_active      = false;
    s_ctx.state                   = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
    _capture_notify( &snap );
  }
  _unlock();

  if ( do_stop ) (void) osal_timer_stop( s_ctx.grace_timer, 0u );
  _fire_notify( &snap, WIFI_PROVISIONING_CONTROLLER_GRACE,
                WIFI_PROVISIONING_CONTROLLER_PROVISIONING );
}

/* Start the success grace window. A zero period shuts the portal down
 * immediately (grace never actually opens). When a usable timer exists it is
 * re-armed for the configured duration under the current session/generation
 * and the controller enters GRACE. @p session is the lifecycle generation in
 * which the triggering event was queued; the window is only opened if it is
 * still the current one. The controller lock is taken internally. */
static void _start_grace( uint32_t session )
{
  bool arm = false;
  uint32_t grace_ms = 0u;
  osal_timer_id_t timer = NULL;
  notify_snapshot_t snap = { NULL, NULL, 0u };

  _lock();
  if ( !s_ctx.enabled || s_ctx.session != session ||
       s_ctx.state != WIFI_PROVISIONING_CONTROLLER_PROVISIONING )
  {
    _unlock();
    return;
  }

  if ( s_ctx.grace_ms == 0u )
  {
    /* Zero grace: retire immediately. Keep the state in GRACE so the retire
     * helper recognises a live window and refuses a spurious later restart. */
    s_ctx.state = WIFI_PROVISIONING_CONTROLLER_GRACE;
    _capture_notify( &snap );
    _unlock();
    _fire_notify( &snap, WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                  WIFI_PROVISIONING_CONTROLLER_GRACE );
    (void) _retire_provisioning( true, session );
    return;
  }

  if ( s_ctx.grace_timer_ready && !s_ctx.grace_timer_active )
  {
    s_ctx.state                   = WIFI_PROVISIONING_CONTROLLER_GRACE;
    s_ctx.grace_armed_session     = s_ctx.session;
    s_ctx.grace_timer_active      = true;
    grace_ms                      = s_ctx.grace_ms;
    timer                         = s_ctx.grace_timer;
    arm                           = true;
    _capture_notify( &snap );
  }
  _unlock();
  _fire_notify( &snap, WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                WIFI_PROVISIONING_CONTROLLER_GRACE );

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
    if ( current ) (void) _retire_provisioning( true, session );
    return;
  }

  /* No usable timer: fall through to an immediate (no-wait) retirement. */
  _lock();
  s_ctx.state = WIFI_PROVISIONING_CONTROLLER_GRACE;
  _capture_notify( &snap );
  _unlock();
  _fire_notify( &snap, WIFI_PROVISIONING_CONTROLLER_PROVISIONING,
                WIFI_PROVISIONING_CONTROLLER_GRACE );
  (void) _retire_provisioning( true, session );
}

/* Retire the temporary access point. If @p as_online is true the target state
 * after the STA-only confirmation is ONLINE (grace expiry); otherwise it is
 * DISABLED (explicit stop). @p session is the lifecycle generation the caller
 * acted for and is re-validated under the lock before any side effect. The
 * graceful retirement is asynchronous: listeners are stopped and the STA-only
 * transition is requested, and only a later WIFI_MGMT_EVENT_MODE_CHANGED moves
 * the controller out of RETIRING_AP. A listener-stop or mode-request failure,
 * also when the transition is not accepted, keeps the controller recoverable
 * and never reports ONLINE. Returns true when the provisioning listeners are
 * (or already were) stopped and the retire was requested; false when a step
 * failed. */
static bool _retire_provisioning( bool as_online, uint32_t session )
{
  bool       do_timer_stop;
  bool       listeners_stopped;
  bool       current;
  wifi_provisioning_controller_state_t prev;
  notify_snapshot_t snap = { NULL, NULL, 0u };

  _lock();
  if ( !s_ctx.enabled || s_ctx.session != session )
  {
    _unlock();
    return false;
  }
  prev = s_ctx.state;

  switch ( s_ctx.state )
  {
    case WIFI_PROVISIONING_CONTROLLER_GRACE:
    case WIFI_PROVISIONING_CONTROLLER_PROVISIONING:
      break;
    case WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT:
      if ( as_online ) break;
      /* Explicit stop while waiting has no active portal to retire. */
      s_ctx.state         = WIFI_PROVISIONING_CONTROLLER_DISABLED;
      s_ctx.retire_online = false;
      _capture_notify( &snap );
      _unlock();
      _fire_notify( &snap, prev, WIFI_PROVISIONING_CONTROLLER_DISABLED );
      return true;
    default:
      /* No active portal. Only an explicit stop turns this into DISABLED; a
       * tokenised grace-expiry retire from here is not meaningful. */
      if ( !as_online )
      {
        s_ctx.state         = WIFI_PROVISIONING_CONTROLLER_DISABLED;
        s_ctx.retire_online = false;
        _capture_notify( &snap );
      }
      _unlock();
      _fire_notify( &snap, prev, WIFI_PROVISIONING_CONTROLLER_DISABLED );
      return true;
  }

  do_timer_stop                = s_ctx.grace_timer_active && s_ctx.grace_timer_ready;
  s_ctx.grace_timer_active     = false;
  s_ctx.state                  = WIFI_PROVISIONING_CONTROLLER_RETIRING_AP;
  s_ctx.retire_online          = as_online;
  _capture_notify( &snap );
  _unlock();
  _fire_notify( &snap, prev, WIFI_PROVISIONING_CONTROLLER_RETIRING_AP );

  if ( do_timer_stop ) (void) osal_timer_stop( s_ctx.grace_timer, 0u );

  /* Stop the listeners outside the lock: wifi_http_provisioning_stop() may
   * join an in-flight lifecycle and re-enter our callbacks. */
  listeners_stopped = wifi_http_provisioning_stop();

  _lock();
  current = s_ctx.enabled &&
            s_ctx.session == session &&
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
    _capture_notify( &snap );
    _unlock();
    _fire_notify( &snap, WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
                  WIFI_PROVISIONING_CONTROLLER_PROVISIONING );
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
    _capture_notify( &snap );
    _unlock();
    _fire_notify( &snap, WIFI_PROVISIONING_CONTROLLER_RETIRING_AP,
                  WIFI_PROVISIONING_CONTROLLER_PROVISIONING );
    (void) wifi_http_provisioning_start();
    return false;
  }

  /* The request was accepted. RETIRING_AP remains until MODE_CHANGED. */
  return true;
}

/* Deferred pipeline ------------------------------------------------------ */

/* Capture, under the controller lock, whether the deferred pipeline is live
 * and which queue to post into. Returning true also snapshots the current
 * session into @p item, so the token always matches the queue that owns it. */
static bool _defer_capture( controller_deferred_t *item, defer_action_t action,
                            wifi_mgmt_event_t event, osal_queue_id_t *out_q )
{
  osal_queue_id_t q;

  _lock();
  q = NULL;
  if ( s_ctx.enabled && s_ctx.defer_ready && s_ctx.defer_queue != NULL )
  {
    item->action  = action;
    item->event   = event;
    item->session = s_ctx.session;
    q             = s_ctx.defer_queue;
  }
  _unlock();

  if ( q == NULL ) return false;
  *out_q = q;
  return true;
}

/* Post a Wi-Fi management event into the deferred pipeline exactly as the
 * real event callback does (session snapshot + FIFO post, never blocking).
 * Returns false when the controller is not live (event dropped). */
static bool _defer_post_event( wifi_mgmt_event_t event )
{
  controller_deferred_t item;
  osal_queue_id_t q;

  if ( !_defer_capture( &item, DEFER_ACTION_MGMT_EVENT, event, &q ) ) return false;
  /* Count the item before it becomes visible in the queue so the test
   * wait_idle helper can never observe a false idle between the send and the
   * worker's dequeue. A failed send rolls the count back. */
  (void) __sync_add_and_fetch( &s_defer_unprocessed, 1u );
  if ( osal_queue_send( q, &item, 0u ) == OSAL_SUCCESS ) return true;
  (void) __sync_sub_and_fetch( &s_defer_unprocessed, 1u );
  /* A dropped policy action must never be silent: without the event the
   * fallback state machine can stall (e.g. no grace after a dropped
   * CONNECTED). */
  osal_log_error( "wifi_prov_ctrl: deferred queue full, dropping event %d",
                  ( int ) event );
  return false;
}

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
/* Test gate: while defer_hold is set the worker parks in front of the queue
 * and applies nothing, so a platform test can prove that a Wi-Fi event
 * callback returns without performing provisioning work and can burst
 * synthetic events before releasing them in arrival order. */
static void _defer_test_hold( void )
{
  bool held;

  _lock();
  held = s_ctx.defer_hold;
  _unlock();

  while ( held )
  {
    (void) osal_bin_sem_timed_wait( s_defer_hold_sem, 10u );
    _lock();
    held = s_ctx.defer_hold;
    _unlock();
  }
}
#endif

/* Forward declarations: the apply entry points live below, next to the event
 * handler they replace. */
static void _controller_apply_event( const controller_deferred_t *item );
static void _controller_apply_grace_expiry( uint32_t session );

/* Apply one deferred item on the controller worker thread. An item carrying
 * the session/generation token of a superseded lifecycle is dropped here,
 * before any witness or side effect: init/deinit bumps the token so an item
 * queued by a cancelled or prior window is a no-op even when the worker had
 * already dequeued it. The per-action apply functions re-validate the token
 * under the lock again right before committing. */
static void _defer_apply_item( const controller_deferred_t *item )
{
  bool current;

  _lock();
  current = s_ctx.enabled && s_ctx.session == item->session;
  _unlock();
  if ( !current ) return;

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
  /* Test observability: report the about-to-be-applied item before any side
   * effect so a platform test can prove the ordered application of a burst. */
  if ( s_ctx.apply_hook != NULL )
  {
    wifi_provisioning_controller_test_deferred_t witness;

    witness.is_grace_expiry = ( item->action == DEFER_ACTION_GRACE_EXPIRED );
    witness.event           = item->event;
    witness.session         = item->session;
    s_ctx.apply_hook( &witness, s_ctx.apply_hook_ctx );
  }
#endif

  switch ( item->action )
  {
    case DEFER_ACTION_MGMT_EVENT:
      _controller_apply_event( item );
      break;
    case DEFER_ACTION_GRACE_EXPIRED:
      _controller_apply_grace_expiry( item->session );
      break;
    default:
      break;
  }
}

/* Deferred policy worker: applies queued actions in FIFO order on its own
 * thread. DEFER_ACTION_QUIT is the shutdown marker posted by deinit; after it
 * the worker drains (stale-session items are dropped without side effects),
 * signals its exit and returns so deinit can join the task. */
static void _defer_worker( void *arg )
{
  controller_deferred_t item;

  (void) arg;

  for ( ;; )
  {
    if ( osal_queue_receive( s_ctx.defer_queue, &item, OSAL_MAX_DELAY ) != OSAL_SUCCESS )
    {
      /* The queue is owned by the controller and outlives this task; a
       * receive failure can only happen while the owner is tearing down. */
      break;
    }

    if ( item.action == DEFER_ACTION_QUIT ) break;

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
    _defer_test_hold();
#endif
    _defer_apply_item( &item );
    /* The item is now fully applied (or dropped as stale): release it from the
     * unprocessed count so the test wait_idle helper can observe quiescence.
     * The producer counted the item before it entered the queue, so a zero
     * count proves no item is in flight. */
    (void) __sync_sub_and_fetch( &s_defer_unprocessed, 1u );
  }

  if ( s_defer_stopped_sem != NULL )
  {
    (void) osal_bin_sem_give( s_defer_stopped_sem );
  }
}

/* Start the deferred policy pipeline: create the FIFO queue and the worker
 * task that applies queued actions on its own thread. The queue pointer is
 * published before the task starts so the worker can block on it at once. The
 * worker is created before any event subscription or grace-timer arming, so
 * every event the controller can receive has a queue to post to. Returns
 * false (and cleans up after itself) when the queue or task cannot be
 * created. */
static bool _deferred_work_start( void )
{
  osal_queue_id_t q = NULL;
  osal_task_id_t  t = 0;

  if ( osal_queue_create( &q, "wifi_prov_defer", CTRL_DEFER_QUEUE_DEPTH,
                          sizeof( controller_deferred_t ) ) != OSAL_SUCCESS )
  {
    return false;
  }

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
  if ( osal_bin_sem_create( &s_defer_hold_sem, "wifi_prov_hold",
                            OSAL_SEM_EMPTY ) != OSAL_SUCCESS )
  {
    (void) osal_queue_delete( q );
    return false;
  }
#endif

  s_ctx.defer_queue = q;
  s_ctx.defer_ready = false;
  if ( osal_task_create( &t, "wifi_prov_defer", _defer_worker, NULL, NULL,
                         CTRL_DEFER_STACK_SIZE, CTRL_DEFER_PRIORITY,
                         NULL ) != OSAL_SUCCESS )
  {
    s_ctx.defer_queue = NULL;
#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
    (void) osal_bin_sem_delete( s_defer_hold_sem );
    s_defer_hold_sem = NULL;
#endif
    (void) osal_queue_delete( q );
    return false;
  }
  s_ctx.defer_task  = t;
  s_ctx.defer_ready = true;

  /* A fresh lifecycle starts unheld and hook-free: a previous lifecycle (or
   * test) must never leak its gate or observer into the new one. The new queue
   * also starts with a zero unprocessed count. */
  s_defer_unprocessed = 0u;
  _lock();
#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
  s_ctx.defer_hold = false;
  s_ctx.apply_hook = NULL;
  s_ctx.apply_hook_ctx = NULL;
#endif
  _unlock();

  return true;
}

/* Stop the deferred policy pipeline: post a QUIT marker (deinit invalidated
 * the session first, so any still-queued item is dropped by the worker), wait
 * briefly for the worker to observe it and then join the task, and only then
 * release the queue. This is the extension of the deinit join semantics:
 * after it returns no deferred item can be applied and no notification hook
 * can run on a worker thread. */
static void _deferred_work_stop( void )
{
  controller_deferred_t quit;
  osal_queue_id_t q;

  _lock();
#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
  /* Release a test-held worker first: the caller (deinit) already invalidated
   * the session, so a worker that unparks and applies the item it is holding
   * hits the stale-session drop path and then observes the QUIT marker. This
   * makes teardown drain deterministically instead of cancelling a parked
   * worker after the bounded wait below. */
  s_ctx.defer_hold = false;
#endif
  const bool ready = s_ctx.defer_ready;
  q = ready ? s_ctx.defer_queue : NULL;
  _unlock();

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
  if ( s_defer_hold_sem != NULL )
  {
    /* Wake the parked worker (if any) so it re-checks the released flag. */
    (void) osal_bin_sem_give( s_defer_hold_sem );
  }
#endif

  if ( q != NULL )
  {
    quit.action  = DEFER_ACTION_QUIT;
    quit.event   = (wifi_mgmt_event_t) 0;
    quit.session = 0u;
    (void) osal_queue_send( q, &quit, 0u );
  }

  if ( ready && s_defer_stopped_sem != NULL )
  {
    /* Bounded wait for the worker to drop stale items and observe QUIT; the
     * join below is the authoritative barrier and also covers a worker that
     * is parked on the test hold or stuck in a blocking call. */
    (void) osal_bin_sem_timed_wait( s_defer_stopped_sem, CTRL_DEFER_STOP_WAIT_MS );
  }

  if ( ready )
  {
    (void) osal_task_delete( s_ctx.defer_task );
  }
  _lock();
  s_ctx.defer_task  = 0;
  q                 = s_ctx.defer_queue;
  s_ctx.defer_queue = NULL;
  s_ctx.defer_ready = false;
  _unlock();
  if ( q != NULL ) (void) osal_queue_delete( q );

  /* The worker joined above has dropped (or applied) every queued item; zero
   * the unprocessed counter so the next lifecycle starts clean and a later
   * wait_idle cannot observe a stale count from the torn-down queue. */
  s_defer_unprocessed = 0u;

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
  _lock();
  s_ctx.defer_hold = false;
  s_ctx.apply_hook = NULL;
  s_ctx.apply_hook_ctx = NULL;
  _unlock();
  if ( s_defer_hold_sem != NULL )
  {
    (void) osal_bin_sem_delete( s_defer_hold_sem );
    s_defer_hold_sem = NULL;
  }
#endif
}

/* Grace timer expiry callback, delivered on the timer task. It never retires
 * the portal itself: the expiry is posted to the deferred pipeline and the
 * controller worker applies it. A stale expiry (a cancelled/prior window, a
 * disconnect, an explicit stop or a deinit superseding the session) is a
 * no-op thanks to the session/generation token; the worker drops it. */
static void _on_grace_timer_expired( osal_timer_id_t timer_id )
{
  controller_deferred_t item;
  osal_queue_id_t q;

  (void) timer_id;

  _lock();
  q = NULL;
  if ( s_ctx.enabled && s_ctx.defer_ready && s_ctx.defer_queue != NULL &&
       s_ctx.state == WIFI_PROVISIONING_CONTROLLER_GRACE &&
       s_ctx.grace_armed_session == s_ctx.session )
  {
    item.action  = DEFER_ACTION_GRACE_EXPIRED;
    item.session = s_ctx.session;
    q            = s_ctx.defer_queue;
  }
  _unlock();

  if ( q == NULL ) return;
  /* Count the item before it enters the queue (see _defer_post_event). */
  (void) __sync_add_and_fetch( &s_defer_unprocessed, 1u );
  if ( osal_queue_send( q, &item, 0u ) != OSAL_SUCCESS )
  {
    (void) __sync_sub_and_fetch( &s_defer_unprocessed, 1u );
  }
}

/* Event callback delivered on the Wi-Fi management event thread. It never
 * performs provisioning or mode-transition work: the raw event is posted to
 * the controller's deferred queue, and the deferred worker applies it in
 * arrival order on a controller-owned thread. */
static void _controller_on_event( wifi_mgmt_event_t event, void* user_data )
{
  (void) user_data;

  switch ( event )
  {
    case WIFI_MGMT_EVENT_CONNECTED:
    case WIFI_MGMT_EVENT_DISCONNECTED:
    case WIFI_MGMT_EVENT_CONNECT_FAILED:
    case WIFI_MGMT_EVENT_MODE_CHANGED:
      break;
    default:
      /* SCAN_COMPLETED is not a fallback trigger. */
      return;
  }

  (void) _defer_post_event( event );
}

/* Apply one deferred grace-timer expiry on the controller worker thread. Only
 * a portal that is still inside the current session's grace window is shut
 * down; a stale expiry (a superseded session, an abort, an explicit stop) is
 * a no-op thanks to the session/generation token. */
static void _controller_apply_grace_expiry( uint32_t session )
{
  _lock();
  const bool valid =
    s_ctx.enabled &&
    s_ctx.session == session &&
    s_ctx.state == WIFI_PROVISIONING_CONTROLLER_GRACE &&
    s_ctx.grace_armed_session == session;
  _unlock();

  if ( !valid ) return;
  (void) _retire_provisioning( true, session );
}

/* Apply one deferred Wi-Fi event on the controller worker thread. The item's
 * session token is re-validated under the controller lock; an item queued by
 * a session that has since been superseded (deinit/re-init) is dropped
 * without side effects. */
static void _controller_apply_event( const controller_deferred_t *item )
{
  const uint32_t session = item->session;

  switch ( item->event )
  {
    case WIFI_MGMT_EVENT_CONNECTED:
      /* If the station obtained an IP while the portal is active, the
       * submitted credential succeeded: enter the grace window. Otherwise
       * this is a saved-credential connect: retire the startup SoftAP and only
       * report ONLINE after the STA-only mode change is acknowledged. Every
       * successful connect resets the fallback failure budget so a later
       * cycle of failures is counted from a fresh baseline. */
      _lock();
      if ( !s_ctx.enabled || s_ctx.session != session ) { _unlock(); return; }
      s_ctx.fallback_failures = 0u;
      if ( s_ctx.state == WIFI_PROVISIONING_CONTROLLER_PROVISIONING )
      {
        _unlock();
        _start_grace( session );
      }
      else if ( s_ctx.state == WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT )
      {
        _unlock();
        (void) _retire_provisioning( true, session );
      }
      else
      {
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
        notify_snapshot_t snap = { NULL, NULL, 0u };

        _lock();
        state_before = s_ctx.state;
        _unlock();

        if ( state_before == WIFI_PROVISIONING_CONTROLLER_GRACE )
        {
          _abort_grace_window( session );
        }
        else if ( state_before == WIFI_PROVISIONING_CONTROLLER_ONLINE )
        {
          _lock();
          if ( s_ctx.enabled && s_ctx.session == session )
          {
            s_ctx.state = WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT;
            /* Re-arm the fallback budget for the new connect cycle: failures
             * accumulated before the device went ONLINE must not carry over
             * into the next cycle. */
            s_ctx.fallback_failures = 0u;
            _capture_notify( &snap );
          }
          _unlock();
          _fire_notify( &snap, WIFI_PROVISIONING_CONTROLLER_ONLINE,
                        WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT );
        }
      }
      break;

    case WIFI_MGMT_EVENT_CONNECT_FAILED:
      /* A saved-credential attempt failed while awaiting a connection: count
       * it against the fallback budget and open the portal once the budget is
       * exhausted (a bounded number of consecutive failures lets a transient
       * router reboot pass without surfacing the provisioning AP while a
       * genuinely exhausted credential still triggers fallback). A budget of 0
       * disables the failure-driven path entirely; the fresh-device start-on-
       * init path is unaffected. The portal is opened at most once per
       * fallback. A failed attempt inside the grace window only cancels the
       * timer and keeps the existing portal. */
      {
        wifi_provisioning_controller_state_t state_before;
        notify_snapshot_t snap = { NULL, NULL, 0u };

        _lock();
        state_before = s_ctx.state;
        _unlock();

        if ( state_before == WIFI_PROVISIONING_CONTROLLER_GRACE )
        {
          _abort_grace_window( session );
        }
        else if ( state_before == WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT )
        {
          bool budget_exhausted = false;
          bool entered_provisioning = false;

          _lock();
          /* The session token is re-validated before and after the blocking
           * start so a deinit or a fresh session that ran in the meantime is
           * not committed. */
          if ( s_ctx.enabled && s_ctx.session == session &&
               !s_ctx.fallback_started && s_ctx.fallback_budget > 0u )
          {
            ++s_ctx.fallback_failures;
            if ( s_ctx.fallback_failures >= s_ctx.fallback_budget )
            {
              s_ctx.fallback_started = true;
              s_ctx.fallback_failures = 0u;
              budget_exhausted = true;
            }
          }
          _unlock();

          if ( budget_exhausted )
          {
            (void) wifi_http_provisioning_start();
            _lock();
            if ( s_ctx.enabled && s_ctx.session == session )
            {
              s_ctx.state = WIFI_PROVISIONING_CONTROLLER_PROVISIONING;
              entered_provisioning = true;
              _capture_notify( &snap );
            }
            _unlock();
            if ( entered_provisioning )
            {
              _fire_notify( &snap, WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                            WIFI_PROVISIONING_CONTROLLER_PROVISIONING );
            }
          }
        }
      }
      break;

    case WIFI_MGMT_EVENT_MODE_CHANGED:
      /* The asynchronous completion of a STA-only retirement: only now may
       * the controller report ONLINE (or, for an explicit stop, DISABLED).
       * A stale confirmation after a newer session is ignored. */
      {
        wifi_provisioning_controller_state_t next = WIFI_PROVISIONING_CONTROLLER_DISABLED;
        notify_snapshot_t snap = { NULL, NULL, 0u };

        _lock();
        if ( s_ctx.enabled && s_ctx.session == session &&
             s_ctx.state == WIFI_PROVISIONING_CONTROLLER_RETIRING_AP )
        {
          if ( s_ctx.retire_online )
          {
            next = WIFI_PROVISIONING_CONTROLLER_ONLINE;
          }
          else
          {
            next = WIFI_PROVISIONING_CONTROLLER_DISABLED;
          }
          s_ctx.state         = next;
          s_ctx.retire_online = false;
          _capture_notify( &snap );
        }
        _unlock();
        _fire_notify( &snap, WIFI_PROVISIONING_CONTROLLER_RETIRING_AP, next );
      }
      break;

    default:
      /* SCAN_COMPLETED is not a fallback trigger. */
      break;
  }
}

/* Public API ------------------------------------------------------------ */

bool wifi_provisioning_controller_init( void )
{
  return wifi_provisioning_controller_init_with_config( NULL );
}

bool wifi_provisioning_controller_init_with_config(
    const wifi_provisioning_controller_config_t *config )
{
  bool timer_ready = false;
  bool entered_provisioning = false;
  wifi_provisioning_controller_state_t prev;
  notify_snapshot_t snap = { NULL, NULL, 0u };

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

  /* The deferred policy pipeline must exist before any event subscription:
   * a failure here aborts init before any controller state is mutated. */
  if ( !_deferred_work_start() )
  {
    (void) osal_mutex_give( s_lifecycle_lock );
    return false;
  }

  _lock();
  prev                     = s_ctx.state;
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
  /* Install the opt-in state-change hook from the init-time configuration.
   * A NULL config (and a NULL hook) disables notifications for this fresh
   * lifecycle; a NULL config also clears any hook left over from an earlier
   * lifecycle so plain init() stays notification-free. */
  if ( config != NULL )
  {
    s_ctx.notify_cb       = config->on_state_changed;
    s_ctx.notify_user_ctx = config->user_ctx;
  }
  else
  {
    s_ctx.notify_cb       = NULL;
    s_ctx.notify_user_ctx = NULL;
  }
  /* Fallback budget: a per-device init-config override wins over the build
   * default (Kconfig CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS). A fresh
   * lifecycle always starts with a re-armed (zeroed) failure counter. */
  if ( config != NULL && config->fallback_budget_set )
  {
    s_ctx.fallback_budget = config->fallback_budget;
  }
  else
  {
    s_ctx.fallback_budget = CONFIG_WIFI_HTTP_PROVISIONING_FALLBACK_ATTEMPTS;
  }
  s_ctx.fallback_failures = 0u;
  s_ctx.state = WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT;
  _capture_notify( &snap );
  _unlock();
  _fire_notify( &snap, prev, WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT );

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
      entered_provisioning = true;
      _capture_notify( &snap );
    }
    _unlock();
    if ( entered_provisioning )
    {
      _fire_notify( &snap, WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT,
                    WIFI_PROVISIONING_CONTROLLER_PROVISIONING );
    }
  }

  (void) osal_mutex_give( s_lifecycle_lock );
  return true;
}

void wifi_provisioning_controller_deinit( void )
{
  wifi_provisioning_controller_state_t prev;
  notify_snapshot_t snap = { NULL, NULL, 0u };

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

  /* Invalidate the lifecycle before stopping the worker: the session bump
   * makes every still-queued deferred item stale, so the worker drops it
   * instead of applying it. The final DISABLED notification is fired with the
   * still-registered hook and the fresh session token; afterwards the hook is
   * cleared so a later lifecycle without a new configuration stays
   * notification-free. */
  _lock();
  prev                     = s_ctx.state;
  s_ctx.enabled            = false;
  s_ctx.fallback_started   = false;
  s_ctx.fallback_failures  = 0u;
  s_ctx.retire_online      = false;
  s_ctx.state              = WIFI_PROVISIONING_CONTROLLER_DISABLED;
  ++s_ctx.session;
  _capture_notify( &snap );
  s_ctx.notify_cb          = NULL;
  s_ctx.notify_user_ctx    = NULL;
  _unlock();
  _fire_notify( &snap, prev, WIFI_PROVISIONING_CONTROLLER_DISABLED );

  /* Join the deferred worker and release the queue. Pending items carry the
   * invalidated session and are dropped; after this returns no deferred item
   * can be applied and no notification hook can run on a worker thread. */
  _deferred_work_stop();

  /* Stop and delete the grace timer only after the worker has been joined:
   * the worker (or a concurrently stopping API caller, serialized on the
   * lifecycle lock) is the only other arm/stop owner of the timer, so once it
   * is joined no in-flight change_period/start can race the teardown.
   * osal_timer_delete joins the timer thread on POSIX, so any expiry callback
   * (which, after the session bump above, only refrains from posting) also
   * completes here. */
  _lock();
  const bool do_stop   = s_ctx.grace_timer_active && s_ctx.grace_timer_ready;
  const bool do_delete = s_ctx.grace_timer_ready;
  s_ctx.grace_timer_active = false;
  s_ctx.grace_timer_ready  = false;
  _unlock();
  if ( do_stop )   (void) osal_timer_stop( s_ctx.grace_timer, 0u );
  if ( do_delete ) (void) osal_timer_delete( s_ctx.grace_timer, 0u );

  (void) osal_mutex_give( s_lifecycle_lock );
}

bool wifi_provisioning_controller_stop( void )
{
  uint32_t session;

  if ( !_ensure_lock() ) return false;

  (void) osal_mutex_take( s_lifecycle_lock );
  _lock();
  const bool active = s_ctx.enabled;
  session           = s_ctx.session;
  _unlock();
  if ( !active )
  {
    (void) osal_mutex_give( s_lifecycle_lock );
    return false;
  }

  const bool stopped = _retire_provisioning( false, session );
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

void wifi_provisioning_controller_test_set_apply_hook(
    wifi_provisioning_controller_test_apply_cb_t cb, void *user_ctx )
{
  if ( !_ensure_lock() ) return;

  _lock();
  s_ctx.apply_hook     = cb;
  s_ctx.apply_hook_ctx = user_ctx;
  _unlock();
}

void wifi_provisioning_controller_test_set_defer_hold( bool hold )
{
  if ( !_ensure_lock() ) return;

  _lock();
  s_ctx.defer_hold = hold;
  _unlock();
  if ( !hold && s_defer_hold_sem != NULL )
  {
    /* Wake a parked worker so it re-checks the flag and resumes applying. */
    (void) osal_bin_sem_give( s_defer_hold_sem );
  }
}

bool wifi_provisioning_controller_test_inject_event( wifi_mgmt_event_t event )
{
  return _defer_post_event( event );
}

bool wifi_provisioning_controller_test_wait_idle( uint32_t timeout_ms )
{
  uint32_t start;

  if ( !_ensure_lock() ) return false;
  start = osal_task_get_time_ms();
  for ( ;; )
  {
    /* Deferred items are counted from before they enter the queue until after
     * the worker applied them, so a zero count is a race-free idle signal:
     * an item can never be invisible to both the queue and the counter. */
    if ( s_defer_unprocessed == 0u ) return true;
    if ( ( osal_task_get_time_ms() - start ) >= timeout_ms ) return false;
    (void) osal_task_delay_ms( 1u );
  }
}
#endif    /* WIFI_PROVISIONING_TEST_OBSERVABILITY */

#endif    /* CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK */