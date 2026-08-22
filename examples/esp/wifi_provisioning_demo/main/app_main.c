/*
 * Wi-Fi provisioning example (ESP32, ESP-IDF)
 *
 * Exercises the complete provisioning flow on real hardware:
 *
 *   - a captive portal served on HTTP port 80,
 *   - a captive DNS responder on UDP port 53,
 *   - automatic fallback: when no saved Wi-Fi credentials exist the
 *     provisioning application is started by the controller at boot, and it
 *     reopens when the saved credentials are exhausted,
 *   - after a successful connection submitted through the portal the
 *     controller keeps HTTP/DNS available for a success grace period and then
 *     transitions to STA-only mode, retiring the temporary access point.
 *
 * The runtime is brought up in the documented ownership order and every
 * module depends only on the layers below it:
 *
 *   init:     OSAL -> storage -> Mongoose -> Wi-Fi management -> provisioning
 *   shutdown: provisioning -> Wi-Fi management -> Mongoose -> storage -> OS
 *
 * Storage (the littlefs "storage" partition, see partitions.csv) is mounted
 * before Wi-Fi management so saved credentials can be loaded at startup and
 * written back after a successful connection (wifi_ap.json).
 *
 * Credential persistence: submitted credentials are stored in wifi_ap.json on
 * the littlefs storage partition. To erase-and-reprovision a device, erase
 * the storage partition (or the whole flash) and reflash; see README.md.
 */

#include <stdio.h>
#include <string.h>

#include "hq_config.h"
#include "mongoose_process.h"
#include "osal_mount.h"
#include "osal_task.h"
#include "wifi_http_provisioning.h"
#include "wifi_managment.h"
#include "wifi_provisioning_controller.h"

/* Storage partition (partitions.csv) that backs credential persistence. */
#define DEMO_STORAGE_DEVICE "storage"
#define DEMO_STORAGE_MOUNT  "/littlefs"

/* How long to wait for the Wi-Fi worker task to come up after start(). */
#define DEMO_WIFI_START_TIMEOUT_MS 5000u

/* Heartbeat period for the status banner. */
#define DEMO_HEARTBEAT_MS 10000u

/* Map the provisioning lifecycle state to a short label. */
static const char *demo_prov_state_name( wifi_http_provisioning_state_t st )
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

static const char *demo_controller_state_name( wifi_provisioning_controller_state_t st )
{
  switch ( st )
  {
    case WIFI_PROVISIONING_CONTROLLER_DISABLED:         return "disabled";
    case WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT: return "awaiting_connect";
    case WIFI_PROVISIONING_CONTROLLER_ONLINE:           return "online";
    case WIFI_PROVISIONING_CONTROLLER_PROVISIONING:     return "provisioning";
    case WIFI_PROVISIONING_CONTROLLER_GRACE:            return "grace";
    default:                                            return "unknown";
  }
}

/* ------------------------------------------------------------------ */
/*  Runtime lifecycle in documented ownership order                    */
/* ------------------------------------------------------------------ */

/* Init step 1: OSAL (tasks, semaphores, queues, timers). On ESP the OSAL
 * wraps FreeRTOS and is self-initializing; touching its time base makes the
 * ownership order explicit. */
static int demo_init_osal( void )
{
  printf( "[demo] init 1/5: OSAL (foundation)...\n" );
  ( void ) osal_task_get_time_ms();
  printf( "[demo] init 1/5: OSAL ready (tick %u ms)\n",
          ( unsigned ) osal_task_get_time_ms() );
  return 0;
}

/* Init step 2: persistent storage (littlefs "storage" partition). The Wi-Fi
 * management layer reads saved credentials during init, so the file system
 * must be mounted before it starts. */
static int demo_init_storage( void )
{
  printf( "[demo] init 2/5: storage (%s on %s)...\n",
          DEMO_STORAGE_DEVICE, DEMO_STORAGE_MOUNT );
  if ( osal_mount( DEMO_STORAGE_DEVICE, DEMO_STORAGE_MOUNT ) != OSAL_SUCCESS )
  {
    printf( "[demo] ERROR: cannot mount %s partition (see partitions.csv)\n",
            DEMO_STORAGE_DEVICE );
    return -1;
  }
  printf( "[demo] init 2/5: storage ready\n" );
  return 0;
}

/* Init step 3: shared Mongoose process (hosts the portal HTTP listener on
 * port 80 and the captive DNS responder on port 53). */
static int demo_init_mongoose( void )
{
  printf( "[demo] init 3/5: Mongoose process...\n" );
  MongooseProcess_Init();
  if ( !MongooseProcess_IsRunning() )
  {
    printf( "[demo] ERROR: Mongoose process failed to start\n" );
    return -1;
  }
  printf( "[demo] init 3/5: Mongoose process running\n" );
  return 0;
}

/* Init step 4: Wi-Fi management. AP+STA is requested up front so the station
 * and the temporary access point both exist before provisioning opens. */
static int demo_init_wifi( void )
{
  uint32_t waited = 0u;

  printf( "[demo] init 4/5: Wi-Fi management (AP+STA)...\n" );
  wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLI_SER );
  wifi_mgmt_init();
  wifi_mgmt_start();

  while ( !wifi_mgmt_is_running() && waited < DEMO_WIFI_START_TIMEOUT_MS )
  {
    osal_task_delay_ms( 10u );
    waited += 10u;
  }
  if ( !wifi_mgmt_is_running() )
  {
    printf( "[demo] ERROR: Wi-Fi management did not start\n" );
    return -1;
  }
  printf( "[demo] init 4/5: Wi-Fi management running (AP+STA)\n" );
  return 0;
}

/* Init step 5: provisioning application with automatic fallback. The listen
 * URLs are taken from the example defconfig (defconfig/esp_provisioning.defconfig):
 *   http://0.0.0.0:80  and  udp://0.0.0.0:53
 * The controller starts the portal immediately when no saved credential
 * exists (fresh device) and reopens it if the saved credentials fail, so the
 * device can always be (re)provisioned without serial access. */
static int demo_init_provisioning( void )
{
  printf( "[demo] init 5/5: provisioning app (HTTP %s, DNS %s)...\n",
          CONFIG_WIFI_HTTP_PROVISIONING_HTTP_URL,
          CONFIG_WIFI_HTTP_PROVISIONING_DNS_URL );

  if ( !wifi_http_provisioning_set_http_url( CONFIG_WIFI_HTTP_PROVISIONING_HTTP_URL ) )
  {
    printf( "[demo] ERROR: cannot configure HTTP listen URL\n" );
    return -1;
  }
  if ( !wifi_http_provisioning_set_dns_url( CONFIG_WIFI_HTTP_PROVISIONING_DNS_URL ) )
  {
    printf( "[demo] ERROR: cannot configure DNS listen URL\n" );
    return -1;
  }

  /* Automatic fallback: starts provisioning now when wifi_ap.json does not
   * exist yet and reopens it when saved credentials are exhausted. */
  if ( !wifi_provisioning_controller_init() )
  {
    printf( "[demo] ERROR: provisioning fallback controller failed\n" );
    return -1;
  }

  printf( "[demo] init 5/5: provisioning controller ready (state=%s)\n",
          demo_controller_state_name( wifi_provisioning_controller_get_state() ) );
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Runtime: status banner                                             */
/* ------------------------------------------------------------------ */

static void demo_print_status( void )
{
  printf( "[demo] status: controller=%s provisioning=%s wifi_running=%s wifi_connected=%s\n",
          demo_controller_state_name( wifi_provisioning_controller_get_state() ),
          demo_prov_state_name( wifi_http_provisioning_get_state() ),
          wifi_mgmt_is_running() ? "yes" : "no",
          wifi_mgmt_is_connected() ? "yes" : "no" );
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

void app_main( void )
{
  printf( "\n" );
  printf( "===========================================================\n" );
  printf( "     Wi-Fi Provisioning Demo (ESP32)\n" );
  printf( "===========================================================\n" );
  printf( "Portal:   http://%s/\n", CONFIG_WIFI_HTTP_PROVISIONING_HTTP_URL );
  printf( "DNS:      %s\n", CONFIG_WIFI_HTTP_PROVISIONING_DNS_URL );
  printf( "Ownership order: OS -> storage -> Mongoose -> Wi-Fi -> provisioning\n" );
  printf( "Automatic fallback: enabled (no saved credential => portal opens)\n" );
  printf( "\n" );

  if ( demo_init_osal() != 0 )          goto fail;
  if ( demo_init_storage() != 0 )       goto fail;
  if ( demo_init_mongoose() != 0 )      goto fail;
  if ( demo_init_wifi() != 0 )          goto fail;
  if ( demo_init_provisioning() != 0 )  goto fail;

  demo_print_status();
  printf( "\n[demo] connect a phone/laptop to the %s access point and open "
          "http://10.10.0.1 in a browser\n", WIFI_AP_NAME );
  printf( "[demo] submitting credentials through the portal stores them in "
          "wifi_ap.json on the storage partition\n" );
  printf( "\n" );

  for ( ;; )
  {
    osal_task_delay_ms( DEMO_HEARTBEAT_MS );
    demo_print_status();
  }

fail:
  printf( "[demo] FATAL: initialization failed in ownership order; "
          "see messages above\n" );
}