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
#include "osal_timer.h"

#define RETRY_COUNT 3
#define MAX_SUBSCRIPTIONS 10
#define TIMEOUT_DEFAULT_MS 5000
#define RECONNECT_DELAY_MS 30000
#define PING_INTERVAL_MS 30000
#define COMMAND_QUEUE_SIZE 8
#define MQTT_APP_TOPIC_MAX_LEN 256
#define MQTT_APP_MESSAGE_MAX_LEN 512

/* Command types for the wakeup queue */
typedef enum {
	MQTT_CMD_TYPE_CONNECT = 0,
	MQTT_CMD_TYPE_DISCONNECT,
	MQTT_CMD_TYPE_APPLY_CONFIG,
	MQTT_CMD_TYPE_PUBLISH,
	MQTT_CMD_TYPE_SUBSCRIBE,
	MQTT_CMD_TYPE_UNSUBSCRIBE,
	MQTT_CMD_TYPE_PING,
	MQTT_CMD_TYPE_PUBACK_TIMEOUT,
	MQTT_CMD_TYPE_SUBACK_TIMEOUT,
	MQTT_CMD_TYPE_UNSUBACK_TIMEOUT,
	MQTT_CMD_TYPE_SHUTDOWN
} mqtt_cmd_type_t;

typedef struct {
	mqtt_cmd_type_t type;
	char topic[MQTT_APP_TOPIC_MAX_LEN];
	char message[MQTT_APP_MESSAGE_MAX_LEN];
	int qos;
} mqtt_cmd_t;

typedef struct {
	char topic[MQTT_APP_TOPIC_MAX_LEN];
	mqtt_message_callback_t callback;
	bool active;
} mqtt_subscription_t;

typedef struct {
	bool initialized;
	bool connected;
	struct mg_connection *nc;
	struct mg_connection *control_nc;
	unsigned long control_conn_id;
	mqtt_subscription_t subscriptions[MAX_SUBSCRIPTIONS];
	char pending_subscribe_topic[MQTT_APP_TOPIC_MAX_LEN];
	char pending_unsubscribe_topic[MQTT_APP_TOPIC_MAX_LEN];
	char publish_topic[MQTT_APP_TOPIC_MAX_LEN];
	char publish_message[MQTT_APP_MESSAGE_MAX_LEN];
	int retries;
	struct mg_mqtt_opts publish_opts;
	bool reconnect_enabled;
	bool shutdown_requested;
} mqtt_state_t;

typedef struct {
	osal_timer_id_t puback;
	osal_timer_id_t suback;
	osal_timer_id_t unsuback;
	osal_timer_id_t reconnect;
	osal_timer_id_t ping;
} mqtt_timers_t;

typedef struct {
	osal_queue_id_t cmd_queue;
	osal_bin_sem_id_t puback;
	osal_bin_sem_id_t suback;
	osal_bin_sem_id_t unsuback;
	osal_bin_sem_id_t shutdown_sem;
	osal_bin_sem_id_t init_sem;
	osal_mutex_id_t subscriptions_lock;
} mqtt_sync_t;

typedef struct {
	bool puback_received;
	bool suback_received;
	bool unsuback_received;
} mqtt_ack_flags_t;

typedef struct {
	mqtt_connect_callback_t connect_cb;
	mqtt_disconnect_callback_t disconnect_cb;
} mqtt_callbacks_t;

static mqtt_state_t mqtt_state = { 0 };
static mqtt_timers_t mqtt_timers = { 0 };
static mqtt_sync_t mqtt_sync = { 0 };
static mqtt_ack_flags_t mqtt_acks = { 0 };
static mqtt_callbacks_t mqtt_callbacks = { 0 };

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data);
static void mqtt_queue_cmd(mqtt_cmd_type_t type);
static void mqtt_queue_cmd_wakeup(mqtt_cmd_type_t type);
static void mqtt_reset_runtime_timers(void);
static bool create_timers(void);
static void destroy_timers(void);
static bool create_sync_objects(void);
static void destroy_sync_objects(void);

/* ---------- Subscription helpers (called only from Mongoose thread) ------- */

static mqtt_subscription_t *find_subscription(const char *topic)
{
	if (!topic)
		return NULL;

	for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
		if (mqtt_state.subscriptions[i].active &&
		    strcmp(mqtt_state.subscriptions[i].topic, topic) == 0)
			return &mqtt_state.subscriptions[i];
	}
	return NULL;
}

static mqtt_subscription_t *find_free_subscription_slot(void)
{
	for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
		if (!mqtt_state.subscriptions[i].active)
			return &mqtt_state.subscriptions[i];
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
	osal_status_t lock_rc;
	char topics[MAX_SUBSCRIPTIONS][MQTT_APP_TOPIC_MAX_LEN] = { 0 };
	int topic_count = 0;

	if (!mqtt_state.nc)
		return;

	lock_rc = osal_mutex_take(mqtt_sync.subscriptions_lock);
	if (lock_rc != OSAL_SUCCESS)
		return;

	for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
		if (mqtt_state.subscriptions[i].active) {
			strncpy(topics[topic_count], mqtt_state.subscriptions[i].topic,
				sizeof(topics[topic_count]) - 1);
			topics[topic_count][sizeof(topics[topic_count]) - 1] =
				'\0';
			topic_count++;
		}
	}

	(void)osal_mutex_give(mqtt_sync.subscriptions_lock);

	for (int i = 0; i < topic_count; i++) {
		mg_mqtt_sub(mqtt_state.nc,
			    &(struct mg_mqtt_opts){
				    .topic = mg_str(topics[i]),
				    .qos = 0,
			    });
	}
}

/* ---------- Timer callbacks (queue + wakeup only) ------------------------- */

static void reconnect_timer_cb(osal_timer_id_t timer_id)
{
	(void)timer_id;
	mqtt_queue_cmd_wakeup(MQTT_CMD_TYPE_CONNECT);
}

static void ping_timer_cb(osal_timer_id_t timer_id)
{
	(void)timer_id;
	mqtt_queue_cmd_wakeup(MQTT_CMD_TYPE_PING);
}

static void puback_timer_callback(osal_timer_id_t timer_id)
{
	(void)timer_id;
	mqtt_queue_cmd_wakeup(MQTT_CMD_TYPE_PUBACK_TIMEOUT);
}

static void suback_timer_callback(osal_timer_id_t timer_id)
{
	(void)timer_id;
	mqtt_queue_cmd_wakeup(MQTT_CMD_TYPE_SUBACK_TIMEOUT);
}

static void unsuback_timer_callback(osal_timer_id_t timer_id)
{
	(void)timer_id;
	mqtt_queue_cmd_wakeup(MQTT_CMD_TYPE_UNSUBACK_TIMEOUT);
}

/* ---------- Queue and wakeup helpers -------------------------------------- */

static void mqtt_queue_cmd(mqtt_cmd_type_t type)
{
	mqtt_cmd_t cmd = { 0 };
	cmd.type = type;
	(void)osal_queue_send(mqtt_sync.cmd_queue, &cmd, 0);
}

static void mqtt_queue_cmd_wakeup(mqtt_cmd_type_t type)
{
	mqtt_queue_cmd(type);
	if (mqtt_state.control_conn_id > 0)
		mg_wakeup(&mgr, mqtt_state.control_conn_id, NULL, 0);
}

/* ---------- TLS setup (called only from Mongoose thread) ------------------ */

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

/* ---------- Mongoose-thread MQTT actions ---------------------------------- */

static void schedule_reconnect(void)
{
	(void)osal_timer_stop(mqtt_timers.reconnect, 0);
	(void)osal_timer_start(mqtt_timers.reconnect, 0);
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

	if (mqtt_callbacks.connect_cb)
		mqtt_callbacks.connect_cb();
}

static void mqtt_disconnected(void)
{
	bool was_connected = mqtt_state.connected;

	mqtt_state.connected = false;
	mqtt_state.nc        = NULL;
	osal_log_warning("MQTT disconnected");

	(void)osal_timer_stop(mqtt_timers.reconnect, 0);
	(void)osal_timer_stop(mqtt_timers.ping, 0);

	if (was_connected && mqtt_callbacks.disconnect_cb)
		mqtt_callbacks.disconnect_cb();

	if (mqtt_state.reconnect_enabled)
		schedule_reconnect();
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

/* ---------- Command handlers (all run in Mongoose thread) ----------------- */

static void handle_cmd_connect(void)
{
	if (mqtt_state.initialized && !mqtt_state.connected) {
		mqtt_state.reconnect_enabled = true;
		mqtt_connect();
	}
}

static void handle_cmd_disconnect(void)
{
	mqtt_disconnect_internal();
}

static void handle_cmd_apply_config(void)
{
	mqtt_disconnect_internal();
	mqtt_state.reconnect_enabled = true;
	mqtt_connect();
}

static void handle_cmd_publish(const mqtt_cmd_t *cmd)
{
	if (!mqtt_state.nc || !mqtt_state.connected) {
		osal_log_warning("MQTT publish skipped: disconnected");
		return;
	}

	mqtt_acks.puback_received = false;
	memset(&mqtt_state.publish_opts, 0, sizeof(mqtt_state.publish_opts));

	strncpy(mqtt_state.publish_topic, cmd->topic,
		sizeof(mqtt_state.publish_topic) - 1);
	mqtt_state.publish_topic[sizeof(mqtt_state.publish_topic) - 1] = '\0';
	strncpy(mqtt_state.publish_message, cmd->message,
		sizeof(mqtt_state.publish_message) - 1);
	mqtt_state.publish_message[sizeof(mqtt_state.publish_message) - 1] =
		'\0';

	mqtt_state.publish_opts.qos     = cmd->qos;
	mqtt_state.publish_opts.topic   = mg_str(mqtt_state.publish_topic);
	mqtt_state.publish_opts.version = 4;
	mqtt_state.publish_opts.message = mg_str(mqtt_state.publish_message);
	mqtt_state.retries              = 0;

	mg_mqtt_pub(mqtt_state.nc, &mqtt_state.publish_opts);

	if (cmd->qos == 1)
		(void)osal_timer_start(mqtt_timers.puback, 0);
}

static void handle_cmd_subscribe(const mqtt_cmd_t *cmd)
{
	if (!mqtt_state.nc || !mqtt_state.connected) {
		osal_log_warning("MQTT subscribe skipped: disconnected");
		(void)osal_bin_sem_give(mqtt_sync.suback);
		return;
	}

	strncpy(mqtt_state.pending_subscribe_topic, cmd->topic,
		sizeof(mqtt_state.pending_subscribe_topic) - 1);
	mqtt_state.pending_subscribe_topic
		[sizeof(mqtt_state.pending_subscribe_topic) - 1] = '\0';

	mqtt_acks.suback_received = false;
	mg_mqtt_sub(mqtt_state.nc,
		    &(struct mg_mqtt_opts){ .topic = mg_str(cmd->topic),
					    .qos = cmd->qos });
}

static void handle_cmd_unsubscribe(const mqtt_cmd_t *cmd)
{
	if (!mqtt_state.nc || !mqtt_state.connected) {
		osal_log_warning("MQTT unsubscribe skipped: disconnected");
		(void)osal_bin_sem_give(mqtt_sync.unsuback);
		return;
	}

	strncpy(mqtt_state.pending_unsubscribe_topic, cmd->topic,
		sizeof(mqtt_state.pending_unsubscribe_topic) - 1);
	mqtt_state.pending_unsubscribe_topic
		[sizeof(mqtt_state.pending_unsubscribe_topic) - 1] = '\0';

	mqtt_acks.unsuback_received = false;
	mg_mqtt_unsub(mqtt_state.nc,
		      &(struct mg_mqtt_opts){ .topic = mg_str(cmd->topic) });
}

static void handle_cmd_ping(void)
{
	if (mqtt_state.connected && mqtt_state.nc != NULL)
		mg_mqtt_ping(mqtt_state.nc);
}

static void handle_cmd_puback_timeout(void)
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

static void handle_cmd_suback_timeout(void)
{
	if (mqtt_acks.suback_received)
		return;
	osal_log_warning("MQTT SUBACK timeout topic=%s",
			 mqtt_state.pending_subscribe_topic);
	(void)osal_bin_sem_give(mqtt_sync.suback);
}

static void handle_cmd_unsuback_timeout(void)
{
	if (mqtt_acks.unsuback_received)
		return;
	osal_log_warning("MQTT UNSUBACK timeout topic=%s",
			 mqtt_state.pending_unsubscribe_topic);
	(void)osal_bin_sem_give(mqtt_sync.unsuback);
}

static void handle_cmd_shutdown(void)
{
	mqtt_disconnect_internal();

	if (mqtt_state.control_nc != NULL) {
		mqtt_state.control_nc->is_closing = 1;
		mqtt_state.control_nc = NULL;
		mqtt_state.control_conn_id = 0;
	}

	(void)osal_bin_sem_give(mqtt_sync.shutdown_sem);
}

/* ---------- Command dispatch (Mongoose thread) ---------------------------- */

static void dispatch_cmd(const mqtt_cmd_t *cmd)
{
	switch (cmd->type) {
	case MQTT_CMD_TYPE_CONNECT:
		handle_cmd_connect();
		break;
	case MQTT_CMD_TYPE_DISCONNECT:
		handle_cmd_disconnect();
		break;
	case MQTT_CMD_TYPE_APPLY_CONFIG:
		handle_cmd_apply_config();
		break;
	case MQTT_CMD_TYPE_PUBLISH:
		handle_cmd_publish(cmd);
		break;
	case MQTT_CMD_TYPE_SUBSCRIBE:
		handle_cmd_subscribe(cmd);
		break;
	case MQTT_CMD_TYPE_UNSUBSCRIBE:
		handle_cmd_unsubscribe(cmd);
		break;
	case MQTT_CMD_TYPE_PING:
		handle_cmd_ping();
		break;
	case MQTT_CMD_TYPE_PUBACK_TIMEOUT:
		handle_cmd_puback_timeout();
		break;
	case MQTT_CMD_TYPE_SUBACK_TIMEOUT:
		handle_cmd_suback_timeout();
		break;
	case MQTT_CMD_TYPE_UNSUBACK_TIMEOUT:
		handle_cmd_unsuback_timeout();
		break;
	case MQTT_CMD_TYPE_SHUTDOWN:
		handle_cmd_shutdown();
		break;
	default:
		break;
	}
}

static void drain_command_queue(void)
{
	mqtt_cmd_t cmd;

	while (osal_queue_receive(mqtt_sync.cmd_queue, &cmd, 0) ==
	       OSAL_SUCCESS) {
		dispatch_cmd(&cmd);
	}
}

/* ---------- MQTT event handling (Mongoose thread) ------------------------- */

static void handle_mqtt_message(struct mg_mqtt_message *mm)
{
	char topic_str[129] = { 0 };
	mqtt_message_callback_t callback = NULL;
	size_t topic_len = mm->topic.len < sizeof(topic_str) - 1 ?
				   mm->topic.len :
				   sizeof(topic_str) - 1;
	size_t payload_preview_len = mm->data.len < 120 ? mm->data.len : 120;
	memcpy(topic_str, mm->topic.buf, topic_len);

	osal_log_info("MQTT RX topic=%s payload_len=%u payload=%.*s", topic_str,
		      (unsigned)mm->data.len, (int)payload_preview_len,
		      mm->data.buf ? mm->data.buf : "");

	if (osal_mutex_take(mqtt_sync.subscriptions_lock) == OSAL_SUCCESS) {
		for (int i = 0; i < MAX_SUBSCRIPTIONS; i++) {
			if (mqtt_state.subscriptions[i].active &&
			    mqtt_state.subscriptions[i].callback) {
				if (mg_match(mg_str(topic_str),
					     mg_str(mqtt_state.subscriptions[i]
						    .topic),
					     NULL)) {
					callback =
						mqtt_state.subscriptions[i].callback;
					break;
				}
			}
		}
		(void)osal_mutex_give(mqtt_sync.subscriptions_lock);
	}

	if (callback) {
		callback(topic_str, mm->data.buf, mm->data.len);
	}
}

static void handle_mqtt_command_event(struct mg_mqtt_message *mm)
{
	switch (mm->cmd) {
	case MQTT_CMD_CONNACK:
		if (mm->ack == 0) {
			mqtt_connected();
		} else {
			osal_log_error("MQTT CONNACK rejected ack=%u",
				       (unsigned)mm->ack);
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

/* ---------- Unified ev_handler -------------------------------------------- */

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data)
{
	/* Control connection: handle wakeup events */
	if (nc == mqtt_state.control_nc) {
		if (ev == MG_EV_WAKEUP)
			drain_command_queue();
		return;
	}

	/* Ignore events from stale broker connections */
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
		handle_mqtt_command_event((struct mg_mqtt_message *)ev_data);
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

/* ---------- Control connection bootstrap (Mongoose thread via poll cb) ----- */

static void init_poll_cb(struct mg_connection *nc, int ev, void *ev_data)
{
	(void)ev_data;
	if (ev == MG_EV_POLL && mqtt_state.control_nc == NULL &&
	    !mqtt_state.shutdown_requested) {
		mqtt_state.control_nc =
			mg_listen(&mgr, "udp://127.0.0.1:0", ev_handler, NULL);
		if (mqtt_state.control_nc) {
			mqtt_state.control_conn_id = mqtt_state.control_nc->id;
			osal_log_info("MQTT control connection id=%lu",
				      mqtt_state.control_conn_id);
			(void)osal_bin_sem_give(mqtt_sync.init_sem);
		}
		/* Remove this temporary listener after first poll */
		nc->is_closing = 1;
	}
}

/* ---------- Config change callback ---------------------------------------- */

static void config_update_callback(void)
{
	if (!mqtt_state.initialized)
		return;
	mqtt_queue_cmd_wakeup(MQTT_CMD_TYPE_APPLY_CONFIG);
}

/* ---------- Timer/sync creation ------------------------------------------- */

static bool create_timers(void)
{
	if (osal_timer_create(&mqtt_timers.puback, "mqtt_puback",
			      TIMEOUT_DEFAULT_MS, true, puback_timer_callback,
			      NULL, NULL, 0) != OSAL_SUCCESS)
		goto fail;
	if (osal_timer_create(&mqtt_timers.suback, "mqtt_suback",
			      TIMEOUT_DEFAULT_MS, false, suback_timer_callback,
			      NULL, NULL, 0) != OSAL_SUCCESS)
		goto fail_puback;
	if (osal_timer_create(&mqtt_timers.unsuback, "mqtt_unsuback",
			      TIMEOUT_DEFAULT_MS, false,
			      unsuback_timer_callback, NULL, NULL,
			      0) != OSAL_SUCCESS)
		goto fail_suback;
	if (osal_timer_create(&mqtt_timers.reconnect, "mqtt_reconnect",
			      RECONNECT_DELAY_MS, false, reconnect_timer_cb,
			      NULL, NULL, 0) != OSAL_SUCCESS)
		goto fail_unsuback;
	if (osal_timer_create(&mqtt_timers.ping, "mqtt_ping",
			      PING_INTERVAL_MS, true, ping_timer_cb,
			      NULL, NULL, 0) != OSAL_SUCCESS)
		goto fail_reconnect;
	return true;

fail_reconnect:
	(void)osal_timer_delete(mqtt_timers.reconnect, 0);
fail_unsuback:
	(void)osal_timer_delete(mqtt_timers.unsuback, 0);
fail_suback:
	(void)osal_timer_delete(mqtt_timers.suback, 0);
fail_puback:
	(void)osal_timer_delete(mqtt_timers.puback, 0);
fail:
	memset(&mqtt_timers, 0, sizeof(mqtt_timers));
	return false;
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
	if (osal_queue_create(&mqtt_sync.cmd_queue, "mqtt_cmd_q",
			      COMMAND_QUEUE_SIZE,
			      sizeof(mqtt_cmd_t)) != OSAL_SUCCESS)
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
	if (osal_bin_sem_create(&mqtt_sync.shutdown_sem, "mqtt_shutdown_sem",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		return false;
	if (osal_bin_sem_create(&mqtt_sync.init_sem, "mqtt_init_sem",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		return false;
	if (osal_mutex_create(&mqtt_sync.subscriptions_lock,
			      "mqtt_subscriptions_lock") != OSAL_SUCCESS)
		return false;
	return true;
}

static void destroy_sync_objects(void)
{
	(void)osal_queue_delete(mqtt_sync.cmd_queue);
	(void)osal_bin_sem_delete(mqtt_sync.puback);
	(void)osal_bin_sem_delete(mqtt_sync.suback);
	(void)osal_bin_sem_delete(mqtt_sync.unsuback);
	(void)osal_bin_sem_delete(mqtt_sync.shutdown_sem);
	(void)osal_bin_sem_delete(mqtt_sync.init_sem);
	(void)osal_mutex_delete(mqtt_sync.subscriptions_lock);
	memset(&mqtt_sync, 0, sizeof(mqtt_sync));
}

/* ---------- Public API ---------------------------------------------------- */

void mqtt_app_init(void)
{
	if (mqtt_state.initialized)
		return;

	/* Ensure defaults + persisted config are loaded even if caller did not
	 * explicitly invoke mqtt_config_init() before mqtt_app_init(). */
	mqtt_config_init();

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
	mqtt_state.shutdown_requested = false;

	/* Create a temporary listener to bootstrap the control connection
	 * from within the Mongoose poll thread. */
	mg_listen(&mgr, "udp://127.0.0.1:0", init_poll_cb, NULL);

	/* Wait for control connection to be established */
	if (osal_bin_sem_timed_wait(mqtt_sync.init_sem, 2000) != OSAL_SUCCESS) {
		osal_log_error("MQTT control connection init timeout");
		goto err_init;
	}

	mqtt_state.initialized = true;

	/* Queue the initial CONNECT command */
	mqtt_queue_cmd_wakeup(MQTT_CMD_TYPE_CONNECT);
	return;

err_init:
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

	/* Request shutdown via wakeup and wait */
	mqtt_state.shutdown_requested = true;
	mqtt_queue_cmd_wakeup(MQTT_CMD_TYPE_SHUTDOWN);
	(void)osal_bin_sem_timed_wait(mqtt_sync.shutdown_sem, 2000);

	destroy_timers();
	destroy_sync_objects();
	memset(&mqtt_state, 0, sizeof(mqtt_state));
	memset(&mqtt_acks, 0, sizeof(mqtt_acks));
}

bool mqtt_app_subscribe(const char *topic, int qos,
			mqtt_message_callback_t callback, uint32_t timeout_ms)
{
	osal_status_t lock_rc;
	mqtt_cmd_t cmd = { 0 };

	if (!mqtt_state.initialized || !mqtt_state.connected || !topic ||
	    !callback) {
		return false;
	}

	lock_rc = osal_mutex_take(mqtt_sync.subscriptions_lock);
	if (lock_rc != OSAL_SUCCESS)
		return false;

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
	(void)osal_mutex_give(mqtt_sync.subscriptions_lock);

	/* Queue SUBSCRIBE command and wake Mongoose thread */
	cmd.type = MQTT_CMD_TYPE_SUBSCRIBE;
	cmd.qos = qos;
	strncpy(cmd.topic, topic, sizeof(cmd.topic) - 1);
	cmd.topic[sizeof(cmd.topic) - 1] = '\0';
	(void)osal_queue_send(mqtt_sync.cmd_queue, &cmd, 0);
	mg_wakeup(&mgr, mqtt_state.control_conn_id, NULL, 0);

	(void)osal_timer_change_period(mqtt_timers.suback, timeout_ms, 0);
	(void)osal_timer_start(mqtt_timers.suback, 0);

	if (osal_bin_sem_timed_wait(mqtt_sync.suback, timeout_ms + 100) !=
	    OSAL_SUCCESS || !mqtt_acks.suback_received) {
		if (osal_mutex_take(mqtt_sync.subscriptions_lock) == OSAL_SUCCESS) {
			mqtt_subscription_t *failed_sub =
				find_subscription(topic);
			if (failed_sub)
				clear_subscription(failed_sub);
			(void)osal_mutex_give(mqtt_sync.subscriptions_lock);
		}
		return false;
	}

	return true;
}

bool mqtt_app_unsubscribe(const char *topic, uint32_t timeout_ms)
{
	osal_status_t lock_rc;
	mqtt_cmd_t cmd = { 0 };

	if (!mqtt_state.initialized || !mqtt_state.connected || !topic)
		return false;

	lock_rc = osal_mutex_take(mqtt_sync.subscriptions_lock);
	if (lock_rc != OSAL_SUCCESS)
		return false;

	mqtt_subscription_t *sub = find_subscription(topic);
	if (!sub) {
		(void)osal_mutex_give(mqtt_sync.subscriptions_lock);
		return false;
	}
	(void)osal_mutex_give(mqtt_sync.subscriptions_lock);

	/* Queue UNSUBSCRIBE command and wake Mongoose thread */
	cmd.type = MQTT_CMD_TYPE_UNSUBSCRIBE;
	strncpy(cmd.topic, topic, sizeof(cmd.topic) - 1);
	cmd.topic[sizeof(cmd.topic) - 1] = '\0';
	(void)osal_queue_send(mqtt_sync.cmd_queue, &cmd, 0);
	mg_wakeup(&mgr, mqtt_state.control_conn_id, NULL, 0);

	(void)osal_timer_change_period(mqtt_timers.unsuback, timeout_ms, 0);
	(void)osal_timer_start(mqtt_timers.unsuback, 0);

	if (osal_bin_sem_timed_wait(mqtt_sync.unsuback, timeout_ms + 100) !=
	    OSAL_SUCCESS || !mqtt_acks.unsuback_received) {
		return false;
	}

	if (osal_mutex_take(mqtt_sync.subscriptions_lock) == OSAL_SUCCESS) {
		mqtt_subscription_t *done_sub = find_subscription(topic);
		if (done_sub)
			clear_subscription(done_sub);
		(void)osal_mutex_give(mqtt_sync.subscriptions_lock);
	}
	return true;
}

bool mqtt_app_post_data(const char *topic, const char *message, int qos)
{
	if (!mqtt_state.initialized || !topic || !message)
		return false;

	if (qos != 0 && qos != 1)
		return false;

	mqtt_cmd_t cmd = { 0 };

	if (strlen(topic) >= sizeof(cmd.topic) ||
	    strlen(message) >= sizeof(cmd.message))
		return false;

	cmd.type = MQTT_CMD_TYPE_PUBLISH;
	strncpy(cmd.topic, topic, sizeof(cmd.topic) - 1);
	strncpy(cmd.message, message, sizeof(cmd.message) - 1);
	cmd.topic[sizeof(cmd.topic) - 1] = '\0';
	cmd.message[sizeof(cmd.message) - 1] = '\0';
	cmd.qos = qos;

	if (osal_queue_send(mqtt_sync.cmd_queue, &cmd, 0) != OSAL_SUCCESS)
		return false;

	mg_wakeup(&mgr, mqtt_state.control_conn_id, NULL, 0);
	return true;
}

bool mqtt_app_is_connected(void)
{
	return mqtt_state.initialized && mqtt_state.connected;
}

void mqtt_app_set_connect_callback(mqtt_connect_callback_t cb)
{
	mqtt_callbacks.connect_cb = cb;
}

void mqtt_app_set_disconnect_callback(mqtt_disconnect_callback_t cb)
{
	mqtt_callbacks.disconnect_cb = cb;
}
