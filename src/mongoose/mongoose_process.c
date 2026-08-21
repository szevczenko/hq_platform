#include "mongoose_process.h"

#include <stdbool.h>
#include <stdint.h>

#include "hq_config.h"
#include "osal_bin_sem.h"
#include "osal_mutex.h"
#include "osal_queue.h"
#include "osal_task.h"

#ifndef CONFIG_MONGOOSE_LOG_LEVEL
#define CONFIG_MONGOOSE_LOG_LEVEL 2
#endif

#define MONGOOSE_POLL_STACK_SIZE    (OSAL_TASK_MIN_STACK_SIZE * 8)
#define MONGOOSE_POLL_MS            10
#define MONGOOSE_INVOKE_QUEUE_DEPTH 16

struct mg_mgr mgr;

static volatile bool    s_running        = false;
static volatile bool    s_stop_requested = false;
static volatile bool    s_shutting_down  = false;
static osal_bin_sem_id_t s_stopped_sem;
static osal_task_id_t   s_poll_task_id;
static osal_mutex_id_t  s_state_mutex;

/* Poll-thread invocation queue and completion notification. Each item carries a
 * monotonically increasing id so a caller only wakes for ITS own completion,
 * even if an earlier invocation previously timed out while its callback was
 * still queued/running on the poll thread. */
typedef struct {
	mongoose_process_fn_t fn;
	void                *user;
	uint32_t              id;
} mongoose_invoke_item_t;

static osal_queue_id_t       s_invoke_q;
static osal_bin_sem_id_t     s_invoke_done;
static struct mg_connection *s_control_nc;
static unsigned long         s_control_conn_id;
static volatile uint32_t    s_invoke_seq;    /* Next invocation id */
static volatile uint32_t    s_completed_id;  /* Id of last finished callback */

static void poll_task(void *arg)
{
	(void)arg;
	while (!s_stop_requested)
		mg_mgr_poll(&mgr, MONGOOSE_POLL_MS);
	(void)osal_bin_sem_give(s_stopped_sem);
}

static void invoke_control_cb(struct mg_connection *nc, int ev, void *ev_data)
{
	mongoose_invoke_item_t item;

	(void)nc;
	(void)ev_data;
	if (ev != MG_EV_WAKEUP || s_invoke_q == NULL)
		return;

	while (osal_queue_receive(s_invoke_q, &item, 0) == OSAL_SUCCESS) {
		if (item.fn != NULL)
			item.fn(&mgr, item.user);
		/* Record completion and wake the waiting caller. */
		s_completed_id = item.id;
		if (s_invoke_done != NULL)
			(void)osal_bin_sem_give(s_invoke_done);
	}
}

void MongooseProcess_Init(void)
{
	if (s_running)
		return;

	s_stop_requested = false;
	s_shutting_down  = false;
	s_invoke_q       = NULL;
	s_invoke_done    = NULL;
	s_control_nc     = NULL;
	s_control_conn_id = 0;

	mg_mgr_init(&mgr);
	mg_wakeup_init(&mgr);
	mg_log_set(CONFIG_MONGOOSE_LOG_LEVEL);

	if (osal_bin_sem_create(&s_stopped_sem, "mg_stopped",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		goto err_mgr_free;

	if (osal_queue_create(&s_invoke_q, "mg_invoke", MONGOOSE_INVOKE_QUEUE_DEPTH,
			      sizeof(mongoose_invoke_item_t)) != OSAL_SUCCESS)
		goto err_stopped_sem_delete;

	if (osal_bin_sem_create(&s_invoke_done, "mg_invoke_done",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		goto err_invoke_q_delete;

	/* Control connection used to wake the poll thread for queued callbacks.
	 * Created on the calling thread before the poll task starts, so it is
	 * registered in the manager and available to receive MG_EV_WAKEUP. */
	s_control_nc = mg_listen(&mgr, "udp://127.0.0.1:0", invoke_control_cb, NULL);
	if (s_control_nc == NULL)
		goto err_invoke_done_delete;
	s_control_conn_id = s_control_nc->id;

	if (osal_task_create(&s_poll_task_id, "mg_poll", poll_task, NULL, NULL,
			     MONGOOSE_POLL_STACK_SIZE, 5, NULL) != OSAL_SUCCESS)
		goto err_control_close;

	if (s_state_mutex == NULL)
		(void)osal_mutex_create(&s_state_mutex, "mg_state");

	(void)osal_mutex_take(s_state_mutex);
	s_running = true;
	(void)osal_mutex_give(s_state_mutex);
	return;

err_control_close:
	s_control_nc     = NULL;
	s_control_conn_id = 0;
err_invoke_done_delete:
	(void)osal_bin_sem_delete(s_invoke_done);
	s_invoke_done = NULL;
err_invoke_q_delete:
	(void)osal_queue_delete(s_invoke_q);
	s_invoke_q = NULL;
err_stopped_sem_delete:
	(void)osal_bin_sem_delete(s_stopped_sem);
err_mgr_free:
	mg_mgr_free(&mgr);
}

void MongooseProcess_Deinit(void)
{
	if (!s_running)
		return;

	/* Reject new invocations while shutdown is in progress. */
	s_shutting_down = true;
	s_stop_requested = true;
	(void)osal_bin_sem_timed_wait(s_stopped_sem, 2000);
	(void)osal_task_delete(s_poll_task_id);
	(void)osal_bin_sem_delete(s_stopped_sem);

	/* Release invocation queue and semaphore resources. */
	if (s_invoke_q != NULL) {
		(void)osal_queue_delete(s_invoke_q);
		s_invoke_q = NULL;
	}
	if (s_invoke_done != NULL) {
		(void)osal_bin_sem_delete(s_invoke_done);
		s_invoke_done = NULL;
	}
	s_control_nc      = NULL;
	s_control_conn_id = 0;

	if (s_state_mutex != NULL)
		(void)osal_mutex_take(s_state_mutex);
	s_running = false;
	if (s_state_mutex != NULL)
		(void)osal_mutex_give(s_state_mutex);

	if (s_state_mutex != NULL) {
		(void)osal_mutex_delete(s_state_mutex);
		s_state_mutex = NULL;
	}

	s_shutting_down = false;
	mg_mgr_free(&mgr);
}

bool MongooseProcess_Invoke(mongoose_process_fn_t fn, void *user,
                            uint32_t timeout_ms)
{
	mongoose_invoke_item_t item;
	osal_status_t          status;
	uint32_t               my_id;
	uint32_t               start;
	uint32_t               deadline;

	if (fn == NULL)
		return false;

	if (!MongooseProcess_IsRunning() || s_shutting_down)
		return false;

	/* Not created if initialisation failed, or already released. */
	if (s_invoke_q == NULL || s_invoke_done == NULL)
		return false;

	my_id   = ++s_invoke_seq;
	item.fn = fn;
	item.user = user;
	item.id = my_id;

	/* The platform OSAL may ignore a blocking timeout on an empty semaphore,
	 * so enqueue without blocking and use an explicit poll loop below. */
	status = osal_queue_send(s_invoke_q, &item, 0);
	if (status != OSAL_SUCCESS)
		return false;

	if (s_control_conn_id != 0)
		mg_wakeup(&mgr, s_control_conn_id, NULL, 0);

	/* Wait for OUR callback to complete, honouring the caller timeout with a
	 * non-blocking poll so we never rely on a platform timed semaphore wait.
	 * Matching the per-invocation id guards against a stale completion signal
	 * left behind by an earlier invocation that timed out. */
	start = osal_task_get_time_ms();
	if (timeout_ms < (UINT32_MAX - start))
		deadline = start + timeout_ms;
	else
		deadline = UINT32_MAX;

	for (;;) {
		if (s_completed_id == my_id)
			return true;

		/* Detect wrap-safe deadline expiry. */
		if ((int32_t)(osal_task_get_time_ms() - deadline) >= 0)
			return false;

		/* Consume any completion signal; the id check above is authoritative. */
		(void)osal_bin_sem_timed_wait(s_invoke_done, 0);
		(void)osal_task_delay_ms(1);
	}
}

bool MongooseProcess_IsRunning(void)
{
	bool running;

	if (s_state_mutex != NULL) {
		(void)osal_mutex_take(s_state_mutex);
		running = s_running;
		(void)osal_mutex_give(s_state_mutex);
	} else {
		running = s_running;
	}

	return running;
}
