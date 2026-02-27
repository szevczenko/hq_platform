/**
 *******************************************************************************
 * @file    mqtt.c
 * @author  Dmytro Shevchenko
 * @brief   MQTT source file
 *******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "mqtt_config.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_err.h"
#include "esp_spiffs.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Private macros ------------------------------------------------------------*/

#define PARTITION_NAME    "dev_config"
#define STORAGE_NAMESPACE "mqtt_config"
#define MQTT_CERT_FILE    "/spiffs/mqtt.pem"

/* Private types -------------------------------------------------------------*/

typedef enum
{
  VALUE_TYPE_INT,
  VALUE_TYPE_BOOL,
  VALUE_TYPE_STRING,
  VALUE_TYPE_CERT
} value_type_t;

typedef struct
{
  const char* name;
  value_type_t type;
  void* value;
  void* default_value;
} value_t;

typedef struct
{
  char address[MQTT_CONFIG_STR_SIZE];
  char config_topic[MQTT_CONFIG_STR_SIZE];
  char control_topic[MQTT_CONFIG_STR_SIZE];
  char post_data_topic[MQTT_CONFIG_STR_SIZE];
  char username[MQTT_CONFIG_STR_SIZE];
  char password[MQTT_CONFIG_STR_SIZE];
  char cert[MQTT_CERT_MAX_SIZE];
  uint8_t use_ssl;
} config_data_t;

static mqtt_apply_config_cb apply_config_callback = NULL;

/* Private variables ---------------------------------------------------------*/
static config_data_t config_data;

#define _default_address       "mqtt://192.168.1.169:1883"
#define _default_config_topic  "/config/"
#define _default_control_topic "/control/"
#define _default_post_topic    "/post_data/"
static uint8_t default_tls = false;

static value_t config_values[MQTT_CONFIG_VALUE_LAST] =
  {
    [MQTT_CONFIG_VALUE_ADDRESS] = {.name = "address",   .type = VALUE_TYPE_STRING, .value = (void*) config_data.address,          .default_value = (void*) _default_address   },
    [MQTT_CONFIG_VALUE_SSL] = {.name = "ssl",       .type = VALUE_TYPE_BOOL,   .value = (void*) &config_data.use_ssl,         .default_value = (void*) &default_tls       },
    [MQTT_CONFIG_VALUE_TOPIC_PREFIX] = {.name = "prefix",    .type = VALUE_TYPE_STRING, .value = (void*) &config_data.config_topic,    .default_value = &_default_config_topic     },
    [MQTT_CONFIG_VALUE_POST_DATA_TOPIC] = {.name = "post",      .type = VALUE_TYPE_STRING, .value = (void*) &config_data.post_data_topic, .default_value = (void*) _default_post_topic},
    [MQTT_CONFIG_VALUE_USERNAME] = {.name = "user",      .type = VALUE_TYPE_STRING, .value = (void*) &config_data.username,        .default_value = (void*) ""                 },
    [MQTT_CONFIG_VALUE_PASSWORD] = {.name = "pass",      .type = VALUE_TYPE_STRING, .value = (void*) &config_data.password,        .default_value = (void*) ""                 },
    [MQTT_CONFIG_VALUE_CLIENT_ID] = {.name = "client_id", .type = VALUE_TYPE_STRING, .value = (void*) &config_data.control_topic,   .default_value = (void*) ""                 },
    [MQTT_CONFIG_VALUE_CERT] = {.name = "cert",      .type = VALUE_TYPE_CERT,   .value = (void*) &config_data.cert,            .default_value = (void*) ""                 },
};

static bool _read_data( void )
{
  nvs_handle_t my_handle;
  esp_err_t err;

  err = nvs_open_from_partition( PARTITION_NAME, STORAGE_NAMESPACE, NVS_READONLY, &my_handle );
  if ( err != ESP_OK )
  {
    return false;
  }

  for ( int i = 0; i < MQTT_CONFIG_VALUE_LAST; i++ )
  {
    switch ( config_values[i].type )
    {
      case VALUE_TYPE_INT:
        err = nvs_get_i32( my_handle, config_values[i].name, config_values[i].value );
        if ( err != ESP_OK )
        {
          printf( "[MQTT_CONFIG] not found in nvs %s\n\r", config_values[i].name );
          memcpy( config_values[i].value, config_values[i].default_value, sizeof( int32_t ) );
        }
        break;

      case VALUE_TYPE_BOOL:
        err = nvs_get_u8( my_handle, config_values[i].name, config_values[i].value );
        if ( err != ESP_OK )
        {
          printf( "[MQTT_CONFIG] not found in nvs %s\n\r", config_values[i].name );
          memcpy( config_values[i].value, config_values[i].default_value, sizeof( uint8_t ) );
        }
        break;

      case VALUE_TYPE_STRING:
        {
          size_t length = MQTT_CONFIG_STR_SIZE;
          err = nvs_get_str( my_handle, config_values[i].name, config_values[i].value, &length );
          if ( err != ESP_OK )
          {
            printf( "[MQTT_CONFIG] not found in nvs %s\n\r", config_values[i].name );
            strcpy( config_values[i].value, config_values[i].default_value );
          }
        }
        break;

      case VALUE_TYPE_CERT:
        {
          FILE* f = fopen( MQTT_CERT_FILE, "rb" );
          if ( f == NULL )
          {
            printf( "MQTT Failed to open file for reading" );
            break;
          }
          fread( config_data.cert, sizeof( config_data.cert ), 1, f );
          fclose( f );
        }
        break;
    }
    if ( err != ESP_OK )
    {
      printf( "[MQTT_CONFIG] not found in nvs %s\n\r", config_values[i].name );
    }
  }

  nvs_close( my_handle );
  return true;
}

static bool _save_data( void )
{
  nvs_handle_t my_handle;
  esp_err_t err;

  err = nvs_open_from_partition( PARTITION_NAME, STORAGE_NAMESPACE, NVS_READWRITE, &my_handle );
  if ( err != ESP_OK )
  {
    return false;
  }

  for ( int i = 0; i < MQTT_CONFIG_VALUE_LAST; i++ )
  {
    switch ( config_values[i].type )
    {
      case VALUE_TYPE_INT:
        err = nvs_set_i32( my_handle, config_values[i].name, *( (int32_t*) config_values[i].value ) );
        break;

      case VALUE_TYPE_BOOL:
        err = nvs_set_u8( my_handle, config_values[i].name, *( (uint8_t*) config_values[i].value ) );
        break;

      case VALUE_TYPE_STRING:
        err = nvs_set_str( my_handle, config_values[i].name, (const char*) config_values[i].value );
        break;

      case VALUE_TYPE_CERT:
        {
          struct stat st;
          if ( stat( MQTT_CERT_FILE, &st ) == 0 )
          {
            unlink( MQTT_CERT_FILE );
          }
          FILE* f = fopen( MQTT_CERT_FILE, "wb" );
          if ( f == NULL )
          {
            printf( "MQTT Failed to open file for writing" );
            break;
          }
          fwrite( config_data.cert, strlen( config_data.cert ), 1, f );
          fclose( f );
        }
        break;
    }
    if ( err != ESP_OK )
    {
      printf( "[MQTT_CONFIG] Cannot save %s\n\r", config_values[i].name );
    }
  }

  err = nvs_commit( my_handle );
  if ( err != ESP_OK )
  {
    nvs_close( my_handle );
    return false;
  }
  nvs_close( my_handle );
  return true;
}

static void _set_default_config( void )
{
  printf( "Set default mqtt config\n\r" );
  for ( int i = 0; i < MQTT_CONFIG_VALUE_LAST; i++ )
  {
    switch ( config_values[i].type )
    {
      case VALUE_TYPE_INT:
        memcpy( config_values[i].value, config_values[i].default_value, sizeof( int ) );
        break;

      case VALUE_TYPE_BOOL:
        memcpy( config_values[i].value, config_values[i].default_value, sizeof( bool ) );
        break;

      case VALUE_TYPE_STRING:
        strcpy( config_values[i].value, config_values[i].default_value );
        break;

      case VALUE_TYPE_CERT:
        memset( config_values[i].value, 0, MQTT_CERT_MAX_SIZE );
        break;
    }
  }
}

void MQTTConfig_Init( void )
{
  if ( false == _read_data() )
  {
    _set_default_config();
  }
}

bool MQTTConfig_SetInt( int value, mqtt_config_value_t config_value )
{
  assert( config_value < MQTT_CONFIG_VALUE_LAST );
  if ( config_values[config_value].type == VALUE_TYPE_INT )
  {
    int* set_value = (int*) config_values[config_value].value;
    *set_value = value;
    return true;
  }
  return false;
}

bool MQTTConfig_SetBool( bool value, mqtt_config_value_t config_value )
{
  assert( config_value < MQTT_CONFIG_VALUE_LAST );
  if ( config_values[config_value].type == VALUE_TYPE_BOOL )
  {
    bool* set_value = (bool*) config_values[config_value].value;
    *set_value = value;
    return true;
  }
  return false;
}

bool MQTTConfig_SetCert( const char* cert, size_t cert_len, size_t offset, mqtt_config_value_t config_value )
{
  assert( config_value < MQTT_CONFIG_VALUE_LAST );
  if ( config_values[config_value].type == VALUE_TYPE_CERT && cert_len + offset < MQTT_CERT_MAX_SIZE )
  {
    char* set_value = (char*) config_values[config_value].value;
    memcpy( &set_value[offset], cert, cert_len );
    return true;
  }
  return false;
}

bool MQTTConfig_SetString( const char* string, mqtt_config_value_t config_value )
{
  assert( string );
  assert( config_value < MQTT_CONFIG_VALUE_LAST );
  if ( config_values[config_value].type == VALUE_TYPE_STRING && strlen( string ) < MQTT_CONFIG_STR_SIZE )
  {
    char* set_value = (char*) config_values[config_value].value;
    strcpy( set_value, string );
    return true;
  }
  return false;
}

bool MQTTConfig_GetInt( int* value, mqtt_config_value_t config_value )
{
  assert( value );
  assert( config_value < MQTT_CONFIG_VALUE_LAST );
  if ( config_values[config_value].type == VALUE_TYPE_INT )
  {
    *value = *( (int*) config_values[config_value].value );
    return true;
  }
  return false;
}

bool MQTTConfig_GetBool( bool* value, mqtt_config_value_t config_value )
{
  assert( value );
  assert( config_value < MQTT_CONFIG_VALUE_LAST );
  if ( config_values[config_value].type == VALUE_TYPE_BOOL )
  {
    *value = *( (bool*) config_values[config_value].value );
    return true;
  }
  return false;
}

const char* MQTTConfig_GetString( mqtt_config_value_t config_value )
{
  assert( config_value < MQTT_CONFIG_VALUE_LAST );
  if ( config_values[config_value].type == VALUE_TYPE_STRING )
  {
    return (const char*) config_values[config_value].value;
  }
  return NULL;
}

const char* MQTTConfig_GetCert( mqtt_config_value_t config_value )
{
  assert( config_value < MQTT_CONFIG_VALUE_LAST );
  if ( config_values[config_value].type == VALUE_TYPE_CERT )
  {
    return (char*) config_values[config_value].value;
  }
  return NULL;
}

bool MQTTConfig_Save( void )
{
  bool result = _save_data();
  if ( apply_config_callback != NULL )
  {
    apply_config_callback();
  }
  return result;
}

void MQTTConfig_SetCallback( mqtt_apply_config_cb cb )
{
  apply_config_callback = cb;
}
