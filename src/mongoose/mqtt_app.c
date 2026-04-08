#include "mqtt_app.h"

#include <stdio.h>
#include <string.h>

#include "mongoose.h"
#include "mongoose_process.h"
#include "mqtt_config.h"
#include "osal_bin_sem.h"
#include "osal_log.h"
#include "osal_queue.h"
#include "osal_task.h"
#include "osal_timer.h"

#define RETRY_COUNT              3
#define MAX_SUBSCRIPTIONS        10
#define TIMEOUT_DEFAULT_MS       5000
#define RECONNECT_DELAY_MS       30000
#define PING_INTERVAL_MS         30000
#define MESSAGE_QUEUE_SIZE       6
#define MONGOOSE_TASK_STACK_SIZE 4096
#define MONGOOSE_TASK_PRIORITY   5

typedef struct
{
  int type;
  char topic[64];
  char message[256];
  int qos;
} mqtt_message_t;

typedef enum
{
  MQTT_WORKER_MSG_PUBLISH = 0,
  MQTT_WORKER_MSG_RECONNECT = 1
} mqtt_worker_msg_type_t;

typedef struct
{
  char topic[128];
  mqtt_message_callback_t callback;
  bool active;
} mqtt_subscription_t;

typedef struct
{
  bool initialized;
  bool connected;
  struct mg_connection* nc;
  struct mg_timer* reconnect_timer;
  struct mg_timer* ping_timer;
  mqtt_subscription_t subscriptions[MAX_SUBSCRIPTIONS];
  char pending_subscribe_topic[128];
  char pending_unsubscribe_topic[128];
  char publish_topic[64];
  char publish_message[256];
  int retries;
  struct mg_mqtt_opts publish_opts;
  osal_task_id_t worker_task_id;
} mqtt_state_t;

typedef struct
{
  osal_timer_id_t puback;
  osal_timer_id_t suback;
  osal_timer_id_t unsuback;
} mqtt_timers_t;

typedef struct
{
  osal_queue_id_t message_queue;
  osal_bin_sem_id_t puback;
  osal_bin_sem_id_t suback;
  osal_bin_sem_id_t unsuback;
} mqtt_sync_t;

typedef struct
{
  bool puback_received;
  bool suback_received;
  bool unsuback_received;
} mqtt_ack_flags_t;

static mqtt_state_t mqtt_state = { 0 };
static mqtt_timers_t mqtt_timers = { 0 };
static mqtt_sync_t mqtt_sync = { 0 };
static mqtt_ack_flags_t mqtt_acks = { 0 };

static void ev_handler( struct mg_connection* nc, int ev, void* ev_data );
static void mqtt_connect( void );
static void mqtt_connected( void );
static void mqtt_disconnected( void );
static void mqtt_publish_internal( const char* topic, const char* message, int qos );
static void mqtt_worker_task( void* arg );
static void mqtt_schedule_reconnect( void );
static void mqtt_reconnect_timer_cb( void* arg );
static void mqtt_ping_timer_cb( void* arg );

static mqtt_subscription_t* find_subscription( const char* topic )
{
  if ( !topic )
  {
    return NULL;
  }

  for ( int i = 0; i < MAX_SUBSCRIPTIONS; i++ )
  {
    if ( mqtt_state.subscriptions[i].active
         && strcmp( mqtt_state.subscriptions[i].topic, topic ) == 0 )
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
  }
}

static void resubscribe_all( void )
{
  if ( !mqtt_state.nc )
  {
    return;
  }

  for ( int i = 0; i < MAX_SUBSCRIPTIONS; i++ )
  {
    if ( mqtt_state.subscriptions[i].active )
    {
      mg_mqtt_sub( mqtt_state.nc,
                   &(struct mg_mqtt_opts) {
                     .topic = mg_str( mqtt_state.subscriptions[i].topic ),
                     .qos = 0,
                   } );
    }
  }
}

static void mqtt_reconnect_timer_cb( void* arg )
{
  (void) arg;
  mqtt_state.reconnect_timer = NULL;

  if ( mqtt_state.initialized && !mqtt_state.connected )
  {
    osal_log_warning( "MQTT reconnect attempt" );
    mqtt_schedule_reconnect();
  }
}

static void mqtt_ping_timer_cb( void* arg )
{
  (void) arg;
  if ( mqtt_state.connected && mqtt_state.nc != NULL )
  {
    mg_mqtt_ping( mqtt_state.nc );
  }
}

static void puback_timer_callback( osal_timer_id_t timer_id )
{
  (void) timer_id;

  if ( !mqtt_acks.puback_received && mqtt_state.retries < RETRY_COUNT && mqtt_state.nc )
  {
    osal_log_warning( "MQTT PUBACK timeout, retries disabled in timer context" );
    mqtt_state.retries = RETRY_COUNT;
  }
  else if ( mqtt_state.retries >= RETRY_COUNT )
  {
    osal_log_error( "MQTT PUBACK retry limit reached" );
    (void) osal_bin_sem_give( mqtt_sync.puback );
    (void) osal_timer_stop( mqtt_timers.puback, 0 );
  }
}

static void suback_timer_callback( osal_timer_id_t timer_id )
{
  (void) timer_id;
  if ( !mqtt_acks.suback_received )
  {
    osal_log_warning( "MQTT SUBACK timeout topic=%s", mqtt_state.pending_subscribe_topic );
    (void) osal_bin_sem_give( mqtt_sync.suback );
  }
}

static void unsuback_timer_callback( osal_timer_id_t timer_id )
{
  (void) timer_id;
  if ( !mqtt_acks.unsuback_received )
  {
    osal_log_warning( "MQTT UNSUBACK timeout topic=%s", mqtt_state.pending_unsubscribe_topic );
    (void) osal_bin_sem_give( mqtt_sync.unsuback );
  }
}

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

  const char* effective_client_id = ( client_id && strlen( client_id ) > 0 ) ? client_id : "esp32_device";

  if ( !address || strlen( address ) == 0 )
  {
    osal_log_error( "MQTT address is empty" );
    return;
  }

  struct mg_mqtt_opts opts_con = {
    .user = mg_str( username ? username : "" ),
    .pass = mg_str( password ? password : "" ),
    .client_id = mg_str( effective_client_id ),
    .keepalive = 60,
    .clean = true,
  };

  osal_log_info( "MQTT connecting address=%s client_id=%s", address, effective_client_id );
  mqtt_state.nc = mg_mqtt_connect( &mgr, address, &opts_con, ev_handler, NULL );
  if ( mqtt_state.nc == NULL )
  {
    osal_log_error( "MQTT connection creation failed" );
    if ( mqtt_state.reconnect_timer == NULL )
    {
      mqtt_state.reconnect_timer = mg_timer_add( &mgr,
                                                 RECONNECT_DELAY_MS,
                                                 MG_TIMER_ONCE | MG_TIMER_AUTODELETE,
                                                 mqtt_reconnect_timer_cb,
                                                 NULL );
    }
  }
}

static void mqtt_connected( void )
{
  mqtt_state.connected = true;

  if ( mqtt_state.reconnect_timer != NULL )
  {
    mg_timer_free( &mgr.timers, mqtt_state.reconnect_timer );
    mqtt_state.reconnect_timer = NULL;
  }

  const char* address = MQTTConfig_GetString( MQTT_CONFIG_VALUE_ADDRESS );
  if ( address && mg_url_is_ssl( address ) )
  {
    const char* cert = MQTTConfig_GetCert( MQTT_CONFIG_VALUE_CERT );
    if ( cert && cert[0] != '\0' )
    {
      struct mg_tls_opts opts_ca = {
        .ca = mg_str( cert ),
        .name = mg_url_host( address ),
      };
      mg_tls_init( mqtt_state.nc, &opts_ca );
    }
  }

  osal_log_info( "MQTT connected" );
  resubscribe_all();

  if ( mqtt_state.ping_timer != NULL )
  {
    mg_timer_free( &mgr.timers, mqtt_state.ping_timer );
    mqtt_state.ping_timer = NULL;
  }
  mqtt_state.ping_timer = mg_timer_add( &mgr,
                                        PING_INTERVAL_MS,
                                        MG_TIMER_REPEAT,
                                        mqtt_ping_timer_cb,
                                        NULL );
}

static void mqtt_disconnected( void )
{
  mqtt_state.connected = false;
  osal_log_warning( "MQTT disconnected" );

  if ( mqtt_state.reconnect_timer != NULL )
  {
    mg_timer_free( &mgr.timers, mqtt_state.reconnect_timer );
    mqtt_state.reconnect_timer = NULL;
  }

  if ( mqtt_state.ping_timer != NULL )
  {
    mg_timer_free( &mgr.timers, mqtt_state.ping_timer );
    mqtt_state.ping_timer = NULL;
  }

  mqtt_state.reconnect_timer = mg_timer_add( &mgr,
                                             RECONNECT_DELAY_MS,
                                             MG_TIMER_ONCE | MG_TIMER_AUTODELETE,
                                             mqtt_reconnect_timer_cb,
                                             NULL );
}

static void mqtt_schedule_reconnect( void )
{
  mqtt_message_t msg = { 0 };
  msg.type = MQTT_WORKER_MSG_RECONNECT;
  (void) osal_queue_send( mqtt_sync.message_queue, &msg, 0 );
}

static void handle_mqtt_message( struct mg_mqtt_message* mm )
{
  char topic_str[129] = { 0 };
  size_t topic_len = mm->topic.len < sizeof( topic_str ) - 1 ? mm->topic.len : sizeof( topic_str ) - 1;
  size_t payload_preview_len = mm->data.len < 120 ? mm->data.len : 120;
  memcpy( topic_str, mm->topic.buf, topic_len );

  osal_log_info( "MQTT RX topic=%s payload_len=%u payload=%.*s",
                 topic_str,
                 (unsigned) mm->data.len,
                 (int) payload_preview_len,
                 mm->data.buf ? mm->data.buf : "" );

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
      mqtt_acks.suback_received = true;
      (void) osal_timer_stop( mqtt_timers.suback, 0 );
      (void) osal_bin_sem_give( mqtt_sync.suback );
      break;

    case MQTT_CMD_UNSUBACK:
      mqtt_acks.unsuback_received = true;
      (void) osal_timer_stop( mqtt_timers.unsuback, 0 );
      (void) osal_bin_sem_give( mqtt_sync.unsuback );
      break;

    case MQTT_CMD_PUBACK:
      mqtt_acks.puback_received = true;
      (void) osal_timer_stop( mqtt_timers.puback, 0 );
      (void) osal_bin_sem_give( mqtt_sync.puback );
      break;

    default:
      break;
  }
}

static void ev_handler( struct mg_connection* nc, int ev, void* ev_data )
{
  (void) nc;

  switch ( ev )
  {
    case MG_EV_CONNECT:
      mqtt_connected();
      break;

    case MG_EV_MQTT_CMD:
      handle_mqtt_command( (struct mg_mqtt_message*) ev_data );
      break;

    case MG_EV_MQTT_MSG:
      handle_mqtt_message( (struct mg_mqtt_message*) ev_data );
      break;

    case MG_EV_CLOSE:
      mqtt_disconnected();
      break;

    case MG_EV_ERROR:
      osal_log_error( "MQTT error: %s", (char*) ev_data );
      mqtt_disconnected();
      break;

    default:
      break;
  }
}

static void mqtt_publish_internal( const char* topic, const char* message, int qos )
{
  if ( !mqtt_state.nc || !mqtt_state.connected )
  {
    osal_log_warning( "MQTT publish skipped: disconnected" );
    return;
  }

  mqtt_acks.puback_received = false;
  memset( &mqtt_state.publish_opts, 0, sizeof( mqtt_state.publish_opts ) );

  strncpy( mqtt_state.publish_topic, topic, sizeof( mqtt_state.publish_topic ) - 1 );
  mqtt_state.publish_topic[sizeof( mqtt_state.publish_topic ) - 1] = '\0';
  strncpy( mqtt_state.publish_message, message, sizeof( mqtt_state.publish_message ) - 1 );
  mqtt_state.publish_message[sizeof( mqtt_state.publish_message ) - 1] = '\0';

  mqtt_state.publish_opts.qos = qos;
  mqtt_state.publish_opts.topic = mg_str( mqtt_state.publish_topic );
  mqtt_state.publish_opts.version = 4;
  mqtt_state.publish_opts.message = mg_str( mqtt_state.publish_message );
  mqtt_state.retries = 0;

  mg_mqtt_pub( mqtt_state.nc, &mqtt_state.publish_opts );

  if ( qos == 1 )
  {
    (void) osal_timer_start( mqtt_timers.puback, 0 );
    if ( osal_bin_sem_timed_wait( mqtt_sync.puback, TIMEOUT_DEFAULT_MS * RETRY_COUNT + 100 ) != OSAL_SUCCESS )
    {
      osal_log_error( "MQTT publish PUBACK timed out" );
    }
  }
}

static void mqtt_worker_task( void* arg )
{
  (void) arg;
  mqtt_message_t msg = { 0 };

  while ( 1 )
  {
    if ( osal_queue_receive( mqtt_sync.message_queue, &msg, OSAL_MAX_DELAY ) == OSAL_SUCCESS )
    {
      if ( msg.type == MQTT_WORKER_MSG_RECONNECT )
      {
        if ( !mqtt_state.connected )
        {
          mqtt_connect();
        }
      }
      else
      {
        mqtt_publish_internal( msg.topic, msg.message, msg.qos );
      }
    }
  }
}

static void config_update_callback( void )
{
  MqttApp_Deinit();
  MqttApp_Init();
}

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
    osal_log_error( "MQTT no free subscription slots" );
    return false;
  }

  strncpy( sub->topic, topic, sizeof( sub->topic ) - 1 );
  sub->topic[sizeof( sub->topic ) - 1] = '\0';
  sub->callback = callback;
  sub->active = true;

  strncpy( mqtt_state.pending_subscribe_topic,
           topic,
           sizeof( mqtt_state.pending_subscribe_topic ) - 1 );
  mqtt_state.pending_subscribe_topic[sizeof( mqtt_state.pending_subscribe_topic ) - 1] = '\0';

  mqtt_acks.suback_received = false;
  mg_mqtt_sub( mqtt_state.nc, &(struct mg_mqtt_opts) { .topic = mg_str( topic ), .qos = qos } );

  (void) osal_timer_change_period( mqtt_timers.suback, timeout_ms, 0 );
  (void) osal_timer_start( mqtt_timers.suback, 0 );

  if ( osal_bin_sem_timed_wait( mqtt_sync.suback, timeout_ms + 100 ) != OSAL_SUCCESS )
  {
    clear_subscription( sub );
    return false;
  }

  if ( !mqtt_acks.suback_received )
  {
    clear_subscription( sub );
    return false;
  }

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
    return false;
  }

  strncpy( mqtt_state.pending_unsubscribe_topic,
           topic,
           sizeof( mqtt_state.pending_unsubscribe_topic ) - 1 );
  mqtt_state.pending_unsubscribe_topic[sizeof( mqtt_state.pending_unsubscribe_topic ) - 1] = '\0';

  mqtt_acks.unsuback_received = false;

  struct mg_str topic_str = mg_str( topic );
  size_t packet_len = 2 + 2 + topic_str.len;

  mg_mqtt_send_header( mqtt_state.nc, MQTT_CMD_UNSUBSCRIBE, 0x02, packet_len );

  static uint16_t packet_id = 1;
  uint8_t id_bytes[2] = { (uint8_t) ( ( packet_id >> 8 ) & 0xFF ), (uint8_t) ( packet_id & 0xFF ) };
  mg_send( mqtt_state.nc, id_bytes, 2 );
  packet_id++;

  uint8_t topic_len_bytes[2] = {
    (uint8_t) ( ( topic_str.len >> 8 ) & 0xFF ),
    (uint8_t) ( topic_str.len & 0xFF ),
  };
  mg_send( mqtt_state.nc, topic_len_bytes, 2 );
  mg_send( mqtt_state.nc, topic_str.buf, topic_str.len );

  (void) osal_timer_change_period( mqtt_timers.unsuback, timeout_ms, 0 );
  (void) osal_timer_start( mqtt_timers.unsuback, 0 );

  if ( osal_bin_sem_timed_wait( mqtt_sync.unsuback, timeout_ms + 100 ) != OSAL_SUCCESS )
  {
    return false;
  }

  if ( !mqtt_acks.unsuback_received )
  {
    return false;
  }

  clear_subscription( sub );
  return true;
}

void MqttApp_Init( void )
{
  if ( mqtt_state.initialized )
  {
    return;
  }

  MongooseProcess_Init();
  MQTTConfig_Init();
  MQTTConfig_SetCallback( config_update_callback );

  if ( osal_timer_create( &mqtt_timers.puback, "mqtt_puback", TIMEOUT_DEFAULT_MS, true, puback_timer_callback, NULL, NULL, 0 ) != OSAL_SUCCESS
      || osal_timer_create( &mqtt_timers.suback, "mqtt_suback", TIMEOUT_DEFAULT_MS, false, suback_timer_callback, NULL, NULL, 0 ) != OSAL_SUCCESS
       || osal_timer_create( &mqtt_timers.unsuback, "mqtt_unsuback", TIMEOUT_DEFAULT_MS, false, unsuback_timer_callback, NULL, NULL, 0 ) != OSAL_SUCCESS )
  {
    osal_log_error( "MQTT timer creation failed" );
    return;
  }

  if ( osal_queue_create( &mqtt_sync.message_queue, "mqtt_msg_q", MESSAGE_QUEUE_SIZE, sizeof( mqtt_message_t ) ) != OSAL_SUCCESS
       || osal_bin_sem_create( &mqtt_sync.puback, "mqtt_puback_sem", OSAL_SEM_EMPTY ) != OSAL_SUCCESS
       || osal_bin_sem_create( &mqtt_sync.suback, "mqtt_suback_sem", OSAL_SEM_EMPTY ) != OSAL_SUCCESS
       || osal_bin_sem_create( &mqtt_sync.unsuback, "mqtt_unsuback_sem", OSAL_SEM_EMPTY ) != OSAL_SUCCESS )
  {
    osal_log_error( "MQTT sync object creation failed" );
    return;
  }

  memset( mqtt_state.subscriptions, 0, sizeof( mqtt_state.subscriptions ) );
  memset( &mqtt_acks, 0, sizeof( mqtt_acks ) );

  if ( osal_task_create( &mqtt_state.worker_task_id,
                         "mqtt_worker",
                         mqtt_worker_task,
                         NULL,
                         NULL,
                         MONGOOSE_TASK_STACK_SIZE,
                         MONGOOSE_TASK_PRIORITY,
                         NULL ) != OSAL_SUCCESS )
  {
    osal_log_error( "MQTT worker task creation failed" );
    return;
  }

  mqtt_connect();
  mqtt_state.initialized = true;
}

void MqttApp_Deinit( void )
{
  if ( !mqtt_state.initialized )
  {
    return;
  }

  (void) osal_task_delete( mqtt_state.worker_task_id );

  (void) osal_timer_delete( mqtt_timers.suback, 0 );
  (void) osal_timer_delete( mqtt_timers.unsuback, 0 );
  (void) osal_timer_delete( mqtt_timers.puback, 0 );

  if ( mqtt_state.reconnect_timer != NULL )
  {
    mg_timer_free( &mgr.timers, mqtt_state.reconnect_timer );
    mqtt_state.reconnect_timer = NULL;
  }

  if ( mqtt_state.ping_timer != NULL )
  {
    mg_timer_free( &mgr.timers, mqtt_state.ping_timer );
    mqtt_state.ping_timer = NULL;
  }

  (void) osal_queue_delete( mqtt_sync.message_queue );
  (void) osal_bin_sem_delete( mqtt_sync.puback );
  (void) osal_bin_sem_delete( mqtt_sync.suback );
  (void) osal_bin_sem_delete( mqtt_sync.unsuback );

  if ( mqtt_state.connected && mqtt_state.nc )
  {
    mg_mqtt_disconnect( mqtt_state.nc, NULL );
    mqtt_state.nc->is_closing = 1;
    mqtt_state.nc = NULL;
  }

  memset( &mqtt_state, 0, sizeof( mqtt_state ) );
  memset( &mqtt_timers, 0, sizeof( mqtt_timers ) );
  memset( &mqtt_sync, 0, sizeof( mqtt_sync ) );
  memset( &mqtt_acks, 0, sizeof( mqtt_acks ) );
}

bool MqttApp_PostData( const char* topic, const char* message, int qos )
{
  if ( !mqtt_state.initialized || !topic || !message )
  {
    return false;
  }

  mqtt_message_t msg = { 0 };

  if ( strlen( topic ) >= sizeof( msg.topic ) || strlen( message ) >= sizeof( msg.message ) )
  {
    return false;
  }

  msg.type = MQTT_WORKER_MSG_PUBLISH;
  strncpy( msg.topic, topic, sizeof( msg.topic ) - 1 );
  strncpy( msg.message, message, sizeof( msg.message ) - 1 );
  msg.topic[sizeof( msg.topic ) - 1] = '\0';
  msg.message[sizeof( msg.message ) - 1] = '\0';
  msg.qos = qos;

  return osal_queue_send( mqtt_sync.message_queue, &msg, 0 ) == OSAL_SUCCESS;
}

bool MqttApp_IsConnected( void )
{
  return mqtt_state.initialized && mqtt_state.connected;
}