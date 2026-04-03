/**
 *******************************************************************************
 * @file    hq_cmd_wifi.c
 * @author  Dmytro Shevchenko
 * @brief   CLI commands for Wi-Fi management
 *
 *          Usage:
 *            wifi --scan                        Scan for access points
 *            wifi --connect <ssid> [--password <pass>]  Connect to AP
 *            wifi --disconnect                  Disconnect from current AP
 *            wifi --info                        Show IP / connection info
 *            wifi --status                      Show connection status
 *            wifi --rssi                        Show current RSSI
 *            wifi --clients                     Show soft-AP client count
 *            wifi --saved                       Show saved credentials
 *            wifi --type <client|server|apsta>  Set operating mode
 *            wifi --start                       Start the Wi-Fi state machine
 *            wifi --stop                        Stop the Wi-Fi state machine
 *******************************************************************************
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hq_cmd.h"
#include "wifi_config.h"
#include "wifi_managment.h"
#include "wifi_utils.h"

/* -------------------------------------------------------------------------- */
/*  Init tracking                                                             */
/* -------------------------------------------------------------------------- */

static bool g_wifi_initialized = false;

/* -------------------------------------------------------------------------- */
/*  Helpers                                                                   */
/* -------------------------------------------------------------------------- */

/** @brief Scratch buffer shared by all sub-commands for snprintf output. */
static char g_out[512];

static void _print_line( const char* fmt, ... )
{
  va_list ap;
  va_start( ap, fmt );
  (void) vsnprintf( g_out, sizeof( g_out ), fmt, ap );
  va_end( ap );
  hq_cmd_print( g_out );
}

static const char* _find_option_value( const char* args, const char* option )
{
  uint16_t count = hq_cmd_get_token_count( args );
  for ( uint16_t i = 1; i <= count; ++i )
  {
    const char* tok = hq_cmd_get_token( args, i );
    if ( tok && strcmp( tok, option ) == 0 )
    {
      /* Return the next token as the value, if present. */
      if ( i < count )
      {
        return hq_cmd_get_token( args, (uint16_t)( i + 1 ) );
      }
      return NULL;
    }
  }
  return NULL;
}

static bool _has_option( const char* args, const char* option )
{
  return hq_cmd_find_token( args, option ) != 0;
}

/* -------------------------------------------------------------------------- */
/*  Sub-commands                                                              */
/* -------------------------------------------------------------------------- */

static void _cmd_scan( void )
{
  hq_cmd_print( "Scanning..." );

  if ( !wifi_mgmt_start_scan() )
  {
    hq_cmd_print( "Scan failed (not in a scannable state)." );
    return;
  }

  static wifi_mgmt_ap_list_t list;
  if ( !wifi_mgmt_get_access_points( &list ) || list.count == 0 )
  {
    hq_cmd_print( "No access points found." );
    return;
  }

  _print_line( "Found %u access points:", (unsigned) list.count );

  for ( uint16_t i = 0; i < list.count; ++i )
  {
    _print_line( "  %2u. %-32s  ch:%2d  rssi:%d  auth:%d",
                 (unsigned)( i + 1 ),
                 list.items[i].ssid,
                 list.items[i].chan,
                 list.items[i].rssi,
                 list.items[i].auth );
  }
}

static void _cmd_connect( const char* args )
{
  const char* ssid = _find_option_value( args, "--connect" );
  if ( !ssid || strlen( ssid ) == 0 )
  {
    hq_cmd_print( "Usage: wifi --connect <ssid> [--password <pass>]" );
    return;
  }

  const char* pass = _find_option_value( args, "--password" );

  if ( !wifi_mgmt_set_ap_name( ssid, strlen( ssid ) ) )
  {
    hq_cmd_print( "Invalid SSID." );
    return;
  }

  if ( pass && strlen( pass ) > 0 )
  {
    if ( !wifi_mgmt_set_password( pass, strlen( pass ) ) )
    {
      hq_cmd_print( "Invalid password." );
      return;
    }
  }
  else
  {
    /* Open network — set empty password. */
    (void) wifi_mgmt_set_password( "", 0 );
  }

  if ( !wifi_mgmt_connect() )
  {
    hq_cmd_print( "Connect request rejected (server mode?)." );
    return;
  }

  _print_line( "Connecting to \"%s\"...", ssid );
}

static void _cmd_disconnect( void )
{
  if ( !wifi_mgmt_disconnect() )
  {
    hq_cmd_print( "Disconnect request rejected." );
    return;
  }

  hq_cmd_print( "Disconnecting..." );
}

static void _cmd_info( void )
{
  wifi_mgmt_ip_info_t info;
  if ( !wifi_mgmt_get_ip_info( &info ) )
  {
    hq_cmd_print( "Could not retrieve IP info." );
    return;
  }

  const char* reason_str;
  switch ( info.urc )
  {
    case 0:  reason_str = "connected";       break;
    case 1:  reason_str = "failed attempt";  break;
    case 2:  reason_str = "user disconnect"; break;
    case 3:  reason_str = "lost connection"; break;
    default: reason_str = "unknown";         break;
  }

  _print_line( "SSID:    %s", info.ssid );
  _print_line( "IP:      %s", info.ip );
  _print_line( "Netmask: %s", info.netmask );
  _print_line( "Gateway: %s", info.gw );
  _print_line( "Status:  %s (urc=%d)", reason_str, info.urc );
}

static void _cmd_status( void )
{
  bool connected = wifi_mgmt_is_connected();
  bool idle      = wifi_mgmt_is_idle();
  bool trying    = wifi_mgmt_trying_connect();
  bool scan_ok   = wifi_mgmt_is_ready_to_scan();
  bool has_data  = wifi_mgmt_is_read_data();

  _print_line( "Connected:      %s", connected ? "yes" : "no" );
  _print_line( "Idle:           %s", idle ? "yes" : "no" );
  _print_line( "Connecting:     %s", trying ? "yes" : "no" );
  _print_line( "Ready to scan:  %s", scan_ok ? "yes" : "no" );
  _print_line( "Config loaded:  %s", has_data ? "yes" : "no" );

  if ( connected )
  {
    char ip[16] = { 0 };
    wifi_mgmt_get_ip_addr( ip, sizeof( ip ) );
    _print_line( "IP address:     %s", ip );
    _print_line( "RSSI:           %d dBm", wifi_mgmt_get_rssi() );
  }
}

static void _cmd_rssi( void )
{
  if ( !wifi_mgmt_is_connected() )
  {
    hq_cmd_print( "Not connected." );
    return;
  }

  _print_line( "RSSI: %d dBm", wifi_mgmt_get_rssi() );
}

static void _cmd_clients( void )
{
  uint32_t cnt = wifi_mgmt_get_client_count();
  _print_line( "AP clients: %u", (unsigned) cnt );
}

static void _cmd_saved( void )
{
  wifi_config_list_t list;
  if ( wifi_config_load( &list ) != OSAL_SUCCESS )
  {
    hq_cmd_print( "No saved credentials (file not found or error)." );
    return;
  }

  if ( list.count == 0 )
  {
    hq_cmd_print( "No saved credentials." );
    return;
  }

  _print_line( "Saved credentials (%u / %u), last_use=nb:%u:",
               (unsigned) list.count,
               (unsigned) WIFI_CONFIG_MAX_CREDENTIALS,
               (unsigned) list.last_use );

  for ( uint8_t i = 0; i < list.count; ++i )
  {
    const char* marker = ( list.entries[i].nb == list.last_use ) ? " <-- active" : "";
    _print_line( "  [nb:%u] %-32s  pw:%s%s",
                 (unsigned) list.entries[i].nb,
                 list.entries[i].ssid,
                 list.entries[i].password[0] ? "****" : "(open)",
                 marker );
  }
}

static void _cmd_type( const char* args )
{
  const char* mode = _find_option_value( args, "--type" );
  if ( !mode )
  {
    hq_cmd_print( "Usage: wifi --type <client|server|apsta>" );
    return;
  }

  if ( strcmp( mode, "client" ) == 0 )
  {
    wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLIENT );
    hq_cmd_print( "Wi-Fi type set to CLIENT." );
  }
  else if ( strcmp( mode, "server" ) == 0 )
  {
    wifi_mgmt_set_wifi_type( T_WIFI_TYPE_SERVER );
    hq_cmd_print( "Wi-Fi type set to SERVER (AP)." );
  }
  else if ( strcmp( mode, "apsta" ) == 0 )
  {
    wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLI_SER );
    hq_cmd_print( "Wi-Fi type set to AP+STA." );
  }
  else
  {
    hq_cmd_print( "Unknown type. Use: client, server, apsta" );
  }
}

static void _cmd_start( void )
{
  if ( !g_wifi_initialized )
  {
    wifi_mgmt_set_wifi_type( T_WIFI_TYPE_CLIENT );
    wifi_mgmt_init();
    g_wifi_initialized = true;
  }

  wifi_mgmt_start();
  hq_cmd_print( "Wi-Fi started." );
}

static void _cmd_stop( void )
{
  wifi_mgmt_stop();
  hq_cmd_print( "Wi-Fi stopped." );
}

static void _cmd_help( void )
{
  hq_cmd_print( "Wi-Fi commands:" );
  hq_cmd_print( "  wifi --scan                              Scan for APs" );
  hq_cmd_print( "  wifi --connect <ssid> [--password <pw>]  Connect" );
  hq_cmd_print( "  wifi --disconnect                        Disconnect" );
  hq_cmd_print( "  wifi --info                              IP / connection info" );
  hq_cmd_print( "  wifi --status                            Status flags" );
  hq_cmd_print( "  wifi --rssi                              Current RSSI" );
  hq_cmd_print( "  wifi --clients                           Soft-AP client count" );
  hq_cmd_print( "  wifi --saved                             Saved credentials" );
  hq_cmd_print( "  wifi --type <client|server|apsta>        Set mode" );
  hq_cmd_print( "  wifi --start                             Start state machine" );
  hq_cmd_print( "  wifi --stop                              Stop state machine" );
}

/* -------------------------------------------------------------------------- */
/*  Main handler                                                              */
/* -------------------------------------------------------------------------- */

static void hq_cmd_wifi_handler( hq_cmd_cli_t* cli, char* args, void* context )
{
  (void) cli;
  (void) context;

  if ( args == NULL || hq_cmd_get_token_count( args ) == 0
       || _has_option( args, "--help" ) || _has_option( args, "help" ) )
  {
    _cmd_help();
    return;
  }

  if ( _has_option( args, "--scan" ) )
  {
    _cmd_scan();
  }
  else if ( _has_option( args, "--connect" ) )
  {
    _cmd_connect( args );
  }
  else if ( _has_option( args, "--disconnect" ) )
  {
    _cmd_disconnect();
  }
  else if ( _has_option( args, "--info" ) )
  {
    _cmd_info();
  }
  else if ( _has_option( args, "--status" ) )
  {
    _cmd_status();
  }
  else if ( _has_option( args, "--rssi" ) )
  {
    _cmd_rssi();
  }
  else if ( _has_option( args, "--clients" ) )
  {
    _cmd_clients();
  }
  else if ( _has_option( args, "--saved" ) )
  {
    _cmd_saved();
  }
  else if ( _has_option( args, "--type" ) )
  {
    _cmd_type( args );
  }
  else if ( _has_option( args, "--start" ) )
  {
    _cmd_start();
  }
  else if ( _has_option( args, "--stop" ) )
  {
    _cmd_stop();
  }
  else
  {
    hq_cmd_print( "Unknown option. Type 'wifi' for help." );
  }
}

/* -------------------------------------------------------------------------- */
/*  Registration                                                              */
/* -------------------------------------------------------------------------- */

void hq_cmd_wifi_register( void )
{
  hq_cmd_binding_t binding = {
    .name          = "wifi",
    .help          = "Wi-Fi management (type 'wifi' for sub-commands)",
    .tokenize_args = true,
    .context       = NULL,
    .handler       = hq_cmd_wifi_handler,
  };

  (void) hq_cmd_register( &binding );
}
