#include "mqtt_app.h"

#include <string.h>

#include "dev_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "mongoose.h"
#include "mongoose_task.h"
#include "mqtt_config.h"

#define RETRY_COUNT              3
#define MAX_SUBSCRIPTIONS        10
#define TIMEOUT_DEFAULT_MS       5000
#define RECONNECT_DELAY_MS       30000
#define MESSAGE_QUEUE_SIZE       6
#define MONGOOSE_TASK_STACK_SIZE 4096
#define MONGOOSE_TASK_PRIORITY   5

typedef struct
{
  char topic[64];
  char message[256];
  int qos;
} mqtt_message_t;

typedef struct
{
  char topic[128];
  mqtt_message_callback_t callback;
  bool active;
  bool pending_unsub;
} mqtt_subscription_t;

typedef struct
{
  int initialized;
  int connected;
  struct mg_connection* nc;
  mqtt_subscription_t subscriptions[MAX_SUBSCRIPTIONS];
  char pending_subscribe_topic[128];
  char pending_unsubscribe_topic[128];
  int retries;
  struct mg_mqtt_opts publish_opts;
} mqtt_state_t;

typedef struct
{
  TimerHandle_t reconnect;
  TimerHandle_t puback;
  TimerHandle_t suback;
  TimerHandle_t unsuback;
} mqtt_timers_t;

typedef struct
{
  QueueHandle_t message_queue;
  SemaphoreHandle_t puback;
  SemaphoreHandle_t suback;
  SemaphoreHandle_t unsuback;
} mqtt_sync_t;

typedef struct
{
  int puback_received;
  int suback_received;
  int unsuback_received;
} mqtt_ack_flags_t;

// Static variables
static mqtt_state_t mqtt_state = { 0 };
static mqtt_timers_t mqtt_timers = { 0 };
static mqtt_sync_t mqtt_sync = { 0 };
static mqtt_ack_flags_t mqtt_acks = { 0 };

// Forward declarations
static void ev_handler( struct mg_connection* nc, int ev, void* ev_data );
static void mqtt_connect( void );
static void mqtt_connected( void );
static void mqtt_publish_internal( const char* topic, const char* message, int qos );
static void mongoose_task( void* pvParameters );

// Subscription management functions
static mqtt_subscription_t* find_subscription( const char* topic )
{
  if ( !topic )
    return NULL;

  for ( int i = 0; i < MAX_SUBSCRIPTIONS; i++ )
  {
    if ( mqtt_state.subscriptions[i].active && strcmp( mqtt_state.subscriptions[i].topic, topic ) == 0 )
    {
      return &mqtt_state.subscriptions[i];
    }
  }
  return NULL;
}

static mqtt_subscription_t* find_free_subscription_slot( void )
{
  for ( int i = 0; i < MAX_SUBSCRIPTIONS; i++ )
  {
    if ( !mqtt_state.subscriptions[i].active )
    {
      return &mqtt_state.subscriptions[i];
    }
  }
  return NULL;
}

static void clear_subscription( mqtt_subscription_t* sub )
{
  if ( sub )
  {
    memset( sub->topic, 0, sizeof( sub->topic ) );
    sub->callback = NULL;
    sub->active = false;
    sub->pending_unsub = false;
  }
}

static void resubscribe_all( void )
{
  for ( int i = 0; i < MAX_SUBSCRIPTIONS; i++ )
  {
    if ( mqtt_state.subscriptions[i].active )
    {
      mg_mqtt_sub( mqtt_state.nc, &(struct mg_mqtt_opts) {
                                    .topic = mg_str( mqtt_state.subscriptions[i].topic ),
                                    .qos = 0 } );
    }
  }
}

// Timer callback functions
static void reconnect_timer_callback( TimerHandle_t xTimer )
{
  if ( !mqtt_state.connected )
  {
    printf( "Attempting to reconnect...\n" );
    mqtt_connect();
  }
}

static void puback_timer_callback( TimerHandle_t xTimer )
{
  if ( !mqtt_acks.puback_received && mqtt_state.retries < RETRY_COUNT )
  {
    printf( "Retrying QoS 1 message, attempt %d...\n", mqtt_state.retries + 1 );
    mg_mqtt_pub( mqtt_state.nc, &mqtt_state.publish_opts );
    mqtt_state.retries++;
  }
  else if ( mqtt_state.retries >= RETRY_COUNT )
  {
    printf( "Failed to receive PUBACK after %d retries\n", RETRY_COUNT );
    xSemaphoreGive( mqtt_sync.puback );
    xTimerStop( mqtt_timers.puback, 0 );
  }
}

static void suback_timer_callback( TimerHandle_t xTimer )
{
  if ( !mqtt_acks.suback_received )
  {
    printf( "SUBACK timeout for topic: %s\n", mqtt_state.pending_subscribe_topic );
    xSemaphoreGive( mqtt_sync.suback );
  }
}

static void unsuback_timer_callback( TimerHandle_t xTimer )
{
  if ( !mqtt_acks.unsuback_received )
  {
    printf( "UNSUBACK timeout for topic: %s\n", mqtt_state.pending_unsubscribe_topic );
    xSemaphoreGive( mqtt_sync.unsuback );
  }
}

// Connection management
static void mqtt_connect( void )
{
  if ( mqtt_state.nc != NULL )
  {
    mqtt_state.nc->is_closing = 1;
  }

  const char* address = MQTTConfig_GetString( MQTT_CONFIG_VALUE_ADDRESS );
  const char* username = MQTTConfig_GetString( MQTT_CONFIG_VALUE_USERNAME );
  const char* password = MQTTConfig_GetString( MQTT_CONFIG_VALUE_PASSWORD );
  const char* client_id = MQTTConfig_GetString( MQTT_CONFIG_VALUE_CLIENT_ID );

  // Use configured client_id, fallback to serial number if empty
  const char* effective_client_id = ( client_id && strlen( client_id ) > 0 ) ? client_id : DevConfig_GetSerialNumber();

  // Ensure we have a valid client_id
  if ( !effective_client_id || strlen( effective_client_id ) == 0 )
  {
    effective_client_id = "esp32_device";    // Default fallback
  }

  printf( "Connecting to MQTT server at %s\n", address );
  printf( "  Client ID: %s\n", effective_client_id );
  printf( "  Username: %s\n", username ? username : "(null)" );
  printf( "  Password: %s\n", password ? "***" : "(null)" );

  struct mg_mqtt_opts opts_con = {
    .user = mg_str( username ? username : "" ),
    .pass = mg_str( password ? password : "" ),
    .client_id = mg_str( effective_client_id ),
    .keepalive = 60,
    .clean = true };

  mqtt_state.nc = mg_mqtt_connect( &mgr, address, &opts_con, ev_handler, NULL );
  if ( mqtt_state.nc == NULL )
  {
    printf( "Failed to create MQTT connection\n" );
  }
}

static void mqtt_connected( void )
{
  mqtt_state.connected = 1;
  const char* address = MQTTConfig_GetString( MQTT_CONFIG_VALUE_ADDRESS );

  if ( mg_url_is_ssl( address ) )
  {
    const char* cert = MQTTConfig_GetCert( MQTT_CONFIG_VALUE_CERT );
    struct mg_tls_opts opts_ca = {
      .ca = mg_str( cert ),
      .name = mg_url_host( address ),
    };
    mg_tls_init( mqtt_state.nc, &opts_ca );
  }

  xTimerStop( mqtt_timers.reconnect, 0 );
  printf( "Connected to MQTT server\n" );
  resubscribe_all();
}

static void mqtt_disconnected( void )
{
  mqtt_state.connected = 0;
  printf( "Disconnected from MQTT server\n" );
  xTimerStart( mqtt_timers.reconnect, 0 );
}

// Message handling
static void handle_mqtt_message( struct mg_mqtt_message* mm )
{
  MG_INFO( ( "%lu RECEIVED %.*s <- %.*s", mqtt_state.nc->id, (int) mm->data.len,
             mm->data.buf, (int) mm->topic.len, mm->topic.buf ) );

  char topic_str[129] = { 0 };
  size_t topic_len = mm->topic.len < 128 ? mm->topic.len : 128;
  memcpy( topic_str, mm->topic.buf, topic_len );

  for ( int i = 0; i < MAX_SUBSCRIPTIONS; i++ )
  {
    if ( mqtt_state.subscriptions[i].active && mqtt_state.subscriptions[i].callback )
    {
      if ( mg_match( mg_str( topic_str ), mg_str( mqtt_state.subscriptions[i].topic ), NULL ) )
      {
        mqtt_state.subscriptions[i].callback( topic_str, mm->data.buf, mm->data.len );
        break;
      }
    }
  }
}

static void handle_mqtt_command( struct mg_mqtt_message* mm )
{
  switch ( mm->cmd )
  {
    case MQTT_CMD_SUBACK:
      mqtt_acks.suback_received = 1;
      xTimerStop( mqtt_timers.suback, 0 );
      xSemaphoreGive( mqtt_sync.suback );
      printf( "Subscribed to topic: %s\n", mqtt_state.pending_subscribe_topic );
      break;

    case MQTT_CMD_UNSUBACK:
      mqtt_acks.unsuback_received = 1;
      xTimerStop( mqtt_timers.unsuback, 0 );
      xSemaphoreGive( mqtt_sync.unsuback );
      printf( "Unsubscribed from topic: %s\n", mqtt_state.pending_unsubscribe_topic );
      break;

    case MQTT_CMD_PUBACK:
      mqtt_acks.puback_received = 1;
      xTimerStop( mqtt_timers.puback, 0 );
      xSemaphoreGive( mqtt_sync.puback );
      printf( "PUBACK received\n" );
      break;

    case MQTT_CMD_PINGRESP:
      printf( "PINGRESP received\n" );
      break;

    case MQTT_CMD_PUBREC:
      printf( "PUBREC received\n" );
      break;

    case MQTT_CMD_PUBCOMP:
      printf( "PUBCOMP received\n" );
      break;

    default:
      break;
  }
}

// Event handler
static void ev_handler( struct mg_connection* nc, int ev, void* ev_data )
{
  switch ( ev )
  {
    case MG_EV_CONNECT:
      printf( "MQTT transport connected\n" );
      mqtt_connected();
      break;

    case MG_EV_MQTT_CMD:
      handle_mqtt_command( (struct mg_mqtt_message*) ev_data );
      break;

    case MG_EV_MQTT_MSG:
      handle_mqtt_message( (struct mg_mqtt_message*) ev_data );
      break;

    case MG_EV_CLOSE:
      printf( "MQTT connection closed\n" );
      mqtt_disconnected();
      break;

    case MG_EV_ERROR:
      printf( "MQTT connection error: %s\n", (char*) ev_data );
      mqtt_disconnected();
      break;

    default:
      break;
  }
}

// Publishing functions
static void mqtt_publish_internal( const char* topic, const char* message, int qos )
{
  if ( !mqtt_state.nc || !mqtt_state.connected )
  {
    printf( "Cannot publish: not connected\n" );
    return;
  }

  printf( "Publishing to topic '%s': %s\n", topic, message );

  mqtt_acks.puback_received = 0;
  memset( &mqtt_state.publish_opts, 0, sizeof( mqtt_state.publish_opts ) );

  mqtt_state.publish_opts.qos = qos;
  mqtt_state.publish_opts.topic = mg_str( topic );
  mqtt_state.publish_opts.version = 4;
  mqtt_state.publish_opts.message = mg_str( message );
  mqtt_state.retries = 0;

  mg_mqtt_pub( mqtt_state.nc, &mqtt_state.publish_opts );

  if ( qos == 1 )
  {
    xTimerStart( mqtt_timers.puback, 0 );
    if ( xSemaphoreTake( mqtt_sync.puback, pdMS_TO_TICKS( TIMEOUT_DEFAULT_MS * RETRY_COUNT + 100 ) ) == pdFALSE )
    {
      printf( "Failed to receive PUBACK after %d retries\n", RETRY_COUNT );
    }
    else
    {
      printf( "PUBACK received for topic '%s'\n", topic );
    }
  }
}

// Task function
static void mongoose_task( void* pvParameters )
{
  mqtt_message_t msg;
  while ( 1 )
  {
    if ( xQueueReceive( mqtt_sync.message_queue, &msg, portMAX_DELAY ) )
    {
      if ( mqtt_state.connected )
      {
        mqtt_publish_internal( msg.topic, msg.message, msg.qos );
      }
    }
  }
}

// Configuration update callback
static void config_update_callback( void )
{
  MqttApp_Deinit();
  MqttApp_Init();
}

// Public API functions
bool MqttApp_Subscribe( const char* topic, int qos, mqtt_message_callback_t callback, uint32_t timeout_ms )
{
  if ( !mqtt_state.initialized || !mqtt_state.connected || !topic || !callback )
  {
    return false;
  }

  mqtt_subscription_t* existing = find_subscription( topic );
  if ( existing )
  {
    existing->callback = callback;
    return true;
  }

  mqtt_subscription_t* sub = find_free_subscription_slot();
  if ( !sub )
  {
    printf( "No free subscription slots available\n" );
    return false;
  }

  strncpy( sub->topic, topic, sizeof( sub->topic ) - 1 );
  sub->callback = callback;
  sub->active = true;
  sub->pending_unsub = false;

  strncpy( mqtt_state.pending_subscribe_topic, topic, sizeof( mqtt_state.pending_subscribe_topic ) - 1 );

  mqtt_acks.suback_received = 0;
  mg_mqtt_sub( mqtt_state.nc, &(struct mg_mqtt_opts) { .topic = mg_str( topic ), .qos = qos } );

  xTimerChangePeriod( mqtt_timers.suback, pdMS_TO_TICKS( timeout_ms ), 0 );
  xTimerStart( mqtt_timers.suback, 0 );

  if ( xSemaphoreTake( mqtt_sync.suback, pdMS_TO_TICKS( timeout_ms + 100 ) ) == pdFALSE )
  {
    printf( "Subscribe timeout for topic: %s\n", topic );
    clear_subscription( sub );
    return false;
  }

  if ( !mqtt_acks.suback_received )
  {
    printf( "Subscribe failed for topic: %s\n", topic );
    clear_subscription( sub );
    return false;
  }

  printf( "Successfully subscribed to topic: %s\n", topic );
  return true;
}

bool MqttApp_Unsubscribe( const char* topic, uint32_t timeout_ms )
{
  if ( !mqtt_state.initialized || !mqtt_state.connected || !topic )
  {
    return false;
  }

  mqtt_subscription_t* sub = find_subscription( topic );
  if ( !sub )
  {
    printf( "Topic not found in subscriptions: %s\n", topic );
    return false;
  }

  strncpy( mqtt_state.pending_unsubscribe_topic, topic, sizeof( mqtt_state.pending_unsubscribe_topic ) - 1 );

  mqtt_acks.unsuback_received = 0;

  // Build UNSUBSCRIBE packet manually since mg_mqtt_unsub doesn't exist
  struct mg_str topic_str = mg_str( topic );
  size_t packet_len = 2 + 2 + topic_str.len;    // packet_id (2) + topic_len (2) + topic

  mg_mqtt_send_header( mqtt_state.nc, MQTT_CMD_UNSUBSCRIBE, 0x02, packet_len );

  // Send packet ID (using a simple counter, could be more sophisticated)
  static uint16_t packet_id = 1;
  uint8_t id_bytes[2] = { ( packet_id >> 8 ) & 0xFF, packet_id & 0xFF };
  mg_send( mqtt_state.nc, id_bytes, 2 );
  packet_id++;

  // Send topic length and topic
  uint8_t topic_len_bytes[2] = { ( topic_str.len >> 8 ) & 0xFF, topic_str.len & 0xFF };
  mg_send( mqtt_state.nc, topic_len_bytes, 2 );
  mg_send( mqtt_state.nc, topic_str.buf, topic_str.len );

  xTimerChangePeriod( mqtt_timers.unsuback, pdMS_TO_TICKS( timeout_ms ), 0 );
  xTimerStart( mqtt_timers.unsuback, 0 );

  if ( xSemaphoreTake( mqtt_sync.unsuback, pdMS_TO_TICKS( timeout_ms + 100 ) ) == pdFALSE )
  {
    printf( "Unsubscribe timeout for topic: %s\n", topic );
    return false;
  }

  if ( !mqtt_acks.unsuback_received )
  {
    printf( "Unsubscribe failed for topic: %s\n", topic );
    return false;
  }

  clear_subscription( sub );
  printf( "Successfully unsubscribed from topic: %s\n", topic );
  return true;
}

void MqttApp_Init( void )
{
  assert( mqtt_state.initialized == 0 );

  MQTTConfig_Init();
  MQTTConfig_SetCallback( config_update_callback );

  // Create timers
  mqtt_timers.puback = xTimerCreate( "PubackTimer", pdMS_TO_TICKS( TIMEOUT_DEFAULT_MS ), pdTRUE, NULL, puback_timer_callback );
  mqtt_timers.reconnect = xTimerCreate( "ReconnectTimer", pdMS_TO_TICKS( RECONNECT_DELAY_MS ), pdTRUE, NULL, reconnect_timer_callback );
  mqtt_timers.suback = xTimerCreate( "SubackTimer", pdMS_TO_TICKS( TIMEOUT_DEFAULT_MS ), pdFALSE, NULL, suback_timer_callback );
  mqtt_timers.unsuback = xTimerCreate( "UnsubackTimer", pdMS_TO_TICKS( TIMEOUT_DEFAULT_MS ), pdFALSE, NULL, unsuback_timer_callback );

  assert( mqtt_timers.puback != NULL );
  assert( mqtt_timers.reconnect != NULL );
  assert( mqtt_timers.suback != NULL );
  assert( mqtt_timers.unsuback != NULL );

  // Create synchronization objects
  mqtt_sync.message_queue = xQueueCreate( MESSAGE_QUEUE_SIZE, sizeof( mqtt_message_t ) );
  mqtt_sync.puback = xSemaphoreCreateBinary();
  mqtt_sync.suback = xSemaphoreCreateBinary();
  mqtt_sync.unsuback = xSemaphoreCreateBinary();

  assert( mqtt_sync.message_queue != NULL );
  assert( mqtt_sync.puback != NULL );
  assert( mqtt_sync.suback != NULL );
  assert( mqtt_sync.unsuback != NULL );

  // Initialize state
  memset( mqtt_state.subscriptions, 0, sizeof( mqtt_state.subscriptions ) );
  memset( &mqtt_acks, 0, sizeof( mqtt_acks ) );

  xTaskCreate( mongoose_task, "MongooseTask", MONGOOSE_TASK_STACK_SIZE, NULL, MONGOOSE_TASK_PRIORITY, NULL );
  xTimerStart( mqtt_timers.reconnect, 0 );
  mqtt_connect();
  mqtt_state.initialized = 1;
}

void MqttApp_Deinit( void )
{
  if ( mqtt_state.initialized == 0 )
  {
    return;
  }

  // Delete timers
  xTimerDelete( mqtt_timers.reconnect, 0 );
  xTimerDelete( mqtt_timers.suback, 0 );
  xTimerDelete( mqtt_timers.unsuback, 0 );
  xTimerDelete( mqtt_timers.puback, 0 );

  // Delete synchronization objects
  vQueueDelete( mqtt_sync.message_queue );
  vSemaphoreDelete( mqtt_sync.puback );
  vSemaphoreDelete( mqtt_sync.suback );
  vSemaphoreDelete( mqtt_sync.unsuback );

  // Clear subscriptions
  memset( mqtt_state.subscriptions, 0, sizeof( mqtt_state.subscriptions ) );

  if ( mqtt_state.connected )
  {
    mg_mqtt_disconnect( mqtt_state.nc, NULL );
    mqtt_state.nc->is_closing = 1;
    mqtt_state.nc = NULL;
  }

  // Reset state
  memset( &mqtt_state, 0, sizeof( mqtt_state ) );
  memset( &mqtt_timers, 0, sizeof( mqtt_timers ) );
  memset( &mqtt_sync, 0, sizeof( mqtt_sync ) );
  memset( &mqtt_acks, 0, sizeof( mqtt_acks ) );
}

bool MqttApp_PostData( const char* topic, const char* message, int qos )
{
  if ( mqtt_state.initialized == 0 )
  {
    return false;
  }

  mqtt_message_t msg = { 0 };

  if ( strlen( topic ) >= sizeof( msg.topic ) )
  {
    printf( "ERROR: Topic too long\n" );
    return false;
  }
  if ( strlen( message ) >= sizeof( msg.message ) )
  {
    printf( "ERROR: Message too long\n" );
    return false;
  }

  strncpy( msg.topic, topic, sizeof( msg.topic ) - 1 );
  strncpy( msg.message, message, sizeof( msg.message ) - 1 );
  msg.topic[sizeof( msg.topic ) - 1] = '\0';
  msg.message[sizeof( msg.message ) - 1] = '\0';
  msg.qos = qos;

  return xQueueSend( mqtt_sync.message_queue, &msg, 0 ) == pdTRUE;
}

bool MqttApp_IsConnected( void )
{
  return mqtt_state.initialized && mqtt_state.connected;
}
