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
#define DEFAULT_KEEPALIVE_SEC 60U
#define DEFAULT_RECONNECT_INITIAL_DELAY_MS 30000U
#define DEFAULT_RECONNECT_MAX_DELAY_MS 300000U
#define MIN_KEEPALIVE_SEC 15U
#define MAX_KEEPALIVE_SEC 1200U
#define MIN_RECONNECT_DELAY_MS 1000U
#define MAX_RECONNECT_DELAY_MS 3600000U
#define MIN_PING_INTERVAL_MS 1000U
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
	int qos;
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
	uint16_t keepalive_sec;
	uint32_t reconnect_initial_delay_ms;
	uint32_t reconnect_max_delay_ms;
	bool reconnect_exponential_backoff;
	uint32_t reconnect_attempt;
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
	mqtt_connect_failure_callback_t connect_failure_cb;
} mqtt_callbacks_t;

/* The safety observer is deliberately separate from the replaceable client
 * callback set.  ThingsBoard may own the latter, but it cannot disable the
 * platform fail-off observer. */
static mqtt_callbacks_t mqtt_safety_callbacks = { 0 };

/* Configuration-owner validation gate (see mqtt_app_set_config_validation_-
 * callback()).  The platform is policy-neutral: it never interprets the
 * configuration, it only requires that a designated owner registered a
 * validator AND that the validator approved the update.  NULL (no registered
 * owner) means the apply-config gate fails closed, so an unvalidated mutated
 * configuration can never reach the transport. */
static mqtt_config_validation_callback_t mqtt_config_validator = NULL;

/* Guards the callback sets and the validator against concurrent readers
 * (Mongoose poll thread) and writers (registration thread).  Created lazily
 * on first registration so registration stays legal before mqtt_app_init()
 * and while the transport is stopped; never deleted, because the observer
 * set is module ownership state, not a transport resource.
 *
 * Initialization is synchronized once: mqtt_registry_lock_state is published
 * with a compare-and-swap plus a full memory barrier (the same pattern
 * mongoose_process.c uses for its own mutex bootstrap), so concurrent first
 * registrations observe a single published mutex instead of racing on a
 * plain non-atomic pointer.  If mutex creation fails, callers refuse the
 * registration rather than silently proceed without synchronization. */
static osal_mutex_id_t mqtt_registry_lock = NULL;
static volatile uint8_t mqtt_registry_lock_state = 0; /* 0=uninit,1=creating,2=ready */

static mqtt_state_t mqtt_state = { 0 };
static mqtt_timers_t mqtt_timers = { 0 };
static mqtt_sync_t mqtt_sync = { 0 };
static mqtt_ack_flags_t mqtt_acks = { 0 };
static mqtt_callbacks_t mqtt_callbacks = { 0 };
static mqtt_connection_policy_t mqtt_policy = {
	.keepalive_sec = DEFAULT_KEEPALIVE_SEC,
	.reconnect_initial_delay_ms = DEFAULT_RECONNECT_INITIAL_DELAY_MS,
	.reconnect_max_delay_ms = DEFAULT_RECONNECT_MAX_DELAY_MS,
	.reconnect_exponential_backoff = false,
};

static void ev_handler(struct mg_connection *nc, int ev, void *ev_data);
static void mqtt_queue_cmd(mqtt_cmd_type_t type);

/* ---------- Observer registry (thread-safe registration) ------------------ */

/* Ensures the registry mutex exists.  Returns false when the mutex could not
 * be created; callers must refuse the registration in that case so access to
 * the registry never proceeds without synchronization. */
static uint8_t mqtt_registry_lock_state_load(void)
{
	/* An atomic read is required here too: readers may be the Mongoose poll
	 * thread and can arrive before the first registration has completed. */
	return __sync_val_compare_and_swap(&mqtt_registry_lock_state, 0, 0);
}

static osal_mutex_id_t mqtt_registry_lock_load(void)
{
	/* Use an atomic pointer load so no reader observes a partially published
	 * identifier while the first user is publishing the mutex. */
	return __sync_val_compare_and_swap(&mqtt_registry_lock, NULL, NULL);
}

static bool mqtt_registry_lock_ensure(void)
{
	osal_mutex_id_t lock = NULL;
	uint8_t state;

	for (;;) {
		state = mqtt_registry_lock_state_load();
		if (state == 2)
			return mqtt_registry_lock_load() != NULL;

		/* Only the first caller creates the mutex; every other concurrent first
		 * user waits until the creator publishes a fully initialized lock. */
		if (state == 0 &&
		    __sync_bool_compare_and_swap(&mqtt_registry_lock_state, 0, 1)) {
			if (osal_mutex_create(&lock, "mqtt_registry") != OSAL_SUCCESS) {
				/* Let another caller retry creation; no caller may use the
				 * callback slots while the state is not READY. */
				(void)__sync_lock_test_and_set(&mqtt_registry_lock_state, 0);
				return false;
			}

			/* Publish the pointer before READY with a full memory barrier. */
			(void)__sync_lock_test_and_set(&mqtt_registry_lock, lock);
			(void)__sync_lock_test_and_set(&mqtt_registry_lock_state, 2);
			return true;
		}

		while (mqtt_registry_lock_state_load() == 1)
			(void)osal_task_delay_ms(1);
		/* The creator may have failed.  Loop so this caller either retries or
		 * observes a successfully published lock, rather than returning to a
		 * NULL-lock access path. */
	}
}

static bool mqtt_registry_lock_take(void)
{
	osal_mutex_id_t lock;

	if (!mqtt_registry_lock_ensure())
		return false;
	lock = mqtt_registry_lock_load();
	return lock != NULL && osal_mutex_take(lock) == OSAL_SUCCESS;
}

static void mqtt_registry_lock_give(void)
{
	osal_mutex_id_t lock = mqtt_registry_lock_load();

	if (lock != NULL)
		(void)osal_mutex_give(lock);
}

/* ---------- Observer fan-out (called only from Mongoose thread) ----------- */

/* Every *notify* helper snapshots both the safety and the client callback
 * sets under the registry lock and invokes them afterwards, outside the
 * lock.  The safety callbacks are always invoked first and IN ADDITION to
 * (never instead of) the product/client callbacks.  All callbacks run on
 * the Mongoose poll thread (the caller of these helpers). */

static void mqtt_notify_connect(void)
{
	mqtt_connect_callback_t safety_cb = NULL;
	mqtt_connect_callback_t client_cb = NULL;

	if (mqtt_registry_lock_take()) {
		safety_cb = mqtt_safety_callbacks.connect_cb;
		client_cb = mqtt_callbacks.connect_cb;
		mqtt_registry_lock_give();
	}

	if (safety_cb)
		safety_cb();
	if (client_cb)
		client_cb();
}

static void mqtt_notify_disconnect(mqtt_disconnect_reason_t reason)
{
	mqtt_disconnect_callback_t safety_cb = NULL;
	mqtt_disconnect_callback_t client_cb = NULL;

	if (mqtt_registry_lock_take()) {
		safety_cb = mqtt_safety_callbacks.disconnect_cb;
		client_cb = mqtt_callbacks.disconnect_cb;
		mqtt_registry_lock_give();
	}

	if (safety_cb)
		safety_cb(reason);
	if (client_cb)
		client_cb(reason);
}

static void mqtt_notify_connect_failure(mqtt_connect_failure_reason_t reason)
{
	mqtt_connect_failure_callback_t safety_cb = NULL;
	mqtt_connect_failure_callback_t client_cb = NULL;

	if (mqtt_registry_lock_take()) {
		safety_cb = mqtt_safety_callbacks.connect_failure_cb;
		client_cb = mqtt_callbacks.connect_failure_cb;
		mqtt_registry_lock_give();
	}

	if (safety_cb)
		safety_cb(reason);
	if (client_cb)
		client_cb(reason);
}

/* ---------- Configuration-owner validation gate --------------------------- */

/* Owned, immutable copy of every transport configuration value the
 * apply-config handler (and only it) uses for its reconnect.  This is the
 * single source of truth for both the validation step and the subsequent
 * connect: mqtt_connect()/setup_tls() never reread the mutable mqtt_config
 * globals for a gated reconnect, so a concurrent public setter cannot change
 * the address, SSL mode, skip-verify flag or certificate material between
 * the gate's approval and the transport actually using it.  The string
 * fields are owned copies, not pointers into the mutable configuration. */
typedef struct {
	char address[MQTT_CONFIG_STR_SIZE];
	char username[MQTT_CONFIG_STR_SIZE];
	char password[MQTT_CONFIG_STR_SIZE];
	char client_id[MQTT_CONFIG_STR_SIZE];
	bool ssl_enabled;
	bool skip_verify;
	mqtt_cert_source_t cert_source;
	char cert_value[MQTT_CONFIG_STR_SIZE];	 /* source value (path or raw) */
	char cert_resolved[MQTT_CERT_MAX_SIZE];  /* PEM handed to the TLS stack */
	mqtt_cert_source_t client_cert_source;
	char client_cert_value[MQTT_CONFIG_STR_SIZE];
	char client_cert_resolved[MQTT_CERT_MAX_SIZE];
	mqtt_cert_source_t client_key_source;
	char client_key_value[MQTT_CONFIG_STR_SIZE];
	char client_key_resolved[MQTT_CERT_MAX_SIZE];
} mqtt_candidate_t;

/* The candidate approved by the gate for the in-flight apply-config
 * reconnect.  Candidate contents are owned by the Mongoose poll thread;
 * the validity bit is also invalidated by the application thread when
 * deinitialization begins.  It is meaningful only between
 * MQTT_CMD_TYPE_APPLY_CONFIG acceptance and connect completion. */
static mqtt_candidate_t mqtt_pending_candidate;
/* Accessed by the application thread during deinit and by the Mongoose poll
 * thread during connect setup.  Keep the validity bit atomic: the candidate
 * remains allocated for the whole module lifetime, but its approval is valid
 * only for the current in-flight apply-config operation. */
static volatile uint8_t mqtt_pending_candidate_valid;

static bool mqtt_pending_candidate_is_valid(void)
{
	return __sync_val_compare_and_swap(&mqtt_pending_candidate_valid, 0, 0) !=
	       0;
}

static void mqtt_set_pending_candidate_valid(bool valid)
{
	(void)__sync_lock_test_and_set(&mqtt_pending_candidate_valid,
				       valid ? 1u : 0u);
}

static void mqtt_copy_str(char *dst, size_t dst_size, const char *src)
{
	strncpy(dst, src ? src : "", dst_size - 1);
	dst[dst_size - 1] = '\0';
}

static void mqtt_capture_candidate(mqtt_candidate_t *cand)
{
	mqtt_cert_source_t source = MQTT_CERT_SOURCE_NONE;
	const char *value = NULL;
	const char *resolved = NULL;
	bool flag = false;

	memset(cand, 0, sizeof(*cand));

	mqtt_copy_str(cand->address, sizeof(cand->address),
		      mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS));
	mqtt_copy_str(cand->username, sizeof(cand->username),
		      mqtt_config_get_string(MQTT_CONFIG_VALUE_USERNAME));
	mqtt_copy_str(cand->password, sizeof(cand->password),
		      mqtt_config_get_string(MQTT_CONFIG_VALUE_PASSWORD));
	mqtt_copy_str(cand->client_id, sizeof(cand->client_id),
		      mqtt_config_get_string(MQTT_CONFIG_VALUE_CLIENT_ID));

	(void)mqtt_config_get_bool(&flag, MQTT_CONFIG_VALUE_SSL);
	cand->ssl_enabled = flag;
	(void)mqtt_config_get_bool(&flag, MQTT_CONFIG_VALUE_SKIP_VERIFY);
	cand->skip_verify = flag;

	(void)mqtt_config_get_cert_source(&source, &value,
					  MQTT_CONFIG_VALUE_CERT);
	cand->cert_source = source;
	mqtt_copy_str(cand->cert_value, sizeof(cand->cert_value), value);
	resolved = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CERT);
	mqtt_copy_str(cand->cert_resolved, sizeof(cand->cert_resolved),
		      resolved);

	(void)mqtt_config_get_cert_source(&source, &value,
					  MQTT_CONFIG_VALUE_CLIENT_CERT);
	cand->client_cert_source = source;
	mqtt_copy_str(cand->client_cert_value,
		      sizeof(cand->client_cert_value), value);
	resolved = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CLIENT_CERT);
	mqtt_copy_str(cand->client_cert_resolved,
		      sizeof(cand->client_cert_resolved), resolved);

	(void)mqtt_config_get_cert_source(&source, &value,
					  MQTT_CONFIG_VALUE_CLIENT_KEY);
	cand->client_key_source = source;
	mqtt_copy_str(cand->client_key_value,
		      sizeof(cand->client_key_value), value);
	resolved = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CLIENT_KEY);
	mqtt_copy_str(cand->client_key_resolved,
		      sizeof(cand->client_key_resolved), resolved);
}

/* Builds the public, read-only snapshot view handed to the registered
 * validation callback.  All pointers reference the owned candidate buffers,
 * which stay valid for the whole apply-config decision. */
static void mqtt_snapshot_from_candidate(mqtt_config_snapshot_t *snapshot,
					 const mqtt_candidate_t *cand)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->address = cand->address;
	snapshot->ssl_enabled = cand->ssl_enabled;
	snapshot->skip_verify = cand->skip_verify;
	snapshot->cert_source = cand->cert_source;
	snapshot->cert_value = cand->cert_value;
	snapshot->client_cert_source = cand->client_cert_source;
	snapshot->client_cert_value = cand->client_cert_value;
	snapshot->client_key_source = cand->client_key_source;
	snapshot->client_key_value = cand->client_key_value;
}

/* Returns true when the apply-config handler may reconnect.  The gate is
 * fail-closed: only a registered configuration-owner callback that returns
 * true can lift it.  With no designated owner registered the update is
 * refused, so an unvalidated mutated configuration can never reach the
 * transport through the generic mqtt_config_save() path.  Runs on the
 * Mongoose poll thread. */
static bool mqtt_config_gate_accepts(mqtt_config_snapshot_t *snapshot)
{
	mqtt_config_validation_callback_t validator = NULL;
	bool accepted = false;

	if (mqtt_registry_lock_take()) {
		validator = mqtt_config_validator;
		mqtt_registry_lock_give();
	}

	if (validator != NULL)
		accepted = validator(snapshot);

	return accepted;
}

static void mqtt_queue_cmd_wakeup(mqtt_cmd_type_t type);
static void mqtt_reset_runtime_timers(void);
static void apply_connection_policy(const mqtt_connection_policy_t *policy);
static bool create_timers(void);
static void destroy_timers(void);
static bool create_sync_objects(void);
static void destroy_sync_objects(void);

static uint16_t clamp_keepalive_sec(uint16_t value)
{
	if (value == 0)
		return DEFAULT_KEEPALIVE_SEC;
	if (value < MIN_KEEPALIVE_SEC)
		return MIN_KEEPALIVE_SEC;
	if (value > MAX_KEEPALIVE_SEC)
		return MAX_KEEPALIVE_SEC;
	return value;
}

static uint32_t clamp_reconnect_delay_ms(uint32_t value, uint32_t fallback)
{
	if (value == 0)
		return fallback;
	if (value < MIN_RECONNECT_DELAY_MS)
		return MIN_RECONNECT_DELAY_MS;
	if (value > MAX_RECONNECT_DELAY_MS)
		return MAX_RECONNECT_DELAY_MS;
	return value;
}

static uint32_t compute_ping_interval_ms(uint16_t keepalive_sec)
{
	uint32_t interval = ((uint32_t)keepalive_sec * 1000U) / 2U;

	if (interval < MIN_PING_INTERVAL_MS)
		interval = MIN_PING_INTERVAL_MS;

	return interval;
}

static uint32_t compute_reconnect_delay_ms(void)
{
	uint32_t delay = mqtt_state.reconnect_initial_delay_ms;

	if (!mqtt_state.reconnect_exponential_backoff) {
		return delay;
	}

	for (uint32_t i = 0;
	     i < mqtt_state.reconnect_attempt && delay < mqtt_state.reconnect_max_delay_ms;
	     i++) {
		if (delay > mqtt_state.reconnect_max_delay_ms / 2U) {
			delay = mqtt_state.reconnect_max_delay_ms;
			break;
		}
		delay *= 2U;
	}

	if (delay > mqtt_state.reconnect_max_delay_ms)
		delay = mqtt_state.reconnect_max_delay_ms;

	return delay;
}

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
		sub->qos = 0;
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
		int qos = 0;

		for (int j = 0; j < MAX_SUBSCRIPTIONS; j++) {
			if (mqtt_state.subscriptions[j].active &&
			    strcmp(mqtt_state.subscriptions[j].topic, topics[i]) == 0) {
				qos = mqtt_state.subscriptions[j].qos;
				break;
			}
		}

		mg_mqtt_sub(mqtt_state.nc,
			    &(struct mg_mqtt_opts){
				    .topic = mg_str(topics[i]),
				    .qos = qos,
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
	bool use_cand = mqtt_pending_candidate_is_valid();

	memset(&tls, 0, sizeof(tls));

	if (use_cand) {
		/* A gated apply-config reconnect must use ONLY the validated
		 * candidate; never reread the mutable global configuration. */
		skip = mqtt_pending_candidate.skip_verify;
		ca   = mqtt_pending_candidate.cert_resolved;
		cert = mqtt_pending_candidate.client_cert_resolved;
		key  = mqtt_pending_candidate.client_key_resolved;
	} else {
		(void)mqtt_config_get_bool(&skip, MQTT_CONFIG_VALUE_SKIP_VERIFY);
		ca   = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CERT);
		cert = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CLIENT_CERT);
		key  = mqtt_config_get_cert(MQTT_CONFIG_VALUE_CLIENT_KEY);
	}

	tls.skip_verification = skip ? 1 : 0;

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
	uint32_t delay_ms = compute_reconnect_delay_ms();

	(void)osal_timer_stop(mqtt_timers.reconnect, 0);
	(void)osal_timer_change_period(mqtt_timers.reconnect, delay_ms, 0);
	(void)osal_timer_start(mqtt_timers.reconnect, 0);

	if (mqtt_state.reconnect_attempt < 31U)
		mqtt_state.reconnect_attempt++;
}

static void mqtt_connect(void)
{
	const char *address, *username, *password, *client_id;
	struct mg_mqtt_opts opts;
	bool use_cand = mqtt_pending_candidate_is_valid();

	if (use_cand) {
		/* A gated apply-config reconnect must use ONLY the validated
		 * candidate; never reread the mutable global configuration. */
		address   = mqtt_pending_candidate.address;
		username  = mqtt_pending_candidate.username;
		password  = mqtt_pending_candidate.password;
		client_id = mqtt_pending_candidate.client_id;
	} else {
		address   = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
		username  = mqtt_config_get_string(MQTT_CONFIG_VALUE_USERNAME);
		password  = mqtt_config_get_string(MQTT_CONFIG_VALUE_PASSWORD);
		client_id = mqtt_config_get_string(MQTT_CONFIG_VALUE_CLIENT_ID);
	}

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
	opts.keepalive = mqtt_state.keepalive_sec;
	opts.clean     = true;

	osal_log_info("MQTT connecting address=%s client_id=%s", address,
		      opts.client_id.buf);

	mqtt_state.nc = mg_mqtt_connect(&mgr, address, &opts, ev_handler, NULL);
	if (!mqtt_state.nc) {
		osal_log_error("MQTT connection creation failed");
		mqtt_notify_connect_failure(
			MQTT_CONNECT_FAILURE_REASON_CONNECT_CREATE_FAILED);
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
	mqtt_state.reconnect_attempt = 0;
	(void)osal_timer_stop(mqtt_timers.reconnect, 0);

	/* The gated apply-config candidate has served its purpose (TLS was set
	 * up on MG_EV_CONNECT); any later reconnect uses the regular path. */
	mqtt_set_pending_candidate_valid(false);

	osal_log_info("MQTT connected");
	resubscribe_all();

	(void)osal_timer_stop(mqtt_timers.ping, 0);
	(void)osal_timer_change_period(
		mqtt_timers.ping,
		compute_ping_interval_ms(mqtt_state.keepalive_sec), 0);
	(void)osal_timer_start(mqtt_timers.ping, 0);

	mqtt_notify_connect();
}

static void mqtt_disconnected(mqtt_disconnect_reason_t reason)
{
	bool was_connected = mqtt_state.connected;

	mqtt_state.connected = false;
	mqtt_state.nc        = NULL;
	osal_log_warning("MQTT disconnected");

	(void)osal_timer_stop(mqtt_timers.reconnect, 0);
	(void)osal_timer_stop(mqtt_timers.ping, 0);

	if (was_connected)
		mqtt_notify_disconnect(reason);

	if (!was_connected && reason == MQTT_DISCONNECT_REASON_ERROR)
		mqtt_notify_connect_failure(
			MQTT_CONNECT_FAILURE_REASON_TRANSPORT_ERROR);

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

static void mqtt_disconnect_internal(bool preserve_pending_candidate)
{
	bool was_connected = mqtt_state.connected;

	/* An explicit stop/rejection invalidates any approved candidate.  The only
	 * caller allowed to preserve it is the approved apply-config path, which
	 * immediately starts the disconnect-and-retry sequence. */
	if (!preserve_pending_candidate)
		mqtt_set_pending_candidate_valid(false);

	mqtt_state.reconnect_enabled = false;
	mqtt_state.reconnect_attempt = 0;
	mqtt_reset_runtime_timers();

	if (mqtt_state.nc != NULL) {
		mg_mqtt_disconnect(mqtt_state.nc, NULL);
		mqtt_state.nc->is_closing = 1;
		mqtt_state.nc = NULL;
	}

	mqtt_state.connected = false;

	if (was_connected)
		mqtt_notify_disconnect(MQTT_DISCONNECT_REASON_EXPLICIT);
}

/* ---------- Command handlers (all run in Mongoose thread) ----------------- */

static void handle_cmd_connect(void)
{
	if (mqtt_state.initialized && !mqtt_state.connected) {
		/* CONNECT is also the retry path for an approved apply-config
		 * operation.  Preserve its candidate until that operation succeeds;
		 * otherwise a setter that runs after approval could make an automatic
		 * retry reconnect with unvalidated values.  Initial/plain connects have
		 * no candidate and continue to use the current configuration. */
		mqtt_state.reconnect_enabled = true;
		mqtt_connect();
	}
}

static void handle_cmd_disconnect(void)
{
	mqtt_disconnect_internal(false);
}

static void handle_cmd_apply_config(void)
{
	mqtt_config_snapshot_t snapshot;

	/* The generic apply-config path must never reconnect from an
	 * unvalidated mutated configuration.  Capture an OWNED candidate with
	 * the exact values the reconnect would use and let the designated
	 * configuration owner validate it BEFORE the transport is touched.
	 * The gate is fail-closed: without a registered owner the update is
	 * refused.  On rejection the reconnect is skipped, the failure
	 * observer is notified and the transport is left disconnected (no
	 * automatic retry). */
	mqtt_capture_candidate(&mqtt_pending_candidate);
	mqtt_snapshot_from_candidate(&snapshot, &mqtt_pending_candidate);

	if (!mqtt_config_gate_accepts(&snapshot)) {
		mqtt_set_pending_candidate_valid(false);
		osal_log_error(
			"MQTT apply-config rejected by configuration owner");
		mqtt_disconnect_internal(false);
		mqtt_state.reconnect_enabled = false;
		mqtt_notify_connect_failure(
			MQTT_CONNECT_FAILURE_REASON_CONFIG_REJECTED);
		return;
	}

	/* Approved: the reconnect below (and setup_tls on MG_EV_CONNECT) reads
	 * only mqtt_pending_candidate, never the mutable global configuration,
	 * so a concurrent setter cannot swap in values the owner did not
	 * approve. */
	mqtt_set_pending_candidate_valid(true);
	mqtt_disconnect_internal(true);
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
	mqtt_disconnect_internal(false);

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
				/* Convert MQTT wildcard '+' to mg_match '*' */
				char pattern[MQTT_APP_TOPIC_MAX_LEN];
				strncpy(pattern,
					mqtt_state.subscriptions[i].topic,
					sizeof(pattern) - 1);
				pattern[sizeof(pattern) - 1] = '\0';
				for (char *p = pattern; *p; p++) {
					if (*p == '+')
						*p = '*';
				}
				if (mg_match(mg_str(topic_str),
					     mg_str(pattern), NULL)) {
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
			mqtt_notify_connect_failure(
				MQTT_CONNECT_FAILURE_REASON_CONNACK_REJECTED);
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
		const char *addr;

		osal_log_info("ev_handler: CONNECT nc=%p", (void *)nc);
		/* Deinitialization invalidates an approved candidate before it asks the
		 * poll thread to stop.  Do not fall back to mutable global configuration
		 * for a connection event that raced with that stop. */
		if (!mqtt_state.initialized || mqtt_state.shutdown_requested) {
			nc->is_closing = 1;
			break;
		}
		if (mqtt_pending_candidate_is_valid())
			addr = mqtt_pending_candidate.address;
		else
			addr = mqtt_config_get_string(MQTT_CONFIG_VALUE_ADDRESS);
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
		mqtt_disconnected(MQTT_DISCONNECT_REASON_REMOTE_CLOSE);
		break;

	case MG_EV_ERROR:
		osal_log_error("ev_handler: ERROR nc=%p err=%s",
			       (void *)nc, (char *)ev_data);
		mqtt_disconnected(MQTT_DISCONNECT_REASON_ERROR);
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

static void apply_connection_policy(const mqtt_connection_policy_t *policy)
{
	uint16_t keepalive_sec = DEFAULT_KEEPALIVE_SEC;
	uint32_t reconnect_initial_ms = DEFAULT_RECONNECT_INITIAL_DELAY_MS;
	uint32_t reconnect_max_ms = DEFAULT_RECONNECT_MAX_DELAY_MS;
	bool reconnect_backoff = false;

	if (policy != NULL) {
		keepalive_sec = policy->keepalive_sec;
		reconnect_initial_ms = policy->reconnect_initial_delay_ms;
		reconnect_max_ms = policy->reconnect_max_delay_ms;
		reconnect_backoff = policy->reconnect_exponential_backoff;
	}

	keepalive_sec = clamp_keepalive_sec(keepalive_sec);
	reconnect_initial_ms = clamp_reconnect_delay_ms(
		reconnect_initial_ms, DEFAULT_RECONNECT_INITIAL_DELAY_MS);
	reconnect_max_ms = clamp_reconnect_delay_ms(
		reconnect_max_ms, DEFAULT_RECONNECT_MAX_DELAY_MS);

	if (reconnect_max_ms < reconnect_initial_ms)
		reconnect_max_ms = reconnect_initial_ms;

	mqtt_state.keepalive_sec = keepalive_sec;
	mqtt_state.reconnect_initial_delay_ms = reconnect_initial_ms;
	mqtt_state.reconnect_max_delay_ms = reconnect_max_ms;
	mqtt_state.reconnect_exponential_backoff = reconnect_backoff;
	mqtt_state.reconnect_attempt = 0;

	mqtt_policy.keepalive_sec = keepalive_sec;
	mqtt_policy.reconnect_initial_delay_ms = reconnect_initial_ms;
	mqtt_policy.reconnect_max_delay_ms = reconnect_max_ms;
	mqtt_policy.reconnect_exponential_backoff = reconnect_backoff;
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
			      DEFAULT_RECONNECT_INITIAL_DELAY_MS, false,
			      reconnect_timer_cb,
			      NULL, NULL, 0) != OSAL_SUCCESS)
		goto fail_unsuback;
	if (osal_timer_create(&mqtt_timers.ping, "mqtt_ping",
			      compute_ping_interval_ms(DEFAULT_KEEPALIVE_SEC),
			      true, ping_timer_cb,
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
		goto fail;
	if (osal_bin_sem_create(&mqtt_sync.puback, "mqtt_puback_sem",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		goto fail_cmd_queue;
	if (osal_bin_sem_create(&mqtt_sync.suback, "mqtt_suback_sem",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		goto fail_puback;
	if (osal_bin_sem_create(&mqtt_sync.unsuback, "mqtt_unsuback_sem",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		goto fail_suback;
	if (osal_bin_sem_create(&mqtt_sync.shutdown_sem, "mqtt_shutdown_sem",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		goto fail_unsuback;
	if (osal_bin_sem_create(&mqtt_sync.init_sem, "mqtt_init_sem",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		goto fail_shutdown_sem;
	if (osal_mutex_create(&mqtt_sync.subscriptions_lock,
			      "mqtt_subscriptions_lock") != OSAL_SUCCESS)
		goto fail_init_sem;
	return true;

fail_init_sem:
	(void)osal_bin_sem_delete(mqtt_sync.init_sem);
fail_shutdown_sem:
	(void)osal_bin_sem_delete(mqtt_sync.shutdown_sem);
fail_unsuback:
	(void)osal_bin_sem_delete(mqtt_sync.unsuback);
fail_suback:
	(void)osal_bin_sem_delete(mqtt_sync.suback);
fail_puback:
	(void)osal_bin_sem_delete(mqtt_sync.puback);
fail_cmd_queue:
	(void)osal_queue_delete(mqtt_sync.cmd_queue);
fail:
	memset(&mqtt_sync, 0, sizeof(mqtt_sync));
	return false;
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

	/* A candidate is scoped to one transport session.  Invalidate any
	 * interrupted apply-config operation before a fresh session can connect. */
	mqtt_set_pending_candidate_valid(false);

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

	memset(&mqtt_acks, 0, sizeof(mqtt_acks));
	apply_connection_policy(&mqtt_policy);
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
	/* Invalidate first, even when initialization already failed or shutdown
	 * was previously requested.  This prevents a candidate approved by an old
	 * session from being consumed by a later mqtt_app_init(). */
	mqtt_set_pending_candidate_valid(false);

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
		existing->qos = qos;
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
	sub->qos = qos;
	sub->callback = callback;
	sub->active = true;
	(void)osal_mutex_give(mqtt_sync.subscriptions_lock);

	/*
	 * Drain stale SUBACK signals and clear ACK flag so the wait below tracks
	 * only this SUBSCRIBE request.
	 */
	while (osal_bin_sem_timed_wait(mqtt_sync.suback, 0) == OSAL_SUCCESS) {
	}
	mqtt_acks.suback_received = false;

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

	/*
	 * Drain any stale UNSUBACK signal from previous operations so this wait
	 * reflects only the current UNSUBSCRIBE transaction.
	 */
	while (osal_bin_sem_timed_wait(mqtt_sync.unsuback, 0) == OSAL_SUCCESS) {
	}
	mqtt_acks.unsuback_received = false;

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
	if (!mqtt_registry_lock_take())
		return;
	mqtt_callbacks.connect_cb = cb;
	mqtt_registry_lock_give();
}

void mqtt_app_set_disconnect_callback(mqtt_disconnect_callback_t cb)
{
	if (!mqtt_registry_lock_take())
		return;
	mqtt_callbacks.disconnect_cb = cb;
	mqtt_registry_lock_give();
}

void mqtt_app_set_connect_failure_callback(mqtt_connect_failure_callback_t cb)
{
	if (!mqtt_registry_lock_take())
		return;
	mqtt_callbacks.connect_failure_cb = cb;
	mqtt_registry_lock_give();
}

void mqtt_app_set_safety_callbacks(mqtt_connect_callback_t connect_cb,
				   mqtt_disconnect_callback_t disconnect_cb,
				   mqtt_connect_failure_callback_t failure_cb)
{
	if (!mqtt_registry_lock_take())
		return;

	/* Idempotent whole-set replacement: a repeated call with the same
	 * callbacks leaves exactly one registered observer set. */
	mqtt_safety_callbacks.connect_cb = connect_cb;
	mqtt_safety_callbacks.disconnect_cb = disconnect_cb;
	mqtt_safety_callbacks.connect_failure_cb = failure_cb;

	mqtt_registry_lock_give();
}

void mqtt_app_set_config_validation_callback(
	mqtt_config_validation_callback_t cb)
{
	if (!mqtt_registry_lock_take())
		return;
	mqtt_config_validator = cb;
	mqtt_registry_lock_give();
}

void mqtt_app_set_connection_policy(const mqtt_connection_policy_t *policy)
{
	apply_connection_policy(policy);
}
