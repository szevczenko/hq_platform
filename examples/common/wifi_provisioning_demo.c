/*
 * Wi-Fi provisioning demo (POSIX, simulated Wi-Fi)
 *
 * Runs the Wi-Fi HTTP provisioning application end-to-end against the POSIX
 * Wi-Fi simulator. The demo brings the layered runtime up in strict ownership
 * order and tears it down in the reverse order:
 *
 *   init:     OSAL -> Mongoose -> Wi-Fi management -> provisioning
 *   shutdown: provisioning -> Wi-Fi management -> Mongoose -> OSAL
 *
 * Listeners owned by the demo:
 *   HTTP   http://127.0.0.1:8080    provisioning portal + REST API
 *   DNS    udp://127.0.0.1:10053    captive DNS responder
 *
 * The HTTP and DNS listeners ride the shared Mongoose process, exactly like
 * MQTT and ThingsBoard would in a real product, so the demo also exercises
 * the "stop provisioning must not tear down the shared process" guarantee.
 *
 * See wifi_provisioning_demo.README.md for the complete user guide and the
 * table of simulated networks provided by the POSIX Wi-Fi simulator.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mongoose_process.h"
#include "osal_task.h"
#include "wifi_http_provisioning.h"
#include "wifi_managment.h"

#ifdef ESP_PLATFORM
#error "wifi_provisioning_demo.c targets POSIX (simulated Wi-Fi) only"
#endif

/* Listen addresses required by TASK-130. */
#define DEMO_HTTP_URL "http://127.0.0.1:8080"
#define DEMO_DNS_URL  "udp://127.0.0.1:10053"

/* Same ports, numeric, used only for the post-shutdown release check. */
#define DEMO_HTTP_PORT 8080
#define DEMO_DNS_PORT  10053

/* How long to wait for the Wi-Fi worker task to come up after start(). */
#define DEMO_WIFI_START_TIMEOUT_MS 3000u

/* Raised by SIGINT (Ctrl-C) or SIGTERM (scripted runner); the main loop then
 * exits and the demo shuts the runtime down in reverse ownership order. */
static volatile sig_atomic_t s_shutdown_requested = 0;

static void demo_on_signal( int sig )
{
  ( void ) sig;
  s_shutdown_requested = 1;
}

/* Map a provisioning lifecycle state to a short label. */
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

/* ------------------------------------------------------------------ */
/*  Runtime lifecycle in ownership order                              */
/* ------------------------------------------------------------------ */

/* Init step 1: OSAL. The OSAL (tasks, semaphores, queues, timers) is the
 * foundation every other module builds on. On POSIX it is self-initializing;
 * touching its time base here makes the ownership order explicit. */
static int demo_init_osal( void )
{
  printf( "[demo] init 1/4: OSAL (foundation)...\n" );
  ( void ) osal_task_get_time_ms();
  printf( "[demo] init 1/4: OSAL ready (tick %u ms)\n",
          ( unsigned ) osal_task_get_time_ms() );
  return 0;
}

/* Init step 2: shared Mongoose process (hosts HTTP and DNS listeners). */
static int demo_init_mongoose( void )
{
  printf( "[demo] init 2/4: Mongoose process...\n" );
  MongooseProcess_Init();
  if ( !MongooseProcess_IsRunning() )
  {
    printf( "[demo] ERROR: Mongoose process failed to start\n" );
    return -1;
  }
  printf( "[demo] init 2/4: Mongoose process running\n" );
  return 0;
}

/* Step 3: Wi-Fi management on the simulated HAL. AP+STA is requested up front
 * so both the station (STA) and the temporary access point (AP) exist before
 * the provisioning listeners open. */
static int demo_init_wifi( void )
{
  uint32_t waited = 0;

  printf( "[demo] init 3/4: Wi-Fi management (AP+STA, simulated HAL)...\n" );
  wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLI_SER );
  wifi_mgmt_init();
  wifi_mgmt_start();

  while ( !wifi_mgmt_is_running() && waited < DEMO_WIFI_START_TIMEOUT_MS )
  {
    osal_task_delay_ms( 10 );
    waited += 10;
  }
  if ( !wifi_mgmt_is_running() )
  {
    printf( "[demo] ERROR: Wi-Fi management did not start\n" );
    return -1;
  }
  printf( "[demo] init 3/4: Wi-Fi management running (AP+STA)\n" );
  return 0;
}

/* Step 4: provisioning application. Both listeners are configured onto the
 * loopback high ports required by this demo, then started. */
static int demo_init_provisioning( void )
{
  printf( "[demo] init 4/4: provisioning app (HTTP %s, DNS %s)...\n",
          DEMO_HTTP_URL, DEMO_DNS_URL );

  if ( !wifi_http_provisioning_set_http_url( DEMO_HTTP_URL ) )
  {
    printf( "[demo] ERROR: cannot configure HTTP listen URL\n" );
    return -1;
  }
  if ( !wifi_http_provisioning_set_dns_url( DEMO_DNS_URL ) )
  {
    printf( "[demo] ERROR: cannot configure DNS listen URL\n" );
    return -1;
  }
  if ( !wifi_http_provisioning_start() )
  {
    printf( "[demo] ERROR: provisioning app failed to start\n" );
    return -1;
  }
  if ( wifi_http_provisioning_get_state() != WIFI_PROVISIONING_RUNNING )
  {
    printf( "[demo] ERROR: provisioning app not RUNNING (state=%s)\n",
            demo_prov_state_name( wifi_http_provisioning_get_state() ) );
    return -1;
  }
  printf( "[demo] init 4/4: provisioning app RUNNING\n" );
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Shutdown in reverse ownership order                               */
/* ------------------------------------------------------------------ */

/* Step 1 of the reverse order: close the HTTP and DNS listeners owned by the
 * provisioning app. The shared Mongoose process and Wi-Fi management stay
 * untouched here. */
static int demo_shutdown_provisioning( void )
{
  printf( "[demo] shutdown 1/3: provisioning app...\n" );
  if ( !wifi_http_provisioning_stop() )
  {
    printf( "[demo] ERROR: provisioning app failed to stop cleanly\n" );
    return -1;
  }
  printf( "[demo] shutdown 1/3: provisioning stopped (state=%s)\n",
          demo_prov_state_name( wifi_http_provisioning_get_state() ) );
  return 0;
}

/* Step 2 of the reverse order: stop the Wi-Fi worker and release the
 * simulated HAL. */
static int demo_shutdown_wifi( void )
{
  printf( "[demo] shutdown 2/3: Wi-Fi management...\n" );
  wifi_mgmt_stop();
  printf( "[demo] shutdown 2/3: Wi-Fi management stopped\n" );
  return 0;
}

/* Step 3 of the reverse order: stop the shared Mongoose poll thread. OSAL, the
 * foundation layer, needs no explicit teardown on POSIX. */
static int demo_shutdown_mongoose( void )
{
  printf( "[demo] shutdown 3/3: Mongoose process...\n" );
  MongooseProcess_Deinit();
  printf( "[demo] shutdown 3/3: Mongoose process stopped\n" );
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Post-shutdown port release check                                  */
/* ------------------------------------------------------------------ */

/* Bind both demo ports on 127.0.0.1. A successful bind proves the shutdown
 * released the HTTP and the DNS listener. The TCP check sets SO_REUSEADDR so
 * short-lived TIME_WAIT connections accepted by the portal (the server closes
 * HTTP/1.1 responses first) do not mask a released listening socket. */
static bool demo_ports_released( void )
{
  struct sockaddr_in addr;
  int                one = 1;
  int                tcp_fd;
  int                udp_fd;
  bool               tcp_free = false;
  bool               udp_free = false;

  tcp_fd = ( int ) socket( AF_INET, SOCK_STREAM, 0 );
  if ( tcp_fd >= 0 )
  {
    ( void ) setsockopt( tcp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) );
    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    addr.sin_port        = htons( DEMO_HTTP_PORT );
    tcp_free = ( bind( tcp_fd, ( struct sockaddr * ) &addr,
                       sizeof( addr ) ) == 0 );
    close( tcp_fd );
  }

  udp_fd = ( int ) socket( AF_INET, SOCK_DGRAM, 0 );
  if ( udp_fd >= 0 )
  {
    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    addr.sin_port        = htons( DEMO_DNS_PORT );
    udp_free = ( bind( udp_fd, ( struct sockaddr * ) &addr, sizeof( addr ) ) == 0 );
    close( udp_fd );
  }

  return tcp_free && udp_free;
}

/* ------------------------------------------------------------------ */
/*  Runtime: instructions, console commands, status                   */
/* ------------------------------------------------------------------ */

static void demo_print_simulated_networks( void )
{
  printf( "Simulated networks (provided by the POSIX Wi-Fi simulator):\n" );
  printf( "  properly_ap       stable connection, password 12345678\n" );
  printf( "  disconnect_15_sec connects, then drops after  15 s\n" );
  printf( "  broken            always rejects connections\n" );
  printf( "  disconnect_1_min  connects, then drops after  60 s\n" );
  printf( "  slow_connect      connects after a 3 s delay\n" );
  printf( "  weak_signal       stable, very low RSSI (-90 dBm)\n" );
  printf( "  wrong_password    rejects unless password is exactly correct_pw\n" );
}

static void demo_print_help( void )
{
  printf( "Commands:\n" );
  printf( "  status - show provisioning and Wi-Fi state\n" );
  printf( "  help   - this help\n" );
  printf( "  exit   - shut down cleanly in reverse ownership order\n" );
}

static void demo_print_status( void )
{
  wifi_mgmt_ap_list_t aps;
  uint16_t            ap_count = 0;

  printf( "[demo] provisioning=%s wifi_running=%s wifi_connected=%s\n",
          demo_prov_state_name( wifi_http_provisioning_get_state() ),
          wifi_mgmt_is_running() ? "yes" : "no",
          wifi_mgmt_is_connected() ? "yes" : "no" );

  if ( wifi_mgmt_get_access_points( &aps ) )
  {
    ap_count = aps.count;
  }
  printf( "[demo] scanned_networks=%u\n", ( unsigned ) ap_count );
}

/* Main event loop. Waits for SIGINT/SIGTERM or an "exit" console command,
 * printing a periodic heartbeat and handling the small console command set.
 * stdin is consumed whenever it is open (terminal or pipe) and the demo keeps
 * serving once stdin reaches EOF, which makes it suitable for scripted
 * runners. */
static int demo_run( void )
{
  fd_set         rfds;
  struct timeval tv;
  char           line[128];
  bool           console_open = !feof( stdin );
  bool           interactive  = ( isatty( STDIN_FILENO ) != 0 );
  unsigned       heartbeat    = 0;

  while ( !s_shutdown_requested )
  {
    FD_ZERO( &rfds );
    tv.tv_sec  = 1;
    tv.tv_usec = 0;

    if ( console_open )
    {
      FD_SET( STDIN_FILENO, &rfds );
      int rc = select( STDIN_FILENO + 1, &rfds, NULL, NULL, &tv );
      if ( rc > 0 && FD_ISSET( STDIN_FILENO, &rfds ) )
      {
        if ( fgets( line, sizeof( line ), stdin ) == NULL )
        {
          console_open = false; /* stdin at EOF; keep serving */
        }
        else
        {
          line[strcspn( line, "\r\n" )] = '\0';
          if ( strcmp( line, "exit" ) == 0 )
          {
            break;
          }
          else if ( strcmp( line, "status" ) == 0 )
          {
            demo_print_status();
          }
          else if ( strcmp( line, "help" ) == 0 )
          {
            demo_print_help();
          }
          else if ( line[0] != '\0' && interactive )
          {
            printf( "unknown command: %s (type help)\n", line );
          }
        }
      }
    }
    else
    {
      /* No console: just pace the heartbeat. */
      osal_task_delay_ms( 1000 );
    }

    if ( ++heartbeat % 5 == 0 )
    {
      demo_print_status();
    }
  }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */

int main( void )
{
  int rc = 0;

  setvbuf( stdout, NULL, _IONBF, 0 );
  signal( SIGINT, demo_on_signal );
  signal( SIGTERM, demo_on_signal );

  printf( "\n" );
  printf( "===========================================================\n" );
  printf( "     Wi-Fi Provisioning Demo (POSIX, simulated Wi-Fi)\n" );
  printf( "===========================================================\n" );

  if ( demo_init_osal() != 0 )       return 1;
  if ( demo_init_mongoose() != 0 )   return 1;
  if ( demo_init_wifi() != 0 )       return 1;
  if ( demo_init_provisioning() != 0 )
  {
    demo_shutdown_wifi();
    demo_shutdown_mongoose();
    return 1;
  }

  printf( "\nPortal:   http://127.0.0.1:%u/\n", DEMO_HTTP_PORT );
  printf( "DNS:      udp://127.0.0.1:%u  (try: dig @127.0.0.1 -p %u provision.local)\n",
          DEMO_DNS_PORT, DEMO_DNS_PORT );
  printf( "Shutdown: press Ctrl-C, or type 'exit' and press Enter\n\n" );
  demo_print_simulated_networks();
  printf( "\n" );
  demo_print_status();
  demo_print_help();
  printf( "\n" );

  rc = demo_run();

  printf( "\n[demo] shutting down in reverse ownership order...\n" );
  if ( demo_shutdown_provisioning() != 0 ) rc = 1;
  if ( demo_shutdown_wifi() != 0 )         rc = 1;
  if ( demo_shutdown_mongoose() != 0 )     rc = 1;

  if ( demo_ports_released() )
  {
    printf( "[demo] shutdown complete: HTTP :%u and DNS :%u were released\n",
            DEMO_HTTP_PORT, DEMO_DNS_PORT );
  }
  else
  {
    printf( "[demo] WARNING: listener ports were not released\n" );
    rc = 1;
  }

  return rc;
}