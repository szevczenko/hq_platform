#include "mqtt_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "osal_file.h"
#include "osal_log.h"

#define MQTT_CONFIG_MAX_JSON_SIZE 4096

typedef struct
{
  char address[MQTT_CONFIG_STR_SIZE];
  char topic_prefix[MQTT_CONFIG_STR_SIZE];
  char post_data_topic[MQTT_CONFIG_STR_SIZE];
  char username[MQTT_CONFIG_STR_SIZE];
  char password[MQTT_CONFIG_STR_SIZE];
  char client_id[MQTT_CONFIG_STR_SIZE];
  char cert[MQTT_CERT_MAX_SIZE];
  uint8_t use_ssl;
} config_data_t;

static mqtt_apply_config_cb g_apply_config_callback = NULL;
static config_data_t g_config_data;

static const char* g_default_address = "mqtt://192.168.1.169:1883";
static const char* g_default_prefix = "/config/";
static const char* g_default_post_topic = "/post_data/";
static const uint8_t g_default_ssl = 0;

static void _str_copy_safe( char* dst, size_t dst_size, const char* src )
{
  if ( !dst || dst_size == 0 )
  {
    return;
  }

  if ( !src )
  {
    dst[0] = '\0';
    return;
  }

  strncpy( dst, src, dst_size - 1 );
  dst[dst_size - 1] = '\0';
}

static void _set_defaults( void )
{
  memset( &g_config_data, 0, sizeof( g_config_data ) );
  _str_copy_safe( g_config_data.address, sizeof( g_config_data.address ), g_default_address );
  _str_copy_safe( g_config_data.topic_prefix, sizeof( g_config_data.topic_prefix ), g_default_prefix );
  _str_copy_safe( g_config_data.post_data_topic, sizeof( g_config_data.post_data_topic ), g_default_post_topic );
  g_config_data.use_ssl = g_default_ssl;
}

static bool _read_file_to_buffer( const char* path, char** out_buf )
{
  if ( !path || !out_buf )
  {
    return false;
  }

  *out_buf = NULL;

  osal_fstat_t st = { 0 };
  if ( osal_stat( path, &st ) != OSAL_SUCCESS )
  {
    return false;
  }

  if ( st.file_size == 0 || st.file_size > MQTT_CONFIG_MAX_JSON_SIZE )
  {
    return false;
  }

  osal_file_id_t fd = osal_open_create( path, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY );
  if ( fd < 0 )
  {
    return false;
  }

  char* buffer = (char*) calloc( 1u, st.file_size + 1u );
  if ( !buffer )
  {
    (void) osal_close( fd );
    return false;
  }

  int32_t read_rc = osal_read( fd, buffer, st.file_size );
  (void) osal_close( fd );
  if ( read_rc < 0 )
  {
    free( buffer );
    return false;
  }

  buffer[read_rc] = '\0';
  *out_buf = buffer;
  return true;
}

static bool _write_buffer_to_file( const char* path, const char* data, size_t len )
{
  osal_file_id_t fd = osal_open_create( path,
                                        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
                                        OSAL_WRITE_ONLY );
  if ( fd < 0 )
  {
    return false;
  }

  int32_t write_rc = osal_write( fd, data, len );
  (void) osal_close( fd );
  return write_rc >= 0 && (size_t) write_rc == len;
}

static bool _load_cert_from_file( void )
{
  osal_fstat_t st = { 0 };
  if ( osal_stat( MQTT_CERT_FILE_PATH, &st ) != OSAL_SUCCESS || st.file_size == 0 )
  {
    g_config_data.cert[0] = '\0';
    return false;
  }

  osal_file_id_t fd = osal_open_create( MQTT_CERT_FILE_PATH, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY );
  if ( fd < 0 )
  {
    g_config_data.cert[0] = '\0';
    return false;
  }

  size_t max_read = sizeof( g_config_data.cert ) - 1;
  size_t to_read = st.file_size < max_read ? st.file_size : max_read;
  int32_t read_rc = osal_read( fd, g_config_data.cert, to_read );
  (void) osal_close( fd );

  if ( read_rc < 0 )
  {
    g_config_data.cert[0] = '\0';
    return false;
  }

  g_config_data.cert[read_rc] = '\0';
  return true;
}

static bool _save_cert_to_file( void )
{
  size_t cert_len = strnlen( g_config_data.cert, sizeof( g_config_data.cert ) );
  if ( cert_len == 0 )
  {
    (void) osal_remove( MQTT_CERT_FILE_PATH );
    return true;
  }

  return _write_buffer_to_file( MQTT_CERT_FILE_PATH, g_config_data.cert, cert_len );
}

static bool _load_json_config( void )
{
  char* content = NULL;
  if ( !_read_file_to_buffer( MQTT_CONFIG_FILE_PATH, &content ) )
  {
    return false;
  }

  cJSON* root = cJSON_Parse( content );
  free( content );
  if ( !root )
  {
    return false;
  }

  cJSON* item = NULL;

  item = cJSON_GetObjectItemCaseSensitive( root, "address" );
  if ( cJSON_IsString( item ) && item->valuestring )
  {
    _str_copy_safe( g_config_data.address, sizeof( g_config_data.address ), item->valuestring );
  }

  item = cJSON_GetObjectItemCaseSensitive( root, "ssl" );
  if ( cJSON_IsBool( item ) )
  {
    g_config_data.use_ssl = cJSON_IsTrue( item ) ? 1 : 0;
  }

  item = cJSON_GetObjectItemCaseSensitive( root, "prefix" );
  if ( cJSON_IsString( item ) && item->valuestring )
  {
    _str_copy_safe( g_config_data.topic_prefix, sizeof( g_config_data.topic_prefix ), item->valuestring );
  }

  item = cJSON_GetObjectItemCaseSensitive( root, "post" );
  if ( cJSON_IsString( item ) && item->valuestring )
  {
    _str_copy_safe( g_config_data.post_data_topic, sizeof( g_config_data.post_data_topic ), item->valuestring );
  }

  item = cJSON_GetObjectItemCaseSensitive( root, "user" );
  if ( cJSON_IsString( item ) && item->valuestring )
  {
    _str_copy_safe( g_config_data.username, sizeof( g_config_data.username ), item->valuestring );
  }

  item = cJSON_GetObjectItemCaseSensitive( root, "pass" );
  if ( cJSON_IsString( item ) && item->valuestring )
  {
    _str_copy_safe( g_config_data.password, sizeof( g_config_data.password ), item->valuestring );
  }

  item = cJSON_GetObjectItemCaseSensitive( root, "client_id" );
  if ( cJSON_IsString( item ) && item->valuestring )
  {
    _str_copy_safe( g_config_data.client_id, sizeof( g_config_data.client_id ), item->valuestring );
  }

  cJSON_Delete( root );
  return true;
}

static bool _save_json_config( void )
{
  cJSON* root = cJSON_CreateObject();
  if ( !root )
  {
    return false;
  }

  bool ok = cJSON_AddStringToObject( root, "address", g_config_data.address ) != NULL;
  ok = ok && cJSON_AddBoolToObject( root, "ssl", g_config_data.use_ssl != 0 ) != NULL;
  ok = ok && cJSON_AddStringToObject( root, "prefix", g_config_data.topic_prefix ) != NULL;
  ok = ok && cJSON_AddStringToObject( root, "post", g_config_data.post_data_topic ) != NULL;
  ok = ok && cJSON_AddStringToObject( root, "user", g_config_data.username ) != NULL;
  ok = ok && cJSON_AddStringToObject( root, "pass", g_config_data.password ) != NULL;
  ok = ok && cJSON_AddStringToObject( root, "client_id", g_config_data.client_id ) != NULL;

  if ( !ok )
  {
    cJSON_Delete( root );
    return false;
  }

  char* json = cJSON_PrintUnformatted( root );
  cJSON_Delete( root );
  if ( !json )
  {
    return false;
  }

  bool write_ok = _write_buffer_to_file( MQTT_CONFIG_FILE_PATH, json, strlen( json ) );
  free( json );
  return write_ok;
}

void MQTTConfig_Init( void )
{
  _set_defaults();
  (void) _load_json_config();
  (void) _load_cert_from_file();
}

bool MQTTConfig_SetInt( int value, mqtt_config_value_t config_value )
{
  (void) value;
  (void) config_value;
  return false;
}

bool MQTTConfig_SetBool( bool value, mqtt_config_value_t config_value )
{
  if ( config_value == MQTT_CONFIG_VALUE_SSL )
  {
    g_config_data.use_ssl = value ? 1u : 0u;
    return true;
  }
  return false;
}

bool MQTTConfig_SetCert( const char* cert, size_t cert_len, size_t offset, mqtt_config_value_t config_value )
{
  if ( !cert || config_value != MQTT_CONFIG_VALUE_CERT )
  {
    return false;
  }

  if ( offset >= sizeof( g_config_data.cert ) || cert_len > sizeof( g_config_data.cert ) - offset - 1u )
  {
    return false;
  }

  memcpy( &g_config_data.cert[offset], cert, cert_len );
  g_config_data.cert[offset + cert_len] = '\0';
  return true;
}

bool MQTTConfig_SetString( const char* string, mqtt_config_value_t config_value )
{
  if ( !string )
  {
    return false;
  }

  switch ( config_value )
  {
    case MQTT_CONFIG_VALUE_ADDRESS:
      _str_copy_safe( g_config_data.address, sizeof( g_config_data.address ), string );
      return true;
    case MQTT_CONFIG_VALUE_TOPIC_PREFIX:
      _str_copy_safe( g_config_data.topic_prefix, sizeof( g_config_data.topic_prefix ), string );
      return true;
    case MQTT_CONFIG_VALUE_POST_DATA_TOPIC:
      _str_copy_safe( g_config_data.post_data_topic, sizeof( g_config_data.post_data_topic ), string );
      return true;
    case MQTT_CONFIG_VALUE_USERNAME:
      _str_copy_safe( g_config_data.username, sizeof( g_config_data.username ), string );
      return true;
    case MQTT_CONFIG_VALUE_PASSWORD:
      _str_copy_safe( g_config_data.password, sizeof( g_config_data.password ), string );
      return true;
    case MQTT_CONFIG_VALUE_CLIENT_ID:
      _str_copy_safe( g_config_data.client_id, sizeof( g_config_data.client_id ), string );
      return true;
    default:
      return false;
  }
}

bool MQTTConfig_GetInt( int* value, mqtt_config_value_t config_value )
{
  (void) value;
  (void) config_value;
  return false;
}

bool MQTTConfig_GetBool( bool* value, mqtt_config_value_t config_value )
{
  if ( !value )
  {
    return false;
  }

  if ( config_value == MQTT_CONFIG_VALUE_SSL )
  {
    *value = g_config_data.use_ssl != 0;
    return true;
  }
  return false;
}

const char* MQTTConfig_GetString( mqtt_config_value_t config_value )
{
  switch ( config_value )
  {
    case MQTT_CONFIG_VALUE_ADDRESS: return g_config_data.address;
    case MQTT_CONFIG_VALUE_TOPIC_PREFIX: return g_config_data.topic_prefix;
    case MQTT_CONFIG_VALUE_POST_DATA_TOPIC: return g_config_data.post_data_topic;
    case MQTT_CONFIG_VALUE_USERNAME: return g_config_data.username;
    case MQTT_CONFIG_VALUE_PASSWORD: return g_config_data.password;
    case MQTT_CONFIG_VALUE_CLIENT_ID: return g_config_data.client_id;
    default: return NULL;
  }
}

const char* MQTTConfig_GetCert( mqtt_config_value_t config_value )
{
  if ( config_value == MQTT_CONFIG_VALUE_CERT )
  {
    return g_config_data.cert;
  }
  return NULL;
}

bool MQTTConfig_Save( void )
{
  bool ok = _save_json_config() && _save_cert_to_file();
  if ( ok && g_apply_config_callback )
  {
    g_apply_config_callback();
  }
  if ( !ok )
  {
    osal_log_error( "mqtt config save failed" );
  }
  return ok;
}

void MQTTConfig_SetCallback( mqtt_apply_config_cb cb )
{
  g_apply_config_callback = cb;
}