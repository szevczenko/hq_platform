#include "mqtt_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mongoose.h"
#include "mongoose_process.h"
#include "mqtt_config.h"
#include "osal_bin_sem.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_queue.h"
#include "osal_task.h"
#include "osal_timer.h"

#define RETRY_COUNT 3
#define MAX_SUBSCRIPTIONS 10
#define TIMEOUT_DEFAULT_MS 5000
#define RECONNECT_DELAY_MS 30000
#define PING_INTERVAL_MS 30000
#define MESSAGE_QUEUE_SIZE 6
#define MONGOOSE_TASK_PRIORITY 5
#define MQTT_WORKER_STACK_SIZE (OSAL_TASK_MIN_STACK_SIZE * 4)
#define MQTT_APP_TOPIC_MAX_LEN 256
#define MQTT_APP_MESSAGE_MAX_LEN 512

typedef enum {
	MQTT_WORKER_MSG_PUBLISH = 0,
	MQTT_WORKER_MSG_CONNECT,
	MQTT_WORKER_MSG_DISCONNECT,
	MQTT_WORKER_MSG_PING,
	MQTT_WORKER_MSG_PUBACK_TIMEOUT,
	MQTT_WORKER_MSG_SUBACK_TIMEOUT,
	MQTT_WORKER_MSG_UNSUBACK_TIMEOUT
} mqtt_worker_msg_type_t;

typedef struct {
	mqtt_worker_msg_type_t type;
	char topic[MQTT_APP_TOPIC_MAX_LEN];
	char message[MQTT_APP_MESSAGE_MAX_LEN];
	int qos;
} mqtt_message_t;

typedef struct {
	char topic[MQTT_APP_TOPIC_MAX_LEN];
	mqtt_message_callback_t callback;
	bool active;
} mqtt_subscription_t;

typedef struct {
	bool initialized;
	bool connected;
	struct mg_connection *nc;
	mqtt_subscription_t subscriptions[MAX_SUBSCRIPTIONS];
	char pending_subscribe_topic[MQTT_APP_TOPIC_MAX_LEN];
	char pending_unsubscribe_topic[MQTT_APP_TOPIC_MAX_LEN];
	char publish_topic[MQTT_APP_TOPIC_MAX_LEN];
	char publish_message[MQTT_APP_MESSAGE_MAX_LEN];
	int retries;
	struct mg_mqtt_opts publish_opts;
	bool reconnect_enabled;
	osal_task_id_t worker_task_id;
} mqtt_state_t;

typedef struct {
	osal_timer_id_t puback;
	osal_timer_id_t suback;
	osal_timer_id_t unsuback;
	osal_timer_id_t reconnect;
	osal_timer_id_t ping;
} mqtt_timers_t;

typedef struct {
	osal_queue_id_t message_queue;
	osal_bin_sem_id_t puback;
	osal_bin_sem_id_t suback;
	osal_bin_sem_id_t unsuback;
	osal_mutex_id_t subscriptions_lock;
} mqtt_sync_t;

typedef struct {
	bool puback_received;
	bool suback_received;
	bool unsuback_received;
} mqtt_ack_flags_t;

static mqtt_state_t mqtt_state = { 0 };
static mqtt_timers_t mqtt_timers = { 0 };
static mqtt_sync_t mqtt_sync = { 0 };
static mqtt_ack_flags_t mqtt_acks = { 0 };

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data);
static void mqtt_connect(void);
static void mqtt_connected(void);
static void mqtt_disconnected(void);
static void mqtt_publish_internal(const char *topic, const char *message,
				  int qos);
static void mqtt_worker_task(void *arg);
static void mqtt_send_worker_msg(mqtt_worker_msg_type_t type);
static void mqtt_reset_runtime_timers(void);
static void mqtt_disconnect_internal(void);
static void reconnect_timer_cb(osal_timer_id_t timer_id);
static void ping_timer_cb(osal_timer_id_t timer_id);
static void setup_tls(struct mg_connection *nc, const char *address);
static void schedule_reconnect(void);
static bool create_timers(void);
static void destroy_timers(void);
static bool create_sync_objects(void);
static void destroy_sync_objects(void);

static mqtt_subscription_t *find_subscription(const char *topic)
{
	if (!topic) {
		return NULL;
	}

	for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
		if (mqtt_state.subscriptions[i].active &&
		    strcmp(mqtt_state.subscriptions[i].topic, topic) == 0) {
			return &mqtt_state.subscriptions[i];
		}
	}

	return NULL;
}

static mqtt_subscription_t *find_free_subscription_slot(void)
{
	for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
		if (!mqtt_state.subscriptions[i].active) {
			return &mqtt_state.subscriptions[i];
		}
	}
	return NULL;
}

static void clear_subscription(mqtt_subscription_t *sub)
{
	if (sub) {
		memset(sub->topic, 0, sizeof(sub->topic));
		sub->callback = NULL;
		sub->active = false;
	}
}

static void resubscribe_all(void)
{
	if (!mqtt_state.nc) {
		return;
	}

	for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
		if (mqtt_state.subscriptions[i].active) {
			mg_mqtt_sub(mqtt_state.nc,
				    &(struct mg_mqtt_opts){
					    .topic = mg_str(
						    mqtt_state.subscriptions[i]
							    .topic),
					    .qos = 0,
				    });
		}
	}
}

static void reconnect_timer_cb(osal_timer_id_t timer_id)
{
	(void)timer_id;
	mqtt_send_worker_msg(MQTT_WORKER_MSG_CONNECT);
}

static void ping_timer_cb(osal_timer_id_t timer_id)
{
	(void)timer_id;
	mqtt_send_worker_msg(MQTT_WORKER_MSG_PING);
}

static void puback_timer_callback(osal_timer_id_t timer_id)
{
	(void)timer_id;
	mqtt_send_worker_msg(MQTT_WORKER_MSG_PUBACK_TIMEOUT);
}

static void suback_timer_callback(osal_timer_id_t timer_id)
{
	(void)timer_id;
	mqtt_send_worker_msg(MQTT_WORKER_MSG_SUBACK_TIMEOUT);
}

static void unsuback_timer_callback(osal_timer_id_t timer_id)
{
	(void)timer_id;
	mqtt_send_worker_msg(MQTT_WORKER_MSG_UNSUBACK_TIMEOUT);
}

static void schedule_reconnect(void)
{
	(void)osal_timer_stop(mqtt_timers.reconnect, 0);
	(void)osal_timer_start(mqtt_timers.reconnect, 0);
}

static void setup_tls(struct mg_connection *nc, const char *address)
{
	struct mg_tls_opts tls;
	const char *ca, *cert, *key;
	bool skip = false;

	memset(&tls, 0, sizeof(tls));
	(void)mqtt_config_get_bool(&skip, MQTT_CONFIG_VALUE_SKIP_VERIFY);
	tls.skip_verification = skip ? 1 : 0;

	ca   = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CERT);
	cert = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CLIENT_CERT);
	key  = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CLIENT_KEY);

	if (!skip && ca && ca[0] != '\0')
		tls.ca = mg_str(ca);
	if (!skip)
		tls.name = mg_url_host(address);
	if (cert && cert[0] != '\0')
		tls.cert = mg_str(cert);
	if (key && key[0] != '\0')
		tls.key = mg_str(key);

	mg_tls_init(nc, &tls);
}

static void mqtt_connect(void)
{
	const char *address, *username, *password, *client_id;
	struct mg_mqtt_opts opts;

	address   = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
	username  = mqtt_config_get_string(MQTT_CONFIG_VALUE_USERNAME);
	password  = mqtt_config_get_string(MQTT_CONFIG_VALUE_PASSWORD);
	client_id = mqtt_config_get_string(MQTT_CONFIG_VALUE_CLIENT_ID);

	if (!address || address[0] == '\0') {
		osal_log_error("MQTT address is empty");
		return;
	}

	if (mqtt_state.nc != NULL)
		mqtt_state.nc->is_closing = 1;

	memset(&opts, 0, sizeof(opts));
	opts.user      = mg_str(username ? username : "");
	opts.pass      = mg_str(password ? password : "");
	opts.client_id = mg_str(client_id && client_id[0] ? client_id : "hq_");
	opts.keepalive = 60;
	opts.clean     = true;

	osal_log_info("MQTT connecting address=%s client_id=%s", address,
		      opts.client_id.buf);

	mqtt_state.nc = mg_mqtt_connect(&mgr, address, &opts, ev_handler, NULL);
	if (!mqtt_state.nc) {
		osal_log_error("MQTT connection creation failed");
		schedule_reconnect();
		return;
	}
}

static void mqtt_connected(void)
{
	if (!mqtt_state.initialized)
		return;

	mqtt_state.connected = true;
	mqtt_state.reconnect_enabled = true;
	(void)osal_timer_stop(mqtt_timers.reconnect, 0);

	osal_log_info("MQTT connected");
	resubscribe_all();

	(void)osal_timer_stop(mqtt_timers.ping, 0);
	(void)osal_timer_start(mqtt_timers.ping, 0);
}

static void mqtt_disconnected(void)
{
	mqtt_state.connected = false;
	mqtt_state.nc        = NULL;
	osal_log_warning("MQTT disconnected");

	(void)osal_timer_stop(mqtt_timers.reconnect, 0);
	(void)osal_timer_stop(mqtt_timers.ping, 0);

	if (mqtt_state.reconnect_enabled)
		schedule_reconnect();
}

static void mqtt_send_worker_msg(mqtt_worker_msg_type_t type)
{
	mqtt_message_t msg = { 0 };
	msg.type = type;
	(void)osal_queue_send(mqtt_sync.message_queue, &msg, 0);
}

static void mqtt_reset_runtime_timers(void)
{
	(void)osal_timer_stop(mqtt_timers.puback, 0);
	(void)osal_timer_stop(mqtt_timers.suback, 0);
	(void)osal_timer_stop(mqtt_timers.unsuback, 0);
	(void)osal_timer_stop(mqtt_timers.reconnect, 0);
	(void)osal_timer_stop(mqtt_timers.ping, 0);
}

static void mqtt_disconnect_internal(void)
{
	mqtt_state.reconnect_enabled = false;
	mqtt_reset_runtime_timers();

	if (mqtt_state.nc != NULL) {
		mg_mqtt_disconnect(mqtt_state.nc, NULL);
		mqtt_state.nc->is_closing = 1;
		mqtt_state.nc = NULL;
	}

	mqtt_state.connected = false;
}

static void handle_mqtt_message(struct mg_mqtt_message *mm)
{
	char topic_str[129] = { 0 };
	size_t topic_len = mm->topic.len < sizeof(topic_str) - 1 ?
				   mm->topic.len :
				   sizeof(topic_str) - 1;
	size_t payload_preview_len = mm->data.len < 120 ? mm->data.len : 120;
	memcpy(topic_str, mm->topic.buf, topic_len);

	osal_log_info("MQTT RX topic=%s payload_len=%u payload=%.*s", topic_str,
		      (unsigned)mm->data.len, (int)payload_preview_len,
		      mm->data.buf ? mm->data.buf : "");

	for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
		if (mqtt_state.subscriptions[i].active &&
		    mqtt_state.subscriptions[i].callback) {
			if (mg_match(mg_str(topic_str),
				     mg_str(mqtt_state.subscriptions[i].topic),
				     NULL)) {
				mqtt_state.subscriptions[i].callback(
					topic_str, mm->data.buf, mm->data.len);
				break;
			}
		}
	}
}

static void handle_mqtt_command(struct mg_mqtt_message *mm)
{
	switch (mm->cmd) {
	case MQTT_CMD_CONNACK:
		if (mm->ack == 0) {
			mqtt_connected();
		} else {
			osal_log_error("MQTT CONNACK rejected ack=%u", (unsigned)mm->ack);
			if (mqtt_state.nc != NULL)
				mqtt_state.nc->is_closing = 1;
		}
		break;

	case MQTT_CMD_SUBACK:
		mqtt_acks.suback_received = true;
		(void)osal_timer_stop(mqtt_timers.suback, 0);
		(void)osal_bin_sem_give(mqtt_sync.suback);
		break;

	case MQTT_CMD_UNSUBACK:
		mqtt_acks.unsuback_received = true;
		(void)osal_timer_stop(mqtt_timers.unsuback, 0);
		(void)osal_bin_sem_give(mqtt_sync.unsuback);
		break;

	case MQTT_CMD_PUBACK:
		mqtt_acks.puback_received = true;
		(void)osal_timer_stop(mqtt_timers.puback, 0);
		(void)osal_bin_sem_give(mqtt_sync.puback);
		break;

	default:
		break;
	}
}

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data)
{
	/*
	 * Ignore events from stale connections.  When mqtt_disconnect_internal()
	 * marks the old connection for closure, the deferred MG_EV_CLOSE fires
	 * from the poll task after a new connection may already have been
	 * created.  Guard against clobbering mqtt_state.nc with the stale event.
	 */
	if (nc != mqtt_state.nc)
		return;

	switch (ev) {
	case MG_EV_CONNECT: {
		osal_log_info("ev_handler: CONNECT nc=%p", (void *)nc);
		const char *addr =
			mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
		if (addr && mg_url_is_ssl(addr))
			setup_tls(nc, addr);
		break;
	}

	case MG_EV_MQTT_CMD:
		osal_log_info("ev_handler: MQTT_CMD cmd=%d nc=%p",
			      ((struct mg_mqtt_message *)ev_data)->cmd,
			      (void *)nc);
		handle_mqtt_command((struct mg_mqtt_message *)ev_data);
		break;

	case MG_EV_MQTT_MSG:
		osal_log_info("ev_handler: MQTT_MSG nc=%p", (void *)nc);
		handle_mqtt_message((struct mg_mqtt_message *)ev_data);
		break;

	case MG_EV_CLOSE:
		osal_log_info("ev_handler: CLOSE nc=%p", (void *)nc);
		mqtt_disconnected();
		break;

	case MG_EV_ERROR:
		osal_log_error("ev_handler: ERROR nc=%p err=%s",
			       (void *)nc, (char *)ev_data);
		mqtt_disconnected();
		break;

	default:
		break;
	}
}

static void mqtt_publish_internal(const char *topic, const char *message,
				  int qos)
{
	if (!mqtt_state.nc || !mqtt_state.connected) {
		osal_log_warning("MQTT publish skipped: disconnected");
		return;
	}

	mqtt_acks.puback_received = false;
	memset(&mqtt_state.publish_opts, 0, sizeof(mqtt_state.publish_opts));

	strncpy(mqtt_state.publish_topic, topic,
		sizeof(mqtt_state.publish_topic) - 1);
	mqtt_state.publish_topic[sizeof(mqtt_state.publish_topic) - 1] = '\0';
	strncpy(mqtt_state.publish_message, message,
		sizeof(mqtt_state.publish_message) - 1);
	mqtt_state.publish_message[sizeof(mqtt_state.publish_message) - 1] = '\0';

	mqtt_state.publish_opts.qos     = qos;
	mqtt_state.publish_opts.topic   = mg_str(mqtt_state.publish_topic);
	mqtt_state.publish_opts.version = 4;
	mqtt_state.publish_opts.message = mg_str(mqtt_state.publish_message);
	mqtt_state.retries              = 0;

	mg_mqtt_pub(mqtt_state.nc, &mqtt_state.publish_opts);

	if (qos != 1)
		return;

	(void)osal_timer_start(mqtt_timers.puback, 0);
	if (osal_bin_sem_timed_wait(mqtt_sync.puback,
				    TIMEOUT_DEFAULT_MS * RETRY_COUNT + 100) !=
	    OSAL_SUCCESS)
		osal_log_error("MQTT publish PUBACK timed out");
}

static void worker_handle_publish(const mqtt_message_t *msg)
{
	mqtt_publish_internal(msg->topic, msg->message, msg->qos);
}

static void worker_handle_connect(void)
{
	if (mqtt_state.initialized && !mqtt_state.connected) {
		mqtt_state.reconnect_enabled = true;
		mqtt_connect();
	}
}

static void worker_handle_disconnect(void)
{
	mqtt_disconnect_internal();
}

static void worker_handle_ping(void)
{
	if (mqtt_state.connected && mqtt_state.nc != NULL)
		mg_mqtt_ping(mqtt_state.nc);
}

static void worker_handle_puback_timeout(void)
{
	if (mqtt_acks.puback_received)
		return;

	if (mqtt_state.retries < RETRY_COUNT && mqtt_state.nc) {
		osal_log_warning("MQTT PUBACK timeout, retries disabled");
		mqtt_state.retries = RETRY_COUNT;
		return;
	}

	osal_log_error("MQTT PUBACK retry limit reached");
	(void)osal_bin_sem_give(mqtt_sync.puback);
	(void)osal_timer_stop(mqtt_timers.puback, 0);
}

static void worker_handle_suback_timeout(void)
{
	if (mqtt_acks.suback_received)
		return;
	osal_log_warning("MQTT SUBACK timeout topic=%s",
			 mqtt_state.pending_subscribe_topic);
	(void)osal_bin_sem_give(mqtt_sync.suback);
}

static void worker_handle_unsuback_timeout(void)
{
	if (mqtt_acks.unsuback_received)
		return;
	osal_log_warning("MQTT UNSUBACK timeout topic=%s",
			 mqtt_state.pending_unsubscribe_topic);
	(void)osal_bin_sem_give(mqtt_sync.unsuback);
}

static void dispatch_worker_msg(const mqtt_message_t *msg)
{
	osal_log_info("worker: dispatch type=%d", (int)msg->type);
	switch (msg->type) {
	case MQTT_WORKER_MSG_PUBLISH:
		worker_handle_publish(msg);
		break;
	case MQTT_WORKER_MSG_CONNECT:
		worker_handle_connect();
		break;
	case MQTT_WORKER_MSG_DISCONNECT:
		worker_handle_disconnect();
		break;
	case MQTT_WORKER_MSG_PING:
		worker_handle_ping();
		break;
	case MQTT_WORKER_MSG_PUBACK_TIMEOUT:
		worker_handle_puback_timeout();
		break;
	case MQTT_WORKER_MSG_SUBACK_TIMEOUT:
		worker_handle_suback_timeout();
		break;
	case MQTT_WORKER_MSG_UNSUBACK_TIMEOUT:
		worker_handle_unsuback_timeout();
		break;
	default:
		break;
	}
}

static void mqtt_worker_task(void *arg)
{
	mqtt_message_t msg;

	(void)arg;

	while (1) {
		if (osal_queue_receive(mqtt_sync.message_queue, &msg,
				       OSAL_MAX_DELAY) == OSAL_SUCCESS)
			dispatch_worker_msg(&msg);
	}
}

static void config_update_callback(void)
{
	if (!mqtt_state.initialized)
		return;

	mqtt_send_worker_msg(MQTT_WORKER_MSG_DISCONNECT);
	mqtt_send_worker_msg(MQTT_WORKER_MSG_CONNECT);
}

static bool create_timers(void)
{
	if (osal_timer_create(&mqtt_timers.puback, "mqtt_puback",
			      TIMEOUT_DEFAULT_MS, true, puback_timer_callback,
			      NULL, NULL, 0) != OSAL_SUCCESS)
		return false;
	if (osal_timer_create(&mqtt_timers.suback, "mqtt_suback",
			      TIMEOUT_DEFAULT_MS, false, suback_timer_callback,
			      NULL, NULL, 0) != OSAL_SUCCESS)
		return false;
	if (osal_timer_create(&mqtt_timers.unsuback, "mqtt_unsuback",
			      TIMEOUT_DEFAULT_MS, false, unsuback_timer_callback,
			      NULL, NULL, 0) != OSAL_SUCCESS)
		return false;
	if (osal_timer_create(&mqtt_timers.reconnect, "mqtt_reconnect",
			      RECONNECT_DELAY_MS, false, reconnect_timer_cb,
			      NULL, NULL, 0) != OSAL_SUCCESS)
		return false;
	if (osal_timer_create(&mqtt_timers.ping, "mqtt_ping",
			      PING_INTERVAL_MS, true, ping_timer_cb,
			      NULL, NULL, 0) != OSAL_SUCCESS)
		return false;
	return true;
}

static void destroy_timers(void)
{
	(void)osal_timer_delete(mqtt_timers.puback, 0);
	(void)osal_timer_delete(mqtt_timers.suback, 0);
	(void)osal_timer_delete(mqtt_timers.unsuback, 0);
	(void)osal_timer_delete(mqtt_timers.reconnect, 0);
	(void)osal_timer_delete(mqtt_timers.ping, 0);
	memset(&mqtt_timers, 0, sizeof(mqtt_timers));
}

static bool create_sync_objects(void)
{
	if (osal_queue_create(&mqtt_sync.message_queue, "mqtt_msg_q",
			      MESSAGE_QUEUE_SIZE,
			      sizeof(mqtt_message_t)) != OSAL_SUCCESS)
		return false;
	if (osal_bin_sem_create(&mqtt_sync.puback, "mqtt_puback_sem",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		return false;
	if (osal_bin_sem_create(&mqtt_sync.suback, "mqtt_suback_sem",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		return false;
	if (osal_bin_sem_create(&mqtt_sync.unsuback, "mqtt_unsuback_sem",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		return false;
	if (osal_mutex_create(&mqtt_sync.subscriptions_lock,
			      "mqtt_subscriptions_lock") != OSAL_SUCCESS)
		return false;
	return true;
}

static void destroy_sync_objects(void)
{
	(void)osal_queue_delete(mqtt_sync.message_queue);
	(void)osal_bin_sem_delete(mqtt_sync.puback);
	(void)osal_bin_sem_delete(mqtt_sync.suback);
	(void)osal_bin_sem_delete(mqtt_sync.unsuback);
	(void)osal_mutex_delete(mqtt_sync.subscriptions_lock);
	memset(&mqtt_sync, 0, sizeof(mqtt_sync));
}

void mqtt_app_init(void)
{
	if (mqtt_state.initialized)
		return;

	mqtt_config_set_callback(config_update_callback);

	if (!create_timers()) {
		osal_log_error("MQTT timer creation failed");
		goto err_timers;
	}

	if (!create_sync_objects()) {
		osal_log_error("MQTT sync object creation failed");
		goto err_sync;
	}

	memset(mqtt_state.subscriptions, 0, sizeof(mqtt_state.subscriptions));
	memset(&mqtt_acks, 0, sizeof(mqtt_acks));
	mqtt_state.reconnect_enabled = true;

	if (osal_task_create(&mqtt_state.worker_task_id, "mqtt_worker",
			     mqtt_worker_task, NULL, NULL,
			     MQTT_WORKER_STACK_SIZE, MONGOOSE_TASK_PRIORITY,
			     NULL) != OSAL_SUCCESS) {
		osal_log_error("MQTT worker task creation failed");
		goto err_task;
	}

	mqtt_connect();
	mqtt_state.initialized = true;
	return;

err_task:
	destroy_sync_objects();
err_sync:
	destroy_timers();
err_timers:
	mqtt_config_set_callback(NULL);
}

void mqtt_app_deinit(void)
{
	if (!mqtt_state.initialized)
		return;

	mqtt_state.initialized = false;
	mqtt_config_set_callback(NULL);
	mqtt_disconnect_internal();
	(void)osal_task_delete(mqtt_state.worker_task_id);
	destroy_timers();
	destroy_sync_objects();
	memset(&mqtt_state, 0, sizeof(mqtt_state));
	memset(&mqtt_acks, 0, sizeof(mqtt_acks));
}

bool mqtt_app_subscribe(const char *topic, int qos,
			mqtt_message_callback_t callback, uint32_t timeout_ms)
{
	osal_status_t lock_rc;

	if (!mqtt_state.initialized || !mqtt_state.connected || !topic ||
	    !callback) {
		return false;
	}

	lock_rc = osal_mutex_take(mqtt_sync.subscriptions_lock);
	if (lock_rc != OSAL_SUCCESS) {
		return false;
	}

	mqtt_subscription_t *existing = find_subscription(topic);
	if (existing) {
		existing->callback = callback;
		(void)osal_mutex_give(mqtt_sync.subscriptions_lock);
		return true;
	}

	mqtt_subscription_t *sub = find_free_subscription_slot();
	if (!sub) {
		osal_log_error("MQTT no free subscription slots");
		(void)osal_mutex_give(mqtt_sync.subscriptions_lock);
		return false;
	}

	strncpy(sub->topic, topic, sizeof(sub->topic) - 1);
	sub->topic[sizeof(sub->topic) - 1] = '\0';
	sub->callback = callback;
	sub->active = true;

	strncpy(mqtt_state.pending_subscribe_topic, topic,
		sizeof(mqtt_state.pending_subscribe_topic) - 1);
	mqtt_state.pending_subscribe_topic
		[sizeof(mqtt_state.pending_subscribe_topic) - 1] = '\0';

	mqtt_acks.suback_received = false;
	mg_mqtt_sub(mqtt_state.nc,
		    &(struct mg_mqtt_opts){ .topic = mg_str(topic),
					    .qos = qos });

	(void)osal_timer_change_period(mqtt_timers.suback, timeout_ms, 0);
	(void)osal_timer_start(mqtt_timers.suback, 0);

	if (osal_bin_sem_timed_wait(mqtt_sync.suback, timeout_ms + 100) !=
	    OSAL_SUCCESS || !mqtt_acks.suback_received) {
		clear_subscription(sub);
		(void)osal_mutex_give(mqtt_sync.subscriptions_lock);
		return false;
	}

	(void)osal_mutex_give(mqtt_sync.subscriptions_lock);
	return true;
}

bool mqtt_app_unsubscribe(const char *topic, uint32_t timeout_ms)
{
	osal_status_t lock_rc;

	if (!mqtt_state.initialized || !mqtt_state.connected || !topic) {
		return false;
	}

	lock_rc = osal_mutex_take(mqtt_sync.subscriptions_lock);
	if (lock_rc != OSAL_SUCCESS) {
		return false;
	}

	mqtt_subscription_t *sub = find_subscription(topic);
	if (!sub) {
		(void)osal_mutex_give(mqtt_sync.subscriptions_lock);
		return false;
	}

	strncpy(mqtt_state.pending_unsubscribe_topic, topic,
		sizeof(mqtt_state.pending_unsubscribe_topic) - 1);
	mqtt_state.pending_unsubscribe_topic
		[sizeof(mqtt_state.pending_unsubscribe_topic) - 1] = '\0';

	mqtt_acks.unsuback_received = false;
	mg_mqtt_unsub(mqtt_state.nc,
		      &(struct mg_mqtt_opts){ .topic = mg_str(topic) });

	(void)osal_timer_change_period(mqtt_timers.unsuback, timeout_ms, 0);
	(void)osal_timer_start(mqtt_timers.unsuback, 0);

	if (osal_bin_sem_timed_wait(mqtt_sync.unsuback, timeout_ms + 100) !=
	    OSAL_SUCCESS || !mqtt_acks.unsuback_received) {
		(void)osal_mutex_give(mqtt_sync.subscriptions_lock);
		return false;
	}

	clear_subscription(sub);
	(void)osal_mutex_give(mqtt_sync.subscriptions_lock);
	return true;
}

bool mqtt_app_post_data(const char *topic, const char *message, int qos)
{
	if (!mqtt_state.initialized || !topic || !message) {
		return false;
	}

	mqtt_message_t msg = { 0 };

	if (strlen(topic) >= sizeof(msg.topic) ||
	    strlen(message) >= sizeof(msg.message)) {
		return false;
	}

	msg.type = MQTT_WORKER_MSG_PUBLISH;
	strncpy(msg.topic, topic, sizeof(msg.topic) - 1);
	strncpy(msg.message, message, sizeof(msg.message) - 1);
	msg.topic[sizeof(msg.topic) - 1] = '\0';
	msg.message[sizeof(msg.message) - 1] = '\0';
	msg.qos = qos;

	return osal_queue_send(mqtt_sync.message_queue, &msg, 0) ==
	       OSAL_SUCCESS;
}

bool mqtt_app_is_connected(void)
{
	return mqtt_state.initialized && mqtt_state.connected;
}