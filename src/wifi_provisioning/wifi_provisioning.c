#include "hq_config.h"
#include "wifi_provisioning.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mongoose_process.h"
#include "osal_log.h"
#include "osal_mutex.h"
#include "osal_task.h"
#include "wifi_http_provisioning.h"
#include "wifi_managment.h"
#include "wifi_provisioning_controller.h"

#define WIFI_PROVISIONING_EVENT_QUEUE_LEN 8u
#define WIFI_PROVISIONING_URL_MAX_LEN 128u
#define WIFI_PROVISIONING_WAIT_LOG_PERIOD_MS 30000u

typedef struct
{
	_Atomic(osal_mutex_id_t) lock;
	_Atomic(osal_mutex_id_t) operation_lock;
	atomic_bool initialized;
	atomic_bool reachable_announced;
	bool controller_session_set;
	bool success_pending;
	uint32_t controller_session;
	uint32_t wait_log_last_ms;
	wifi_provisioning_event_t events[WIFI_PROVISIONING_EVENT_QUEUE_LEN];
	unsigned event_head;
	unsigned event_count;
#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
	char http_url[WIFI_PROVISIONING_URL_MAX_LEN];
	bool http_url_set;
	char dns_url[WIFI_PROVISIONING_URL_MAX_LEN];
	bool dns_url_set;
#endif
} wifi_provisioning_context_t;

static wifi_provisioning_context_t s_ctx = {
	.lock = NULL,
	.operation_lock = NULL,
	.initialized = false,
	.reachable_announced = false,
	.wait_log_last_ms = UINT32_MAX,
};

static bool create_lock(_Atomic(osal_mutex_id_t) *slot, const char *name)
{
	osal_mutex_id_t lock = atomic_load_explicit(slot, memory_order_acquire);
	osal_mutex_id_t created = NULL;
	osal_mutex_id_t expected = NULL;

	if (lock != NULL)
	{
		return true;
	}
	if (osal_mutex_create(&created, name) != OSAL_SUCCESS)
	{
		return false;
	}
	if (!atomic_compare_exchange_strong_explicit(slot, &expected, created,
																								memory_order_release,
																								memory_order_acquire))
	{
		(void)osal_mutex_delete(created);
	}
	return true;
}

static bool take_lock(_Atomic(osal_mutex_id_t) *slot)
{
	osal_mutex_id_t lock = atomic_load_explicit(slot, memory_order_acquire);
	return lock != NULL && osal_mutex_take(lock) == OSAL_SUCCESS;
}

static void give_lock(_Atomic(osal_mutex_id_t) *slot)
{
	osal_mutex_id_t lock = atomic_load_explicit(slot, memory_order_acquire);
	if (lock != NULL)
	{
		(void)osal_mutex_give(lock);
	}
}

static bool ensure_locks(void)
{
	return create_lock(&s_ctx.lock, "wifi_prov_events") &&
				 create_lock(&s_ctx.operation_lock, "wifi_prov_api");
}

/* Bounded queue: on overflow the oldest event is dropped so the latest
 * provisioning state is always delivered. */
static void enqueue_event(wifi_provisioning_event_t event)
{
	unsigned tail;

	if (s_ctx.event_count == WIFI_PROVISIONING_EVENT_QUEUE_LEN)
	{
		s_ctx.event_head = (s_ctx.event_head + 1u) % WIFI_PROVISIONING_EVENT_QUEUE_LEN;
		--s_ctx.event_count;
		osal_log_warning("[prov_mgr] pending provisioning event queue overflow: oldest event dropped");
	}
	tail = (s_ctx.event_head + s_ctx.event_count) % WIFI_PROVISIONING_EVENT_QUEUE_LEN;
	s_ctx.events[tail] = event;
	++s_ctx.event_count;
}

#ifdef CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK
static wifi_provisioning_event_t translate_transition(
		wifi_provisioning_controller_state_t previous,
		wifi_provisioning_controller_state_t current)
{
	if (current == WIFI_PROVISIONING_CONTROLLER_PROVISIONING)
	{
		return WIFI_PROVISIONING_EVENT_STARTED;
	}
	if (current == WIFI_PROVISIONING_CONTROLLER_ONLINE &&
			(previous == WIFI_PROVISIONING_CONTROLLER_GRACE ||
			 previous == WIFI_PROVISIONING_CONTROLLER_RETIRING_AP))
	{
		return WIFI_PROVISIONING_EVENT_SUCCEEDED;
	}
	return WIFI_PROVISIONING_EVENT_NONE;
}

static void on_controller_state_changed(
		wifi_provisioning_controller_state_t previous,
		wifi_provisioning_controller_state_t current,
		uint32_t session,
		void *user_ctx)
{
	wifi_provisioning_event_t event;
	(void)user_ctx;

	if (!take_lock(&s_ctx.lock))
	{
		return;
	}
	if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
	{
		give_lock(&s_ctx.lock);
		return;
	}
	if (!s_ctx.controller_session_set)
	{
		s_ctx.controller_session = session;
		s_ctx.controller_session_set = true;
	}
	else if (s_ctx.controller_session != session)
	{
		give_lock(&s_ctx.lock);
		return;
	}

	if (current == WIFI_PROVISIONING_CONTROLLER_GRACE)
	{
		s_ctx.success_pending = true;
	}
	else if (current == WIFI_PROVISIONING_CONTROLLER_PROVISIONING ||
					 current == WIFI_PROVISIONING_CONTROLLER_AWAITING_CONNECT ||
					 current == WIFI_PROVISIONING_CONTROLLER_DISABLED)
	{
		s_ctx.success_pending = false;
	}

	event = translate_transition(previous, current);
	if (event == WIFI_PROVISIONING_EVENT_SUCCEEDED && !s_ctx.success_pending)
	{
		event = WIFI_PROVISIONING_EVENT_NONE;
	}
	if (event != WIFI_PROVISIONING_EVENT_NONE)
	{
		enqueue_event(event);
	}
	if (current == WIFI_PROVISIONING_CONTROLLER_ONLINE ||
			current == WIFI_PROVISIONING_CONTROLLER_DISABLED)
	{
		s_ctx.success_pending = false;
	}
	give_lock(&s_ctx.lock);
}
#endif

static void announce_reachable(void)
{
	if (wifi_http_provisioning_is_reachable() &&
			!atomic_exchange_explicit(&s_ctx.reachable_announced, true,
																memory_order_acq_rel))
	{
		osal_log_info("[prov_mgr] provisioning AP up; portal reachable");
	}
}

static void observe_wait_state(void)
{
#ifdef CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK
	uint32_t now;
	if (!wifi_http_provisioning_is_reachable() ||
			wifi_provisioning_controller_get_state() !=
					WIFI_PROVISIONING_CONTROLLER_PROVISIONING)
	{
		return;
	}
	now = osal_task_get_time_ms();
	if (s_ctx.wait_log_last_ms == UINT32_MAX ||
			now - s_ctx.wait_log_last_ms >= WIFI_PROVISIONING_WAIT_LOG_PERIOD_MS)
	{
		s_ctx.wait_log_last_ms = now;
		osal_log_info("[prov_mgr] portal up; station not yet connected "
					  "(waiting for a client to join and submit; one log line per 30 s)");
	}
#endif
}

wifi_provisioning_status_t wifi_provisioning_init(void)
{
	if (!ensure_locks() || !take_lock(&s_ctx.operation_lock))
	{
		return WIFI_PROVISIONING_ERR_RESOURCE;
	}
	if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
	{
		if (!take_lock(&s_ctx.lock))
		{
			give_lock(&s_ctx.operation_lock);
			return WIFI_PROVISIONING_ERR_RESOURCE;
		}
		s_ctx.controller_session_set = false;
		s_ctx.success_pending = false;
		s_ctx.event_head = 0u;
		s_ctx.event_count = 0u;
		s_ctx.wait_log_last_ms = UINT32_MAX;
		atomic_store_explicit(&s_ctx.reachable_announced, false, memory_order_release);
		atomic_store_explicit(&s_ctx.initialized, true, memory_order_release);
		give_lock(&s_ctx.lock);
#ifdef CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK
		{
			wifi_provisioning_controller_config_t config;
			memset(&config, 0, sizeof(config));
			config.on_state_changed = on_controller_state_changed;
			config.user_ctx = &s_ctx;
			if (!wifi_provisioning_controller_init_with_config(&config))
			{
				osal_log_error("[wifi_provisioning] fallback controller init failed");
			}
		}
#endif
	}
	give_lock(&s_ctx.operation_lock);
	return WIFI_PROVISIONING_OK;
}

wifi_provisioning_status_t wifi_provisioning_deinit(void)
{
	if (!ensure_locks() || !take_lock(&s_ctx.operation_lock))
	{
		return WIFI_PROVISIONING_ERR_RESOURCE;
	}
	if (atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
	{
		(void)wifi_http_provisioning_stop();
		if (take_lock(&s_ctx.lock))
		{
			atomic_store_explicit(&s_ctx.initialized, false, memory_order_release);
			s_ctx.controller_session_set = false;
			s_ctx.success_pending = false;
			s_ctx.event_head = 0u;
			s_ctx.event_count = 0u;
			s_ctx.wait_log_last_ms = UINT32_MAX;
			atomic_store_explicit(&s_ctx.reachable_announced, false,
														memory_order_release);
#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
			s_ctx.http_url_set = false;
			s_ctx.dns_url_set = false;
#endif
			give_lock(&s_ctx.lock);
		}
#ifdef CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK
		wifi_provisioning_controller_deinit();
#endif
	}
	give_lock(&s_ctx.operation_lock);
	return WIFI_PROVISIONING_OK;
}

wifi_provisioning_status_t wifi_provisioning_start(void)
{
	wifi_provisioning_status_t result = WIFI_PROVISIONING_OK;
	wifi_http_provisioning_start_status_t start_status;

	if (!ensure_locks() || !take_lock(&s_ctx.operation_lock))
	{
		return WIFI_PROVISIONING_ERR_RESOURCE;
	}
	if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
	{
		result = WIFI_PROVISIONING_ERR_NOT_INITIALIZED;
		goto done;
	}
	if (!MongooseProcess_IsRunning())
	{
		result = WIFI_PROVISIONING_ERR_MONGOOSE_NOT_RUNNING;
		goto done;
	}
#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
	if (s_ctx.http_url_set)
	{
		(void)wifi_http_provisioning_set_http_url(s_ctx.http_url);
	}
	if (s_ctx.dns_url_set)
	{
		(void)wifi_http_provisioning_set_dns_url(s_ctx.dns_url);
	}
#endif
	start_status = wifi_http_provisioning_start_ex();
	switch (start_status)
	{
		case WIFI_HTTP_PROVISIONING_START_OK:
		case WIFI_HTTP_PROVISIONING_START_ALREADY_RUNNING:
			break;
		case WIFI_HTTP_PROVISIONING_START_ERR_DEPENDENCY:
			result = WIFI_PROVISIONING_ERR_DEPENDENCY;
			break;
		case WIFI_HTTP_PROVISIONING_START_ERR_MODE_TRANSITION:
			result = WIFI_PROVISIONING_ERR_MODE_TRANSITION;
			break;
		case WIFI_HTTP_PROVISIONING_START_ERR_HTTP_BIND:
			result = WIFI_PROVISIONING_ERR_HTTP_BIND;
			break;
		case WIFI_HTTP_PROVISIONING_START_ERR_DNS_BIND:
			result = WIFI_PROVISIONING_ERR_DNS_BIND;
			break;
		case WIFI_HTTP_PROVISIONING_START_ERR_NO_AP:
			result = WIFI_PROVISIONING_ERR_NO_AP;
			break;
		default:
			result = WIFI_PROVISIONING_ERR_START_FAILED;
			break;
	}
	if (result != WIFI_PROVISIONING_OK && take_lock(&s_ctx.lock))
	{
		enqueue_event(WIFI_PROVISIONING_EVENT_FAILED);
		give_lock(&s_ctx.lock);
	}
	else if (result != WIFI_PROVISIONING_OK)
	{
		osal_log_warning("[wifi_provisioning] failed event dropped: event lock unavailable");
	}
	give_lock(&s_ctx.operation_lock);
	if (result == WIFI_PROVISIONING_OK)
	{
		announce_reachable();
	}
	return result;

done:
	give_lock(&s_ctx.operation_lock);
	return result;
}

wifi_provisioning_status_t wifi_provisioning_stop(void)
{
	bool controller_ok = true;
	bool portal_ok;

	if (!ensure_locks() || !take_lock(&s_ctx.operation_lock))
	{
		return WIFI_PROVISIONING_ERR_RESOURCE;
	}
	if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
	{
		give_lock(&s_ctx.operation_lock);
		return WIFI_PROVISIONING_ERR_NOT_INITIALIZED;
	}
#ifdef CONFIG_WIFI_HTTP_PROVISIONING_AUTO_FALLBACK
	controller_ok = wifi_provisioning_controller_stop();
#endif
	if (!controller_ok)
	{
		osal_log_warning("[wifi_provisioning] controller remains recoverable");
	}
	portal_ok = wifi_http_provisioning_stop();
	if (portal_ok)
	{
		atomic_store_explicit(&s_ctx.reachable_announced, false,
													memory_order_release);
		osal_log_info("[prov_mgr] provisioning portal stopped "
									"(controller lifecycle ended)");
	}
	give_lock(&s_ctx.operation_lock);
	return portal_ok ? WIFI_PROVISIONING_OK : WIFI_PROVISIONING_ERR_STOP_FAILED;
}

wifi_provisioning_state_t wifi_provisioning_get_state(void)
{
	if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
	{
		return WIFI_PROVISIONING_STATE_STOPPED;
	}
	switch (wifi_http_provisioning_get_state())
	{
		case WIFI_PROVISIONING_STARTING:
			return WIFI_PROVISIONING_STATE_STARTING;
		case WIFI_PROVISIONING_RUNNING:
			return WIFI_PROVISIONING_STATE_RUNNING;
		case WIFI_PROVISIONING_STOPPING:
			return WIFI_PROVISIONING_STATE_STOPPING;
		case WIFI_PROVISIONING_ERROR:
			return WIFI_PROVISIONING_STATE_ERROR;
		case WIFI_PROVISIONING_STOPPED:
		default:
			return WIFI_PROVISIONING_STATE_STOPPED;
	}
}

bool wifi_provisioning_is_active(void)
{
	if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
	{
		return false;
	}
	announce_reachable();
	return wifi_http_provisioning_is_reachable();
}

bool wifi_provisioning_has_saved_credentials(void)
{
	return atomic_load_explicit(&s_ctx.initialized, memory_order_acquire) &&
				 wifi_mgmt_is_read_data();
}

wifi_provisioning_event_t wifi_provisioning_poll_event(void)
{
	wifi_provisioning_event_t event = WIFI_PROVISIONING_EVENT_NONE;
	if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
	{
		return event;
	}
	announce_reachable();
	if (!ensure_locks() || !take_lock(&s_ctx.lock))
	{
		return event;
	}
	if (s_ctx.event_count != 0u)
	{
		event = s_ctx.events[s_ctx.event_head];
		s_ctx.event_head = (s_ctx.event_head + 1u) % WIFI_PROVISIONING_EVENT_QUEUE_LEN;
		--s_ctx.event_count;
	}
	observe_wait_state();
	give_lock(&s_ctx.lock);
	return event;
}

#ifdef WIFI_PROVISIONING_TEST_OBSERVABILITY
static bool store_url(char *slot, bool *set, const char *url)
{
	size_t length;
	if (url == NULL || (length = strlen(url)) == 0u ||
			length >= WIFI_PROVISIONING_URL_MAX_LEN || !ensure_locks() ||
			!take_lock(&s_ctx.operation_lock))
	{
		return false;
	}
	if (!atomic_load_explicit(&s_ctx.initialized, memory_order_acquire))
	{
		give_lock(&s_ctx.operation_lock);
		return false;
	}
	memcpy(slot, url, length + 1u);
	*set = true;
	give_lock(&s_ctx.operation_lock);
	return true;
}

bool wifi_provisioning_set_http_url(const char *url)
{
	return store_url(s_ctx.http_url, &s_ctx.http_url_set, url);
}

bool wifi_provisioning_set_dns_url(const char *url)
{
	return store_url(s_ctx.dns_url, &s_ctx.dns_url_set, url);
}
#endif