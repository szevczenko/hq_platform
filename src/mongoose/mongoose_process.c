#include "mongoose_process.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

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

static volatile bool             s_running        = false;
static volatile bool             s_stop_requested = false;
static volatile bool             s_shutting_down  = false;
static osal_bin_sem_id_t         s_stopped_sem;
static osal_task_id_t            s_poll_task_id;
static osal_mutex_id_t           s_state_mutex;

/* Each queued invocation is a heap-allocated record independently owned by the
 * caller and by the poll thread (refs starts at 2).  The caller owns one
 * reference while it is waiting, and releases it on completion or timeout; the
 * poll thread owns the other from the moment the item is queued until the
 * callback (or a shutdown cancellation) is drained.  Because the record --
 * including its completion semaphore -- is heap owned and reference counted, a
 * timed-out caller can never leave the poll thread with a dangling stack
 * pointer or a semaphore that the caller deleted. */
typedef struct mongoose_invoke_record {
	mongoose_process_fn_t fn;
	void                *user;
	osal_bin_sem_id_t     done_sem;   /* Caller's per-invocation completion */
	int32_t               refs;        /* 2 = caller + poll thread ownership */
	uint8_t               cancelled;   /* Set when the callback will not run */
} mongoose_invoke_record_t;

/* The queue holds a pointer to a record.  Ownership of the pointed-to record
 * transfers to the poll thread once the item is written into the queue. */
typedef struct {
	mongoose_invoke_record_t *rec;
} mongoose_invoke_item_t;

static osal_queue_id_t       s_invoke_q;
static struct mg_connection *s_control_nc;
static unsigned long         s_control_conn_id;

/* Drop one party's ownership of an invocation record.  The last owner releases
 * the completion semaphore and the heap block.  Atomic so a caller timing out
 * and a poll thread finishing an item cannot double-free the record. */
static void mongoose_invoke_record_release(mongoose_invoke_record_t *rec)
{
	if (rec == NULL)
		return;
	/* GCC built-in keeps the two-owner handshake race free on both POSIX and
	 * ESP-IDF (the vendored Mongoose already uses __sync_synchronize). */
	if (__sync_sub_and_fetch(&rec->refs, 1) == 0) {
		if (rec->done_sem != NULL)
			(void)osal_bin_sem_delete(rec->done_sem);
		free(rec);
	}
}

static void poll_task(void *arg)
{
	mongoose_invoke_item_t item;

	(void)arg;
	while (!s_stop_requested)
		mg_mgr_poll(&mgr, MONGOOSE_POLL_MS);

	/* Shutting down: complete (cancel) every record still queued so a caller
	 * that is still blocked in MongooseProcess_Invoke() observes a release
	 * and the record is freed exactly once.  This runs on the poll thread
	 * after the dispatch loop has exited, so no callback is mid-air while the
	 * queue is later reclaimed by the caller. */
	while (s_invoke_q != NULL &&
	       osal_queue_receive(s_invoke_q, &item, 0) == OSAL_SUCCESS) {
		if (item.rec != NULL) {
			item.rec->cancelled = 1;
			(void)osal_bin_sem_give(item.rec->done_sem);
			mongoose_invoke_record_release(item.rec);
		}
	}
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
		if (item.rec != NULL) {
			if (item.rec->fn != NULL)
				item.rec->fn(&mgr, item.rec->user);
			/* Wake exactly this one caller, then drop the poll thread's
			 * reference.  The record is freed when the caller also drops
			 * its reference, even if the caller already timed out. */
			(void)osal_bin_sem_give(item.rec->done_sem);
			mongoose_invoke_record_release(item.rec);
		}
	}
}

void MongooseProcess_Init(void)
{
	if (s_running)
		return;

	s_stop_requested  = false;
	s_shutting_down   = false;
	s_invoke_q        = NULL;
	s_control_nc      = NULL;
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

	/* Control connection used to wake the poll thread for queued callbacks. */
	s_control_nc = mg_listen(&mgr, "udp://127.0.0.1:0", invoke_control_cb, NULL);
	if (s_control_nc == NULL)
		goto err_invoke_q_delete;
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
	s_control_nc      = NULL;
	s_control_conn_id = 0;
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
	/* Never initialised, or already torn down: the state mutex is not created
	 * on a failed init, so bail before touching it. */
	if (s_state_mutex == NULL) {
		s_running = false;
		return;
	}

	/* Serialize with enqueue: take the lock, mark shutdown, release the lock
	 * so the poll thread can run and so an enqueuer already inside the state
	 * lock may finish handing its item to the poll thread. */
	(void)osal_mutex_take(s_state_mutex);
	if (!s_running) {
		(void)osal_mutex_give(s_state_mutex);
		return;
	}
	s_shutting_down  = true;
	s_stop_requested = true;
	(void)osal_mutex_give(s_state_mutex);

	/* The poll thread cancels every queued record before giving this sem.
	 * The bounded wait tolerates a stuck callback, but we only reclaim the
	 * queue after it has produced a final release for each pending caller. */
	(void)osal_bin_sem_timed_wait(s_stopped_sem, 2000);
	(void)osal_task_delete(s_poll_task_id);
	(void)osal_bin_sem_delete(s_stopped_sem);

	(void)osal_mutex_take(s_state_mutex);
	if (s_invoke_q != NULL) {
		(void)osal_queue_delete(s_invoke_q);
		s_invoke_q = NULL;
	}
	s_control_nc      = NULL;
	s_control_conn_id = 0;
	s_running         = false;
	/* Tear the manager down under the state lock so no enqueue can be midway
	 * through mg_wakeup() against a connection that is being freed. */
	mg_mgr_free(&mgr);
	(void)osal_mutex_give(s_state_mutex);

	s_shutting_down = false;
}

bool MongooseProcess_Invoke(mongoose_process_fn_t fn, void *user,
                            uint32_t timeout_ms)
{
	mongoose_invoke_item_t    item;
	mongoose_invoke_record_t *rec;
	uint32_t                  start;
	uint32_t                  deadline;
	bool                      success = false;

	/* Fast-path reject before alloc.  The locked re-check below is the
	 * authoritative guard that serializes against Deinit(). */
	if (fn == NULL)
		return false;
	if (s_state_mutex == NULL)
		return false;
	if (!s_running || s_shutting_down)
		return false;

	/* Build the per-invocation record first so a heap-owned completion object
	 * always exists for both parties.  Two references are kept: one for this
	 * caller and one for the poll thread that will dequeue the item. */
	rec = malloc(sizeof(*rec));
	if (rec == NULL)
		return false;
	rec->fn        = fn;
	rec->user      = user;
	rec->refs      = 2;
	rec->cancelled = 0;
	if (osal_bin_sem_create(&rec->done_sem, "mg_inv_done",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS) {
		free(rec);
		return false;
	}

	/* Serialize enqueue + wakeup with initialization and shutdown.  Holding
	 * the lock across queue insertion means Deinit() can never delete the
	 * queue (or the manager) while an item is being handed to the poll
	 * thread. */
	(void)osal_mutex_take(s_state_mutex);
	if (!s_running || s_shutting_down || s_invoke_q == NULL) {
		(void)osal_mutex_give(s_state_mutex);
		/* No item was queued, so neither party ultimately held a live
		 * reference: drop the caller and the intended poll ownership. */
		mongoose_invoke_record_release(rec);
		mongoose_invoke_record_release(rec);
		return false;
	}

	item.rec = rec;
	if (osal_queue_send(s_invoke_q, &item, 0) != OSAL_SUCCESS) {
		(void)osal_mutex_give(s_state_mutex);
		mongoose_invoke_record_release(rec);
		mongoose_invoke_record_release(rec);
		return false;
	}

	if (s_control_conn_id != 0)
		mg_wakeup(&mgr, s_control_conn_id, NULL, 0);
	(void)osal_mutex_give(s_state_mutex);

	/* Wait for OUR completion.  The poll thread wakes exactly this one record,
	 * so a timed-out caller can never consume another caller's completion. */
	start = osal_task_get_time_ms();
	if (timeout_ms < (UINT32_MAX - start))
		deadline = start + timeout_ms;
	else
		deadline = UINT32_MAX;

	for (;;) {
		if (osal_bin_sem_timed_wait(rec->done_sem, 0) == OSAL_SUCCESS) {
			/* The poll thread finalised exactly this record. */
			success = !rec->cancelled;
			break;
		}
		if ((int32_t)(osal_task_get_time_ms() - deadline) >= 0)
			break;
		(void)osal_task_delay_ms(1);
	}
	/* Drop the caller's ownership; the record lives on under the poll thread's
	 * reference until it is drained. */
	mongoose_invoke_record_release(rec);
	return success;
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