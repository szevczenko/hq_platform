/**
 *******************************************************************************
 * @file    hq_cmd_mqtt.c
 * @author  Dmytro Shevchenko
 * @brief   CLI commands for MQTT management
 *******************************************************************************
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hq_cmd.h"
#include "mqtt_app.h"
#include "mqtt_config.h"

static bool g_mqtt_initialized = false;
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
      if ( i < count )
      {
        return hq_cmd_get_token( args, (uint16_t) ( i + 1 ) );
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

static void _cmd_help( void )
{
  hq_cmd_print( "MQTT commands:" );
  hq_cmd_print( "  mqtt --start                                  Init/start MQTT app" );
  hq_cmd_print( "  mqtt --stop                                   Deinit MQTT app" );
  hq_cmd_print( "  mqtt --status                                 Show connection status" );
  hq_cmd_print( "  mqtt --show                                   Show current config" );
  hq_cmd_print( "  mqtt --pub <topic> --msg <payload> [--qos n] Publish message" );
  hq_cmd_print( "  mqtt --set-address <url>                      Set broker url" );
  hq_cmd_print( "  mqtt --set-user <name>                        Set username" );
  hq_cmd_print( "  mqtt --set-pass <pass>                        Set password" );
  hq_cmd_print( "  mqtt --set-client-id <id>                     Set client id" );
  hq_cmd_print( "  mqtt --set-prefix <topic>                     Set prefix topic" );
  hq_cmd_print( "  mqtt --set-post-topic <topic>                 Set post topic" );
  hq_cmd_print( "  mqtt --set-ssl <0|1>                          Set SSL usage" );
  hq_cmd_print( "  mqtt --set-cert <pem-text>                    Set certificate in memory" );
  hq_cmd_print( "  mqtt --save                                   Save mqtt.json + cert file" );
}

static void _cmd_show( void )
{
  bool ssl = false;
  (void) MQTTConfig_GetBool( &ssl, MQTT_CONFIG_VALUE_SSL );

  _print_line( "address:    %s", MQTTConfig_GetString( MQTT_CONFIG_VALUE_ADDRESS ) );
  _print_line( "username:   %s", MQTTConfig_GetString( MQTT_CONFIG_VALUE_USERNAME ) );
  _print_line( "password:   %s", strlen( MQTTConfig_GetString( MQTT_CONFIG_VALUE_PASSWORD ) ) > 0 ? "****" : "(empty)" );
  _print_line( "client_id:  %s", MQTTConfig_GetString( MQTT_CONFIG_VALUE_CLIENT_ID ) );
  _print_line( "prefix:     %s", MQTTConfig_GetString( MQTT_CONFIG_VALUE_TOPIC_PREFIX ) );
  _print_line( "post topic: %s", MQTTConfig_GetString( MQTT_CONFIG_VALUE_POST_DATA_TOPIC ) );
  _print_line( "ssl:        %s", ssl ? "enabled" : "disabled" );
  _print_line( "cert:       %s", strlen( MQTTConfig_GetCert( MQTT_CONFIG_VALUE_CERT ) ) > 0 ? "loaded" : "empty" );
}

static void _cmd_set( const char* args )
{
  const char* value = NULL;

  if ( ( value = _find_option_value( args, "--set-address" ) ) )
  {
    (void) MQTTConfig_SetString( value, MQTT_CONFIG_VALUE_ADDRESS );
    hq_cmd_print( "MQTT address updated." );
    return;
  }
  if ( ( value = _find_option_value( args, "--set-user" ) ) )
  {
    (void) MQTTConfig_SetString( value, MQTT_CONFIG_VALUE_USERNAME );
    hq_cmd_print( "MQTT user updated." );
    return;
  }
  if ( ( value = _find_option_value( args, "--set-pass" ) ) )
  {
    (void) MQTTConfig_SetString( value, MQTT_CONFIG_VALUE_PASSWORD );
    hq_cmd_print( "MQTT password updated." );
    return;
  }
  if ( ( value = _find_option_value( args, "--set-client-id" ) ) )
  {
    (void) MQTTConfig_SetString( value, MQTT_CONFIG_VALUE_CLIENT_ID );
    hq_cmd_print( "MQTT client id updated." );
    return;
  }
  if ( ( value = _find_option_value( args, "--set-prefix" ) ) )
  {
    (void) MQTTConfig_SetString( value, MQTT_CONFIG_VALUE_TOPIC_PREFIX );
    hq_cmd_print( "MQTT prefix updated." );
    return;
  }
  if ( ( value = _find_option_value( args, "--set-post-topic" ) ) )
  {
    (void) MQTTConfig_SetString( value, MQTT_CONFIG_VALUE_POST_DATA_TOPIC );
    hq_cmd_print( "MQTT post topic updated." );
    return;
  }
  if ( ( value = _find_option_value( args, "--set-ssl" ) ) )
  {
    bool en = ( strcmp( value, "1" ) == 0 || strcmp( value, "true" ) == 0 );
    (void) MQTTConfig_SetBool( en, MQTT_CONFIG_VALUE_SSL );
    hq_cmd_print( "MQTT SSL flag updated." );
    return;
  }
  if ( ( value = _find_option_value( args, "--set-cert" ) ) )
  {
    (void) MQTTConfig_SetCert( value, strlen( value ), 0, MQTT_CONFIG_VALUE_CERT );
    hq_cmd_print( "MQTT cert updated in memory." );
    return;
  }

  hq_cmd_print( "No MQTT set option found." );
}

static void _cmd_publish( const char* args )
{
  const char* topic = _find_option_value( args, "--pub" );
  const char* msg = _find_option_value( args, "--msg" );
  const char* qos_str = _find_option_value( args, "--qos" );

  if ( topic && !msg )
  {
    uint16_t pub_pos = hq_cmd_find_token( args, "--pub" );
    if ( pub_pos != 0 )
    {
      const char* shorthand_msg = hq_cmd_get_token( args, (uint16_t) ( pub_pos + 2 ) );
      if ( shorthand_msg && strncmp( shorthand_msg, "--", 2 ) != 0 )
      {
        msg = shorthand_msg;
      }
    }
  }

  if ( !topic || !msg )
  {
    hq_cmd_print( "Usage: mqtt --pub <topic> --msg <payload> [--qos n]" );
    hq_cmd_print( "   or: mqtt --pub <topic> <payload> [--qos n]" );
    return;
  }

  int qos = 0;
  if ( qos_str )
  {
    qos = atoi( qos_str );
    if ( qos < 0 || qos > 2 )
    {
      hq_cmd_print( "Invalid QoS. Use 0, 1, or 2." );
      return;
    }
  }

  if ( !MqttApp_PostData( topic, msg, qos ) )
  {
    hq_cmd_print( "Publish queue failed." );
    return;
  }

  hq_cmd_print( "Message queued." );
}

static void hq_cmd_mqtt_handler( hq_cmd_cli_t* cli, char* args, void* context )
{
  (void) cli;
  (void) context;

  if ( args == NULL || hq_cmd_get_token_count( args ) == 0
       || _has_option( args, "--help" ) || _has_option( args, "help" ) )
  {
    _cmd_help();
    return;
  }

  if ( _has_option( args, "--start" ) )
  {
    MQTTConfig_Init();
    MqttApp_Init();
    g_mqtt_initialized = true;
    hq_cmd_print( "MQTT started." );
  }
  else if ( _has_option( args, "--stop" ) )
  {
    MqttApp_Deinit();
    g_mqtt_initialized = false;
    hq_cmd_print( "MQTT stopped." );
  }
  else if ( _has_option( args, "--status" ) )
  {
    _print_line( "Initialized: %s", g_mqtt_initialized ? "yes" : "no" );
    _print_line( "Connected:   %s", MqttApp_IsConnected() ? "yes" : "no" );
  }
  else if ( _has_option( args, "--show" ) )
  {
    _cmd_show();
  }
  else if ( _has_option( args, "--save" ) )
  {
    if ( MQTTConfig_Save() )
    {
      hq_cmd_print( "MQTT config saved." );
    }
    else
    {
      hq_cmd_print( "MQTT config save failed." );
    }
  }
  else if ( _has_option( args, "--pub" ) )
  {
    _cmd_publish( args );
  }
  else if ( _has_option( args, "--set-address" )
            || _has_option( args, "--set-user" )
            || _has_option( args, "--set-pass" )
            || _has_option( args, "--set-client-id" )
            || _has_option( args, "--set-prefix" )
            || _has_option( args, "--set-post-topic" )
            || _has_option( args, "--set-ssl" )
            || _has_option( args, "--set-cert" ) )
  {
    _cmd_set( args );
  }
  else
  {
    hq_cmd_print( "Unknown option. Type 'mqtt' for help." );
  }
}

void hq_cmd_mqtt_register( void )
{
  hq_cmd_binding_t binding = {
    .name = "mqtt",
    .help = "MQTT management (type 'mqtt' for sub-commands)",
    .tokenize_args = true,
    .context = NULL,
    .handler = hq_cmd_mqtt_handler,
  };

  (void) hq_cmd_register( &binding );
}