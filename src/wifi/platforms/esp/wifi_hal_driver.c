/**
 *******************************************************************************
 * @file    wifi_hal_driver.c
 * @author  Dmytro Shevchenko
 * @brief   Wi-Fi HAL — ESP-IDF implementation
 *
 *          Implements the TASK-134A teardown contract with an ESP-owned
 *          lifecycle state machine.  Init/deinit are serialized by a
 *          process-lifetime lifecycle mutex and an explicit state machine with
 *          a CLEANUP_REQUIRED state.  Each platform resource owned by this HAL
 *          (the callback gate, the callback completion primitive, the two
 *          default Wi-Fi netifs, the Wi-Fi driver init/start state and the two
 *          event-handler instance registrations) is released by a staged,
 *          retry-safe teardown shared between failed-init unwind and deinit.
 *          esp_netif_init() and the default event loop are process facilities
 *          that are not torn down here.
 *******************************************************************************
 */

#include "wifi_hal_driver.h"

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "osal_bin_sem.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/*  Lifecycle state machine                                                    */
/* -------------------------------------------------------------------------- */

typedef enum
{
  HAL_STATE_UNINITIALIZED = 0, /**< No session; init may create one.            */
  HAL_STATE_ACTIVE,            /**< Session up; operation admission open.       */
  HAL_STATE_DEINITIALIZING,    /**< A lifecycle transition owns the teardown.   */
  HAL_STATE_CLEANUP_REQUIRED,  /**< Teardown incomplete; deinit retry owns it.  */
} hal_state_t;

/* Owned-resource release outcome returned by _teardown_session(). */
typedef enum
{
  TEARDOWN_DONE = 0, /**< Every owned resource was released.               */
  TEARDOWN_RETAINED, /**< Some resource is still owned; a retry is needed. */
} teardown_result_t;

/* --------------------------------------------------------------------------- */
/*  Session state                                                               */
/* --------------------------------------------------------------------------- */

typedef struct
{
  /* Wi-Fi driver ownership: esp_wifi_init()/esp_wifi_start() state.  The
   * esp_netif_init() call and the default event loop are process facilities
   * and are not tracked here. */
  bool         wifi_inited; /**< True once esp_wifi_init() succeeded.         */
  bool         started;     /**< True while this session started Wi-Fi.        */

  /* The two default Wi-Fi netifs created by this HAL. */
  esp_netif_t* netif_sta;
  esp_netif_t* netif_ap;
  bool         netif_sta_created;
  bool         netif_ap_created;

  /* Event handler instance registrations, tracked by the token returned by
   * esp_event_handler_instance_register().  A token is set only after ESP_OK
   * and cleared only after its matching instance unregister returned ESP_OK. */
  esp_event_handler_instance_t handler_wifi; /**< WIFI_EVENT instance token.     */
  bool                         handler_wifi_registered;
  esp_event_handler_instance_t handler_ip;   /**< IP_EVENT STA_GOT_IP token.     */
  bool                         handler_ip_registered;

  /* Callback delivery gate and completion primitive.  cb_lock guards cb,
   * cb_user_data, cb_delivery_disabled and cb_in_flight; the user callback is
   * always invoked outside every lifecycle/callback lock.  cb_done is given
   * when the final in-flight callback returns, so teardown waits on a
   * completion primitive instead of a 1 ms polling loop. */
  bool               cb_delivery_disabled;
  uint32_t           cb_in_flight;
  wifi_hal_event_cb_t cb;
  void*              cb_user_data;
  osal_bin_sem_id_t  cb_lock;
  osal_bin_sem_id_t  cb_done;

  /* Configuration and cached runtime values. */
  wifi_hal_sta_config_t sta_cfg;
  wifi_hal_ap_config_t  ap_cfg;
  uint32_t              client_count;
} wifi_hal_ctx_t;

/* --------------------------------------------------------------------------- */
/* Process-lifetime lifecycle state                                            */
/* --------------------------------------------------------------------------- */

/**
 * The lifecycle mutex/state pair is process-lifetime so init and deinit stay
 * serialized even across a failed teardown (a later deinit must own the
 * cleanup).  The mutex is a single FreeRTOS mutex created exactly once and
 * never deleted.  One-time creation is guarded by a statically-initialized
 * portMUX critical section so every concurrent first-use caller observes and
 * shares the same fully-created serializer instead of each creating a private
 * one (FreeRTOS has no native one-time initializer).
 *
 * The lifecycle condition uses one task-notification slot per waiter.  A
 * waiter is linked while it still owns the lifecycle mutex, then releases that
 * mutex and waits on its own task notification.  Broadcast walks the linked
 * waiters while holding the same mutex, so no wakeup can be lost between
 * registration and blocking or bounded registry capacity be exceeded.
 */
typedef struct lifecycle_waiter
{
  TaskHandle_t              handle;
  struct lifecycle_waiter*  next;
} lifecycle_waiter_t;

static wifi_hal_ctx_t          g_wifi_hal_ctx       = { 0 };
static wifi_ap_record_t        scan_ap_records[64]  = { 0 };

static hal_state_t             g_lifecycle_state    = HAL_STATE_UNINITIALIZED;
static osal_mutex_id_t         g_lifecycle_mutex    = 0;
static bool                    g_lifecycle_ready    = false;
static portMUX_TYPE            g_lifecycle_once_mux = portMUX_INITIALIZER_UNLOCKED;
static lifecycle_waiter_t*     g_lifecycle_waiters  = NULL;
static uint32_t                g_admitted_ops       = 0;
/* ESP event handlers may execute concurrently on more than one task.  A
 * single shared callback-task marker cannot identify all callbacks, while a
 * task-local depth also handles nested event delivery correctly. */
static __thread uint32_t       g_callback_depth     = 0;

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )
static StaticSemaphore_t       g_lifecycle_mutex_storage;
#endif

/* --------------------------------------------------------------------------- */
/* Small helpers                                                                */
/* --------------------------------------------------------------------------- */

static osal_status_t _esp_to_status( esp_err_t err )
{
  return ( err == ESP_OK ) ? OSAL_SUCCESS : OSAL_ERROR;
}

/* --------------------------------------------------------------------------- */
/* Process-lifetime lifecycle serializer                                       */
/* --------------------------------------------------------------------------- */

/* Create the process-lifetime lifecycle mutex exactly once.  The static
 * portMUX protects first use, including a concurrent first init and deinit.
 * The dynamic fallback keeps this source usable with an ESP-IDF configuration
 * that disables static FreeRTOS allocation. */
static bool _ensure_lifecycle( void )
{
  bool ready;

  /* Enter the guard even for the fast path.  This makes the publication of
   * both handles and g_lifecycle_ready ordered on SMP targets as well as on
   * single-core FreeRTOS configurations. */
  portENTER_CRITICAL( &g_lifecycle_once_mux );
  if ( !g_lifecycle_ready )
  {
    if ( !g_lifecycle_mutex )
    {
#if ( configSUPPORT_STATIC_ALLOCATION == 1 )
      g_lifecycle_mutex = xSemaphoreCreateMutexStatic( &g_lifecycle_mutex_storage );
#else
      (void) osal_mutex_create( &g_lifecycle_mutex, "wifi_hal_lc" );
#endif
    }
    g_lifecycle_ready = ( g_lifecycle_mutex != 0 );
  }
  ready = g_lifecycle_ready;
  portEXIT_CRITICAL( &g_lifecycle_once_mux );
  return ready;
}

/* Wake every waiter currently linked in _cond_wait().  The caller owns the
 * lifecycle mutex.  Each waiter has its own task-notification slot and remains
 * linked until it has reacquired the mutex, so no wakeup can race with
 * registration, blocking, or removal. */
static void _cond_broadcast( void )
{
  lifecycle_waiter_t* waiter = g_lifecycle_waiters;
  while ( waiter )
  {
    if ( waiter->handle )
    {
      (void) xTaskNotifyGive( waiter->handle );
    }
    waiter = waiter->next;
  }
}

/* Wait while holding the lifecycle mutex.  Registration and releasing the
 * mutex are separated from blocking only by this waiter's linked-list entry;
 * broadcasters hold the same mutex and therefore either see the waiter and
 * signal it, or run before it is registered.  The caller retains ownership of
 * the mutex when this returns. */
static bool _cond_wait( void )
{
  lifecycle_waiter_t waiter = { 0 };
  lifecycle_waiter_t** link;

  waiter.handle = xTaskGetCurrentTaskHandle();
  if ( !waiter.handle )
  {
    return false;
  }

  waiter.next = g_lifecycle_waiters;
  g_lifecycle_waiters = &waiter;
  (void) osal_mutex_give( g_lifecycle_mutex );
  (void) ulTaskNotifyTake( pdTRUE, portMAX_DELAY );
  (void) osal_mutex_take( g_lifecycle_mutex );

  link = &g_lifecycle_waiters;
  while ( *link && *link != &waiter )
  {
    link = &( *link )->next;
  }
  if ( *link == &waiter )
  {
    *link = waiter.next;
  }
  return true;
}

/* Grant a "session lease" to a public operation.  Returns true only when the
 * HAL is ACTIVE, in which case the operation is counted; deinit closes
 * admission and waits for the drain on the same lifecycle signal.  A callback
 * that re-enters a HAL API while teardown is in progress receives the
 * rejection here and cannot start a new platform operation. */
static bool _admit_operation( void )
{
  if ( !g_lifecycle_mutex )
  {
    return false;
  }
  (void) osal_mutex_take( g_lifecycle_mutex );
  bool admitted = ( g_lifecycle_state == HAL_STATE_ACTIVE );
  if ( admitted )
  {
    g_admitted_ops++;
  }
  (void) osal_mutex_give( g_lifecycle_mutex );
  return admitted;
}

static void _release_operation( void )
{
  (void) osal_mutex_take( g_lifecycle_mutex );
  if ( g_admitted_ops > 0 )
  {
    g_admitted_ops--;
    if ( g_admitted_ops == 0 )
    {
      _cond_broadcast();
    }
  }
  (void) osal_mutex_give( g_lifecycle_mutex );
}

/* Detect a lifecycle call made from inside an in-flight user callback.  Such
 * a call must be rejected without touching the lifecycle serializer: the
 * teardown that would satisfy its wait is the very teardown that is draining
 * this callback, so waiting on it here would self-deadlock.  The marker is
 * task-local, allowing concurrent callbacks and nested delivery. */
static bool _lifecycle_call_from_callback( void )
{
  return g_callback_depth != 0;
}

/* --------------------------------------------------------------------------- */
/* IPv4 validator                                                               */
/* --------------------------------------------------------------------------- */

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

/* --------------------------------------------------------------------------- */
/* Callback delivery                                                            */
/* --------------------------------------------------------------------------- */

/* Admit one event through a stable lifetime protocol.  The lifecycle mutex is
 * process-lifetime and is acquired before inspecting cb_lock; teardown changes
 * the state while holding that mutex, so it cannot delete cb_lock or cb_done
 * while an event handler is between its pointer check and gate acquisition.
 * Both locks are released before invoking user code. */
static void _emit_event( wifi_hal_event_t event, const wifi_hal_event_data_t* data )
{
  wifi_hal_event_cb_t cb;
  void*               user_data;

  if ( !g_lifecycle_mutex )
  {
    return;
  }
  (void) osal_mutex_take( g_lifecycle_mutex );
  if ( g_lifecycle_state != HAL_STATE_ACTIVE || !g_wifi_hal_ctx.cb_lock )
  {
    (void) osal_mutex_give( g_lifecycle_mutex );
    return;
  }

  (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_lock );
  if ( g_wifi_hal_ctx.cb_delivery_disabled || g_wifi_hal_ctx.cb == NULL )
  {
    (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );
    (void) osal_mutex_give( g_lifecycle_mutex );
    return;
  }

  if ( event == WIFI_HAL_EVT_AP_CLIENT_CONNECTED )
  {
    g_wifi_hal_ctx.client_count++;
  }
  else if ( event == WIFI_HAL_EVT_AP_CLIENT_DISCONNECTED && g_wifi_hal_ctx.client_count > 0 )
  {
    g_wifi_hal_ctx.client_count--;
  }
  cb        = g_wifi_hal_ctx.cb;
  user_data = g_wifi_hal_ctx.cb_user_data;
  g_wifi_hal_ctx.cb_in_flight++;
  (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );
  (void) osal_mutex_give( g_lifecycle_mutex );

  g_callback_depth++;
  cb( event, data, user_data );
  g_callback_depth--;

  (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_lock );
  if ( g_wifi_hal_ctx.cb_in_flight > 0 )
  {
    g_wifi_hal_ctx.cb_in_flight--;
  }
  if ( g_wifi_hal_ctx.cb_in_flight == 0 && g_wifi_hal_ctx.cb_done )
  {
    /* Acknowledge the final in-flight callback with the completion primitive. */
    (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_done );
  }
  (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );
}

/* Disable new deliveries, wait for every in-flight callback through the
 * completion semaphore and clear the callback/user-data pointers.  The event
 * handlers must already be unregistered (or retained only behind the disabled
 * flag) and cb_lock/cb_done must still exist for a callback to use them. */
static void _quiesce_gate( void )
{
  if ( !g_wifi_hal_ctx.cb_lock || !g_wifi_hal_ctx.cb_done )
  {
    /* Neither a gate nor a completion primitive exists: no callback can be in
     * flight (creation failed or a prior teardown released them).  Clear any
     * stored pointers and return. */
    g_wifi_hal_ctx.cb           = NULL;
    g_wifi_hal_ctx.cb_user_data = NULL;
    return;
  }

  (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_lock );
  g_wifi_hal_ctx.cb_delivery_disabled = true;
  (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );

  /* Discard the (at most one) stale binary completion token left by an earlier
   * normal event so the wait below observes the current drain's final
   * transition to zero. */
  (void) osal_bin_sem_timed_wait( g_wifi_hal_ctx.cb_done, 0 );

  (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_lock );
  while ( g_wifi_hal_ctx.cb_in_flight > 0 )
  {
    /* Release the gate while waiting; the in-flight callback needs it to
     * acknowledge its own completion. */
    (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );
    (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_done );
    (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_lock );
  }

  g_wifi_hal_ctx.cb           = NULL;
  g_wifi_hal_ctx.cb_user_data = NULL;
  (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );
}

/* --------------------------------------------------------------------------- */
/* Staged, retry-safe teardown                                                  */
/* --------------------------------------------------------------------------- */

/* Release every owned resource in dependency-safe order (the same staged
 * routine used by a failed init unwind and by deinit):
 *   (1) disable new callback delivery,
 *   (2) unregister every retained handler instance (token cleared only on ESP_OK),
 *   (3) wait for in-flight callbacks via the completion primitive and clear
 *       callback/user data,
 *   (4) stop Wi-Fi if started; only on confirmed stop continue,
 *   (5) deinitialize the Wi-Fi driver only after stop succeeded (or it was
 *       already stopped / never initialized),
 *   (6) destroy each owned default netif only after driver deinit succeeded
 *       (or the driver was never initialized),
 *   (7) delete the callback synchronization only when no retained handler or
 *       callback can still use it.
 *
 * On any failure the exact per-resource flags are preserved and the teardown
 * stops before destroying a resource that is still required, so a retry
 * performs the exact same cleanup again.  The caller must NOT hold the
 * lifecycle mutex while this runs. */
static teardown_result_t _teardown_session( void )
{
  bool incomplete = false;

  /* (1) Disable delivery FIRST so a still-registered handler cannot start a
   * new callback after this point, even if the event loop still dispatches
   * it. */
  if ( g_wifi_hal_ctx.cb_lock )
  {
    (void) osal_bin_sem_take( g_wifi_hal_ctx.cb_lock );
    g_wifi_hal_ctx.cb_delivery_disabled = true;
    (void) osal_bin_sem_give( g_wifi_hal_ctx.cb_lock );
  }
  else
  {
    g_wifi_hal_ctx.cb_delivery_disabled = true;
  }

  /* (2) Unregister each retained event-handler instance independently.  A
   * token is cleared only when its instance unregister returned ESP_OK; ANY
   * other result (in particular ESP_ERR_INVALID_ARG) is NOT treated as proof
   * of absence, so the token and the registered flag are kept and this stage
   * is retried exactly. */
  if ( g_wifi_hal_ctx.handler_wifi_registered )
  {
    if ( !g_wifi_hal_ctx.handler_wifi )
    {
      /* A registered instance without its token cannot be safely guessed at
       * or replaced; preserve the ownership state for a later retry. */
      incomplete = true;
    }
    else
    {
      esp_err_t uerr = esp_event_handler_instance_unregister( WIFI_EVENT,
                                                              ESP_EVENT_ANY_ID,
                                                              g_wifi_hal_ctx.handler_wifi );
      if ( uerr == ESP_OK )
      {
        g_wifi_hal_ctx.handler_wifi            = NULL;
        g_wifi_hal_ctx.handler_wifi_registered = false;
      }
      else
      {
        incomplete = true;
      }
    }
  }
  if ( g_wifi_hal_ctx.handler_ip_registered )
  {
    if ( !g_wifi_hal_ctx.handler_ip )
    {
      /* See the WIFI_EVENT instance case above. */
      incomplete = true;
    }
    else
    {
      esp_err_t uerr = esp_event_handler_instance_unregister( IP_EVENT,
                                                              IP_EVENT_STA_GOT_IP,
                                                              g_wifi_hal_ctx.handler_ip );
      if ( uerr == ESP_OK )
      {
        g_wifi_hal_ctx.handler_ip            = NULL;
        g_wifi_hal_ctx.handler_ip_registered = false;
      }
      else
      {
        incomplete = true;
      }
    }
  }

  /* (3) Wait for every in-flight callback to return and clear callback state.
   * Even when a handler could not be unregistered, delivery is already
   * disabled so it can no longer reach the user through this gate.  A retained
   * registration is a hard dependency failure: do not stop the driver or
   * destroy netifs while its exact token still needs to be retried. */
  (void) _quiesce_gate();
  if ( incomplete )
  {
    osal_log_error( "[wifi-hal] teardown: handler unregister incomplete, retry" );
    return TEARDOWN_RETAINED;
  }

  /* (4) Stop Wi-Fi if this session started it.  The idempotent return codes
   * accepted as "already stopped / not started / driver not installed" are
   * ESP_OK, ESP_ERR_WIFI_NOT_STARTED and ESP_ERR_WIFI_NOT_INIT.  On any other
   * error the started flag is kept and no dependent cleanup is attempted. */
  if ( g_wifi_hal_ctx.started )
  {
    esp_err_t serr = esp_wifi_stop();
    if ( serr == ESP_OK || serr == ESP_ERR_WIFI_NOT_STARTED || serr == ESP_ERR_WIFI_NOT_INIT )
    {
      g_wifi_hal_ctx.started = false;
    }
    else
    {
      osal_log_error( "[wifi-hal] teardown: esp_wifi_stop failed (0x%x), retry",
                      (unsigned) serr );
      return TEARDOWN_RETAINED;
    }
  }

  /* (5) Deinitialize the Wi-Fi driver only after stop succeeded (or was
   * already stopped).  ESP_ERR_WIFI_NOT_INIT means the driver is not installed
   * and is accepted as already released.  On success the started flag is also
   * cleared so a session never keeps started==true once the driver is no
   * longer initialized. */
  if ( g_wifi_hal_ctx.wifi_inited )
  {
    esp_err_t derr = esp_wifi_deinit();
    if ( derr == ESP_OK || derr == ESP_ERR_WIFI_NOT_INIT )
    {
      g_wifi_hal_ctx.wifi_inited = false;
      g_wifi_hal_ctx.started     = false;
    }
    else
    {
      osal_log_error( "[wifi-hal] teardown: esp_wifi_deinit failed (0x%x), retry",
                      (unsigned) derr );
      return TEARDOWN_RETAINED;
    }
  }

  /* (6) Destroy each owned default netif exactly once: clear both the pointer
   * and the ownership bit immediately after the (void) destroy call, so a
   * retry never calls destroy on an already-released netif. */
  if ( g_wifi_hal_ctx.netif_sta_created )
  {
    if ( !g_wifi_hal_ctx.netif_sta )
    {
      /* An owned netif without its handle cannot be safely released. */
      incomplete = true;
    }
    else
    {
      esp_netif_destroy_default_wifi( g_wifi_hal_ctx.netif_sta );
      g_wifi_hal_ctx.netif_sta         = NULL;
      g_wifi_hal_ctx.netif_sta_created = false;
    }
  }
  if ( g_wifi_hal_ctx.netif_ap_created )
  {
    if ( !g_wifi_hal_ctx.netif_ap )
    {
      /* An owned netif without its handle cannot be safely released. */
      incomplete = true;
    }
    else
    {
      esp_netif_destroy_default_wifi( g_wifi_hal_ctx.netif_ap );
      g_wifi_hal_ctx.netif_ap         = NULL;
      g_wifi_hal_ctx.netif_ap_created = false;
    }
  }

  /* (7) Delete the callback synchronization only when no retained handler can
   * dispatch into it and no callback is in flight.  If an unregister failed,
   * the retained handler could still reach the gate/completion primitives, so
   * they must stay alive for a safe retry. */
  if ( g_wifi_hal_ctx.handler_wifi_registered || g_wifi_hal_ctx.handler_ip_registered )
  {
    incomplete = true;
  }

  if ( !incomplete )
  {
    if ( g_wifi_hal_ctx.cb_done )
    {
      if ( osal_bin_sem_delete( g_wifi_hal_ctx.cb_done ) == OSAL_SUCCESS )
      {
        g_wifi_hal_ctx.cb_done = 0;
      }
      else
      {
        incomplete = true;
      }
    }
    if ( g_wifi_hal_ctx.cb_lock )
    {
      if ( osal_bin_sem_delete( g_wifi_hal_ctx.cb_lock ) == OSAL_SUCCESS )
      {
        g_wifi_hal_ctx.cb_lock = 0;
      }
      else
      {
        incomplete = true;
      }
    }
  }

  if ( incomplete )
  {
    osal_log_error( "[wifi-hal] teardown incomplete, retry required" );
  }
  return incomplete ? TEARDOWN_RETAINED : TEARDOWN_DONE;
}

/* --------------------------------------------------------------------------- */
/* Event handler                                                                */
/* --------------------------------------------------------------------------- */

static void _wifi_event_handler( void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data )
{
  (void) arg;

  if ( event_base == WIFI_EVENT )
  {
    if ( event_id == WIFI_EVENT_STA_DISCONNECTED )
    {
      wifi_hal_event_data_t                 event       = { 0 };
      wifi_event_sta_disconnected_t*       disconnected = (wifi_event_sta_disconnected_t*) event_data;
      event.disconnect_reason                          = disconnected->reason;
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
      _emit_event( WIFI_HAL_EVT_AP_CLIENT_CONNECTED, NULL );
      return;
    }

    if ( event_id == WIFI_EVENT_AP_STADISCONNECTED )
    {
      _emit_event( WIFI_HAL_EVT_AP_CLIENT_DISCONNECTED, NULL );
      return;
    }
  }

  if ( event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP )
  {
    wifi_hal_event_data_t data   = { 0 };
    ip_event_got_ip_t*    got_ip = (ip_event_got_ip_t*) event_data;
    esp_ip4addr_ntoa( &got_ip->ip_info.ip, data.ip_info.ip, sizeof( data.ip_info.ip ) );
    esp_ip4addr_ntoa( &got_ip->ip_info.netmask, data.ip_info.netmask, sizeof( data.ip_info.netmask ) );
    esp_ip4addr_ntoa( &got_ip->ip_info.gw, data.ip_info.gw, sizeof( data.ip_info.gw ) );
    _emit_event( WIFI_HAL_EVT_STA_GOT_IP, &data );
  }
}

/* --------------------------------------------------------------------------- */
/* Config helpers                                                               */
/* --------------------------------------------------------------------------- */

static void _copy_sta_config( wifi_config_t* out, const wifi_hal_sta_config_t* in )
{
  memset( out, 0, sizeof( *out ) );
  strncpy( (char*) out->sta.ssid, in->ssid, sizeof( out->sta.ssid ) - 1 );
  strncpy( (char*) out->sta.password, in->password, sizeof( out->sta.password ) - 1 );
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

/* Configure the DNS address advertised by the soft-AP DHCP server.  All
 * ESP-IDF calls stay inside this ESP HAL. */
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

  esp_netif_dns_info_t dns_info = { 0 };
  dns_info.ip.type             = ESP_IPADDR_TYPE_V4;
  dns_info.ip.u_addr.ip4       = dns_ip;
  if ( esp_netif_set_dns_info( g_wifi_hal_ctx.netif_ap, ESP_NETIF_DNS_MAIN, &dns_info ) != ESP_OK )
  {
    return OSAL_ERROR;
  }

  uint8_t offer = 1;
  if ( esp_netif_dhcps_option( g_wifi_hal_ctx.netif_ap, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof( offer ) ) != ESP_OK )
  {
    return OSAL_ERROR;
  }

  return OSAL_SUCCESS;
}

/* --------------------------------------------------------------------------- */
/* HAL lifecycle                                                                */
/* --------------------------------------------------------------------------- */

osal_status_t wifi_hal_init( const wifi_hal_init_t* init )
{
  if ( !init || !init->event_cb )
  {
    return OSAL_INVALID_POINTER;
  }
  if ( _lifecycle_call_from_callback() )
  {
    osal_log_error( "[wifi-hal] init rejected from event callback" );
    return OSAL_ERROR;
  }
  if ( !_ensure_lifecycle() )
  {
    return OSAL_ERROR;
  }

  (void) osal_mutex_take( g_lifecycle_mutex );

  /* Wait for an in-progress teardown, then re-check the state.  A retained
   * CLEANUP_REQUIRED session is not replaceable: only a later deinit may own
   * and finish its cleanup, so init fails without touching that session. */
  while ( g_lifecycle_state == HAL_STATE_DEINITIALIZING )
  {
    if ( !_cond_wait() )
    {
      (void) osal_mutex_give( g_lifecycle_mutex );
      return OSAL_ERROR;
    }
  }

  if ( g_lifecycle_state == HAL_STATE_CLEANUP_REQUIRED )
  {
    (void) osal_mutex_give( g_lifecycle_mutex );
    osal_log_error( "[wifi-hal] init: CLEANUP_REQUIRED, deinit first" );
    return OSAL_ERROR;
  }

  if ( g_lifecycle_state == HAL_STATE_ACTIVE )
  {
    /* A repeated init is a no-op only for a fully active session: never
     * memset or re-create live session objects. */
    (void) osal_mutex_give( g_lifecycle_mutex );
    return OSAL_SUCCESS;
  }

  /* UNINITIALIZED: this caller owns the transition to ACTIVE. */
  memset( &g_wifi_hal_ctx, 0, sizeof( g_wifi_hal_ctx ) );

  teardown_result_t rc = TEARDOWN_RETAINED;

  /* Callback delivery gate and completion primitive — created first so every
   * unwind path can release them and every delivery is protected. */
  if ( osal_bin_sem_create( &g_wifi_hal_ctx.cb_lock, "wifi_hal_cb", OSAL_SEM_FULL ) != OSAL_SUCCESS )
  {
    goto unwind;
  }

  if ( osal_bin_sem_create( &g_wifi_hal_ctx.cb_done, "wifi_hal_cbdone", OSAL_SEM_EMPTY ) != OSAL_SUCCESS )
  {
    goto unwind;
  }

  g_wifi_hal_ctx.cb           = init->event_cb;
  g_wifi_hal_ctx.cb_user_data = init->user_data;

  /* esp_netif_init() and the default event loop are process facilities: they
   * are ensured here but ownership is never acquired, so they are not torn
   * down by this HAL. */
  {
    esp_err_t err = esp_netif_init();
    if ( err != ESP_OK && err != ESP_ERR_INVALID_STATE )
    {
      osal_log_error( "[wifi-hal] init: esp_netif_init failed (0x%x)", (unsigned) err );
      goto unwind;
    }
  }
  {
    esp_err_t err = esp_event_loop_create_default();
    if ( err != ESP_OK && err != ESP_ERR_INVALID_STATE )
    {
      osal_log_error( "[wifi-hal] init: default event loop failed (0x%x)", (unsigned) err );
      goto unwind;
    }
  }

  g_wifi_hal_ctx.netif_ap = esp_netif_create_default_wifi_ap();
  if ( !g_wifi_hal_ctx.netif_ap )
  {
    osal_log_error( "[wifi-hal] init: default AP netif failed" );
    goto unwind;
  }
  g_wifi_hal_ctx.netif_ap_created = true;

  g_wifi_hal_ctx.netif_sta = esp_netif_create_default_wifi_sta();
  if ( !g_wifi_hal_ctx.netif_sta )
  {
    osal_log_error( "[wifi-hal] init: default STA netif failed" );
    goto unwind;
  }
  g_wifi_hal_ctx.netif_sta_created = true;

  {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    esp_err_t err  = esp_wifi_init( &cfg );
    if ( err != ESP_OK )
    {
      osal_log_error( "[wifi-hal] init: esp_wifi_init failed (0x%x)", (unsigned) err );
      goto unwind;
    }
    g_wifi_hal_ctx.wifi_inited = true;
  }

  if ( init->ap_ip && init->ap_gateway && init->ap_netmask )
  {
    esp_err_t dhcp_err = esp_netif_dhcps_stop( g_wifi_hal_ctx.netif_ap );
    if ( dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED )
    {
      goto unwind;
    }

    esp_netif_ip_info_t ap_ip_info = { 0 };
    inet_pton( AF_INET, init->ap_ip, &ap_ip_info.ip );
    inet_pton( AF_INET, init->ap_gateway, &ap_ip_info.gw );
    inet_pton( AF_INET, init->ap_netmask, &ap_ip_info.netmask );

    if ( esp_netif_set_ip_info( g_wifi_hal_ctx.netif_ap, &ap_ip_info ) != ESP_OK )
    {
      goto unwind;
    }

    if ( init->ap_dns && init->ap_dns[0] != '\0' )
    {
      if ( _configure_ap_dns( init->ap_dns ) != OSAL_SUCCESS )
      {
        goto unwind;
      }
    }

    if ( esp_netif_dhcps_start( g_wifi_hal_ctx.netif_ap ) != ESP_OK )
    {
      goto unwind;
    }
  }
  else if ( init->ap_dns && init->ap_dns[0] != '\0' )
  {
    /* Advertising a captive DNS requires the AP IP/DHCP configuration, so
     * reject the combination rather than silently dropping the request. */
    osal_log_error( "[wifi-hal] init: captive DNS requires AP IP/DHCP config" );
    goto unwind;
  }

  /* Register both handlers with the instance API and retain each token only
   * after ESP_OK.  Some shims/IDF implementations may write the output token
   * before returning an error; a local token prevents that unowned value from
   * being mistaken for a retained registration. */
  {
    esp_event_handler_instance_t handler_wifi = NULL;
    esp_err_t                    err = esp_event_handler_instance_register( WIFI_EVENT,
                                                                             ESP_EVENT_ANY_ID,
                                                                             _wifi_event_handler,
                                                                             NULL,
                                                                             &handler_wifi );
    if ( err != ESP_OK )
    {
      osal_log_error( "[wifi-hal] init: WIFI handler register failed (0x%x)", (unsigned) err );
      goto unwind;
    }
    g_wifi_hal_ctx.handler_wifi            = handler_wifi;
    g_wifi_hal_ctx.handler_wifi_registered = true;
  }
  {
    esp_event_handler_instance_t handler_ip = NULL;
    esp_err_t                    err = esp_event_handler_instance_register( IP_EVENT,
                                                                             IP_EVENT_STA_GOT_IP,
                                                                             _wifi_event_handler,
                                                                             NULL,
                                                                             &handler_ip );
    if ( err != ESP_OK )
    {
      osal_log_error( "[wifi-hal] init: IP handler register failed (0x%x)", (unsigned) err );
      goto unwind;
    }
    g_wifi_hal_ctx.handler_ip            = handler_ip;
    g_wifi_hal_ctx.handler_ip_registered = true;
  }

  /* Publish the session only after every owned resource is allocated. */
  g_lifecycle_state = HAL_STATE_ACTIVE;
  (void) osal_mutex_give( g_lifecycle_mutex );
  osal_log_info( "[wifi-hal] initialized" );
  return OSAL_SUCCESS;

unwind:
  /* Transactional init unwind: identical staging to deinit.  The lifecycle is
   * moved to DEINITIALIZING (so concurrent init/deinit callers wait) and the
   * teardown runs without holding the lifecycle serializer.  esp_wifi_stop() is
   * only ever done by _teardown_session() when this session actually started
   * Wi-Fi (init never starts it), and esp_wifi_deinit() only when
   * esp_wifi_init() succeeded (wifi_inited).  The final lifecycle state is
   * published only after all owned resources are released. */
  g_lifecycle_state = HAL_STATE_DEINITIALIZING;
  (void) _cond_broadcast();
  (void) osal_mutex_give( g_lifecycle_mutex );

  rc = _teardown_session();

  (void) osal_mutex_take( g_lifecycle_mutex );
  if ( rc == TEARDOWN_DONE )
  {
    memset( &g_wifi_hal_ctx, 0, sizeof( g_wifi_hal_ctx ) );
    g_lifecycle_state = HAL_STATE_UNINITIALIZED;
  }
  else
  {
    g_lifecycle_state = HAL_STATE_CLEANUP_REQUIRED;
    osal_log_error( "[wifi-hal] init unwind incomplete, retry via deinit" );
  }
  (void) _cond_broadcast();
  (void) osal_mutex_give( g_lifecycle_mutex );
  return OSAL_ERROR;
}

osal_status_t wifi_hal_deinit( void )
{
  /* A deinit issued from inside an in-flight user callback can never observe
   * itself draining itself: reject it before it can wait on (or become the
   * owner of) the teardown that is awaiting this callback's return. */
  if ( _lifecycle_call_from_callback() )
  {
    osal_log_error( "[wifi-hal] deinit rejected from event callback" );
    return OSAL_ERROR;
  }
  if ( !_ensure_lifecycle() )
  {
    return OSAL_ERROR;
  }

  (void) osal_mutex_take( g_lifecycle_mutex );

  for ( ;; )
  {
    if ( g_lifecycle_state == HAL_STATE_UNINITIALIZED )
    {
      (void) osal_mutex_give( g_lifecycle_mutex );
      osal_log_info( "[wifi-hal] deinit: not initialized (no-op)" );
      return OSAL_SUCCESS;
    }
    if ( g_lifecycle_state == HAL_STATE_DEINITIALIZING )
    {
      /* Another deinitializer owns the teardown: wait and re-check. */
      if ( !_cond_wait() )
      {
        (void) osal_mutex_give( g_lifecycle_mutex );
        return OSAL_ERROR;
      }
      continue;
    }
    /* ACTIVE or CLEANUP_REQUIRED: this caller owns the transition.  Close
     * admission immediately so no new operation can touch the session. */
    g_lifecycle_state = HAL_STATE_DEINITIALIZING;
    (void) _cond_broadcast();
    break;
  }

  /* Wait for every admitted public operation to finish.  The lifecycle mutex
   * is released while waiting (cond_wait), so an operation arriving now just
   * fails fast in _admit_operation() and an init caller waits likewise. */
  while ( g_admitted_ops > 0 )
  {
    if ( !_cond_wait() )
    {
      /* Keep DEINITIALIZING and callback delivery disabled.  No operation may
       * be started until a lifecycle owner can complete this transition. */
      (void) osal_mutex_give( g_lifecycle_mutex );
      return OSAL_ERROR;
    }
  }

  (void) osal_mutex_give( g_lifecycle_mutex );

  /* The teardown itself runs without holding the lifecycle serializer; no
   * operation is admitted and delivery is disabled at stage (1) of it. */
  teardown_result_t rc = _teardown_session();

  (void) osal_mutex_take( g_lifecycle_mutex );
  if ( rc == TEARDOWN_DONE )
  {
    memset( &g_wifi_hal_ctx, 0, sizeof( g_wifi_hal_ctx ) );
    g_lifecycle_state = HAL_STATE_UNINITIALIZED;
    osal_log_info( "[wifi-hal] deinitialized" );
  }
  else
  {
    g_lifecycle_state = HAL_STATE_CLEANUP_REQUIRED;
    osal_log_error( "[wifi-hal] deinit failed; call deinit again to finish cleanup" );
  }
  (void) _cond_broadcast();
  (void) osal_mutex_give( g_lifecycle_mutex );

  return ( rc == TEARDOWN_DONE ) ? OSAL_SUCCESS : OSAL_ERROR;
}

/* --------------------------------------------------------------------------- */
/* Public operations (guarded so teardown states never start new work)           */
/* --------------------------------------------------------------------------- */

osal_status_t wifi_hal_start( wifi_hal_mode_t mode )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_status_t result = OSAL_SUCCESS;
  wifi_mode_t   esp_mode = WIFI_MODE_STA;

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
    result = OSAL_ERROR;
    goto done;
  }

  if ( mode == WIFI_HAL_MODE_AP || mode == WIFI_HAL_MODE_APSTA )
  {
    wifi_config_t ap_cfg;
    _copy_ap_config( &ap_cfg, &g_wifi_hal_ctx.ap_cfg );
    if ( esp_wifi_set_config( WIFI_IF_AP, &ap_cfg ) != ESP_OK )
    {
      result = OSAL_ERROR;
      goto done;
    }
  }

  if ( mode == WIFI_HAL_MODE_STA || mode == WIFI_HAL_MODE_APSTA )
  {
    wifi_config_t sta_cfg;
    _copy_sta_config( &sta_cfg, &g_wifi_hal_ctx.sta_cfg );
    if ( esp_wifi_set_config( WIFI_IF_STA, &sta_cfg ) != ESP_OK )
    {
      result = OSAL_ERROR;
      goto done;
    }
  }

  if ( esp_wifi_start() != ESP_OK )
  {
    result = OSAL_ERROR;
    goto done;
  }

  g_wifi_hal_ctx.started = true;

done:
  _release_operation();
  return result;
}

osal_status_t wifi_hal_stop( void )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_status_t result = OSAL_SUCCESS;

  if ( g_wifi_hal_ctx.started )
  {
    if ( esp_wifi_stop() == ESP_OK )
    {
      g_wifi_hal_ctx.started = false;
    }
    else
    {
      result = OSAL_ERROR;
    }
  }

  _release_operation();
  return result;
}

osal_status_t wifi_hal_set_sta_config( const wifi_hal_sta_config_t* config )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_status_t result = OSAL_SUCCESS;
  if ( !config )
  {
    result = OSAL_INVALID_POINTER;
  }
  else
  {
    memset( &g_wifi_hal_ctx.sta_cfg, 0, sizeof( g_wifi_hal_ctx.sta_cfg ) );
    strncpy( g_wifi_hal_ctx.sta_cfg.ssid, config->ssid, WIFI_HAL_SSID_MAX_LEN );
    strncpy( g_wifi_hal_ctx.sta_cfg.password, config->password, WIFI_HAL_PASSWORD_MAX_LEN );
  }

  _release_operation();
  return result;
}

osal_status_t wifi_hal_set_ap_config( const wifi_hal_ap_config_t* config )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_status_t result = OSAL_SUCCESS;
  if ( !config )
  {
    result = OSAL_INVALID_POINTER;
  }
  else
  {
    memset( &g_wifi_hal_ctx.ap_cfg, 0, sizeof( g_wifi_hal_ctx.ap_cfg ) );
    strncpy( g_wifi_hal_ctx.ap_cfg.ssid, config->ssid, WIFI_HAL_SSID_MAX_LEN );
    strncpy( g_wifi_hal_ctx.ap_cfg.password, config->password, WIFI_HAL_PASSWORD_MAX_LEN );
    g_wifi_hal_ctx.ap_cfg.max_connection = config->max_connection;
    g_wifi_hal_ctx.ap_cfg.authmode       = config->authmode;
  }

  _release_operation();
  return result;
}

osal_status_t wifi_hal_connect( void )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  size_t pass_len = strlen( g_wifi_hal_ctx.sta_cfg.password );
  osal_log_debug( "[wifi-hal] connect requested: ssid='%s', pass_len=%u",
                  g_wifi_hal_ctx.sta_cfg.ssid,
                  (unsigned) pass_len );

  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_err_t err = esp_wifi_get_mode( &mode );
  if ( err != ESP_OK )
  {
    osal_log_error( "[wifi-hal] esp_wifi_get_mode failed: %s (0x%x)",
                    esp_err_to_name( err ), (unsigned) err );
    _release_operation();
    return OSAL_ERROR;
  }

  if ( mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA )
  {
    osal_log_error( "[wifi-hal] invalid mode for connect: %d", (int) mode );
    _release_operation();
    return OSAL_ERROR;
  }

  wifi_config_t sta_cfg;
  _copy_sta_config( &sta_cfg, &g_wifi_hal_ctx.sta_cfg );
  err = esp_wifi_set_config( WIFI_IF_STA, &sta_cfg );
  if ( err != ESP_OK )
  {
    osal_log_error( "[wifi-hal] esp_wifi_set_config failed: %s (0x%x)",
                    esp_err_to_name( err ), (unsigned) err );
    _release_operation();
    return OSAL_ERROR;
  }

  err = esp_wifi_connect();
  if ( err != ESP_OK )
  {
    osal_log_error( "[wifi-hal] esp_wifi_connect failed: %s (0x%x)",
                    esp_err_to_name( err ), (unsigned) err );
    _release_operation();
    return OSAL_ERROR;
  }

  osal_log_debug( "[wifi-hal] esp_wifi_connect accepted" );
  _release_operation();
  return OSAL_SUCCESS;
}

osal_status_t wifi_hal_disconnect( void )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }
  osal_status_t result = _esp_to_status( esp_wifi_disconnect() );
  _release_operation();
  return result;
}

osal_status_t wifi_hal_start_scan( bool block )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }
  wifi_scan_config_t config = { 0 };
  osal_status_t result = _esp_to_status( esp_wifi_scan_start( &config, block ) );
  _release_operation();
  return result;
}

osal_status_t wifi_hal_get_scanned_ap( wifi_hal_ap_record_t* records, uint16_t* in_out_count )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_status_t result = OSAL_SUCCESS;
  if ( !records || !in_out_count )
  {
    result = OSAL_INVALID_POINTER;
  }
  else
  {
    uint16_t count = *in_out_count;
    if ( count > 64u )
    {
      count = 64u;
    }

    esp_err_t err = esp_wifi_scan_get_ap_records( &count, scan_ap_records );
    if ( err != ESP_OK )
    {
      result = OSAL_ERROR;
    }
    else
    {
      for ( uint16_t i = 0; i < count; ++i )
      {
        memset( &records[i], 0, sizeof( records[i] ) );
        strncpy( records[i].ssid, (const char*) scan_ap_records[i].ssid, WIFI_HAL_SSID_MAX_LEN );
        records[i].channel = scan_ap_records[i].primary;
        records[i].rssi    = scan_ap_records[i].rssi;
        records[i].authmode = scan_ap_records[i].authmode;
      }
      *in_out_count = count;
    }
  }

  _release_operation();
  return result;
}

osal_status_t wifi_hal_get_sta_ip_info( wifi_hal_ip_info_t* out_info )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_status_t result = OSAL_SUCCESS;
  if ( !out_info )
  {
    result = OSAL_INVALID_POINTER;
  }
  else
  {
    esp_netif_ip_info_t ip_info = { 0 };
    if ( esp_netif_get_ip_info( g_wifi_hal_ctx.netif_sta, &ip_info ) == ESP_OK )
    {
      esp_ip4addr_ntoa( &ip_info.ip, out_info->ip, sizeof( out_info->ip ) );
      esp_ip4addr_ntoa( &ip_info.netmask, out_info->netmask, sizeof( out_info->netmask ) );
      esp_ip4addr_ntoa( &ip_info.gw, out_info->gw, sizeof( out_info->gw ) );
    }
    else
    {
      result = OSAL_ERROR;
    }
  }

  _release_operation();
  return result;
}

osal_status_t wifi_hal_get_sta_rssi( int* out_rssi )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_status_t result = OSAL_SUCCESS;
  if ( !out_rssi )
  {
    result = OSAL_INVALID_POINTER;
  }
  else
  {
    wifi_ap_record_t ap_info = { 0 };
    if ( esp_wifi_sta_get_ap_info( &ap_info ) == ESP_OK )
    {
      *out_rssi = ap_info.rssi;
    }
    else
    {
      result = OSAL_ERROR;
    }
  }

  _release_operation();
  return result;
}

osal_status_t wifi_hal_set_power_save( bool enabled )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }
  osal_status_t result = _esp_to_status( esp_wifi_set_ps( enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE ) );
  _release_operation();
  return result;
}

osal_status_t wifi_hal_get_default_mac( uint8_t mac[6] )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }
  osal_status_t result = OSAL_SUCCESS;
  if ( !mac )
  {
    result = OSAL_INVALID_POINTER;
  }
  else
  {
    result = _esp_to_status( esp_efuse_mac_get_default( mac ) );
  }
  _release_operation();
  return result;
}

osal_status_t wifi_hal_get_client_count( uint32_t* out_client_count )
{
  if ( !_admit_operation() )
  {
    return OSAL_ERROR;
  }

  osal_status_t result = OSAL_SUCCESS;
  if ( !out_client_count )
  {
    result = OSAL_INVALID_POINTER;
  }
  else
  {
    *out_client_count = g_wifi_hal_ctx.client_count;
  }

  _release_operation();
  return result;
}