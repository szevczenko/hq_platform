/*
 * MongooseProcess_Invoke() unit tests (POSIX).
 *
 * Validates that the poll-thread invocation queue:
 *  - rejects invocations before init and after deinit,
 *  - runs callbacks on the Mongoose poll thread,
 *  - passes the caller context through to the callback,
 *  - supports repeated sequential invocations,
 *  - survives a caller timeout without leaking OSAL objects,
 *  - isolates concurrent callers so each observes only its own completion,
 *  - lets a timed-out caller be followed by successful independent work,
 *  - shuts down cleanly while invocations are still pending,
 *  - can be re-initialised cleanly.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "mongoose_process.h"
#include "osal_bin_sem.h"
#include "osal_task.h"
#include "unity.h"

#ifdef ESP_PLATFORM
#error "mongoose_process_test.c targets POSIX only"
#endif

static int            g_callback_count;
static void          *g_captured_user;
static pthread_t      g_main_thread;
static pthread_t      g_callback_thread;
static osal_bin_sem_id_t g_block_sem;
static osal_bin_sem_id_t g_pause_started;
static void          *g_expected_user;

static void counting_cb(struct mg_mgr *mgr, void *user)
{
	(void)mgr;
	(void)user;
	g_callback_count++;
	g_callback_thread = pthread_self();
}

static void blocking_cb(struct mg_mgr *mgr, void *user)
{
	(void)mgr;
	(void)user;
	g_callback_count++;
	/* Block on the poll thread until the test releases this callback. */
	while (osal_bin_sem_take(g_block_sem) != OSAL_SUCCESS) {
	}
}

static void quick_cb(struct mg_mgr *mgr, void *user)
{
	(void)mgr;
	(void)user;
	g_callback_count++;
}

static void capture_context_cb(struct mg_mgr *mgr, void *user)
{
	(void)mgr;
	g_callback_count++;
	g_captured_user = user;
}

void setUp(void)
{
	g_callback_count = 0;
	g_captured_user  = NULL;
	g_expected_user  = (void *)(uintptr_t)0x5A5A5A5A;
	g_main_thread    = pthread_self();
	(void)osal_bin_sem_create(&g_block_sem, "mg_test_block", OSAL_SEM_EMPTY);
	(void)osal_bin_sem_create(&g_pause_started, "mg_test_pause_start",
				 OSAL_SEM_EMPTY);
}

void tearDown(void)
{
	/* Make sure no callback is left blocking the poll thread. */
	(void)osal_bin_sem_give(g_block_sem);
	if (g_block_sem != NULL) {
		(void)osal_bin_sem_delete(g_block_sem);
		g_block_sem = NULL;
	}
	if (g_pause_started != NULL) {
		(void)osal_bin_sem_delete(g_pause_started);
		g_pause_started = NULL;
	}
	MongooseProcess_Deinit();
}

static void test_invoke_before_init_is_rejected(void)
{
	/* setUp does not initialise the process, so this must fail cleanly. */
	TEST_ASSERT_FALSE(MongooseProcess_IsRunning());
	TEST_ASSERT_FALSE_MESSAGE(MongooseProcess_Invoke(quick_cb, NULL, 100),
				  "Invoke before init must fail cleanly");
}

static void test_invoke_after_deinit_is_rejected(void)
{
	MongooseProcess_Init();
	MongooseProcess_Deinit();
	TEST_ASSERT_FALSE_MESSAGE(MongooseProcess_Invoke(quick_cb, NULL, 100),
				  "Invoke after deinit must fail cleanly");
}

static void test_invoke_runs_on_poll_thread(void)
{
	MongooseProcess_Init();
	TEST_ASSERT_TRUE(MongooseProcess_IsRunning());
	TEST_ASSERT_TRUE_MESSAGE(MongooseProcess_Invoke(counting_cb, NULL, 2000),
				"First invoke completed");
	TEST_ASSERT_EQUAL_INT(1, g_callback_count);
	TEST_ASSERT_EQUAL_INT_MESSAGE(
		0, pthread_equal(g_main_thread, g_callback_thread),
		"Callback must run on a thread other than the caller");
}

static void test_repeated_sequential_invocations(void)
{
	MongooseProcess_Init();
	for (int i = 0; i < 10; i++) {
		TEST_ASSERT_TRUE_MESSAGE(MongooseProcess_Invoke(counting_cb, NULL, 2000),
					 "Sequential invoke failed");
	}
	TEST_ASSERT_EQUAL_INT(10, g_callback_count);
}

static void test_invoke_passes_context(void)
{
	MongooseProcess_Init();
	TEST_ASSERT_TRUE_MESSAGE(MongooseProcess_Invoke(capture_context_cb,
						       g_expected_user, 1000),
				"Invoke with a caller context failed");
	TEST_ASSERT_EQUAL_INT(1, g_callback_count);
	TEST_ASSERT_EQUAL_PTR_MESSAGE(g_expected_user, g_captured_user,
				      "Callback must receive the caller context");
}

static void test_timeout_does_not_break_invocations(void)
{
	MongooseProcess_Init();
	/* The blocking callback holds the poll thread past the short timeout, so
	 * the caller must observe a timeout while the callback is still pending. */
	TEST_ASSERT_FALSE_MESSAGE(MongooseProcess_Invoke(blocking_cb, NULL, 50),
				  "Blocking callback must time out");
	/* Release the pending callback so the poll thread can proceed. */
	(void)osal_bin_sem_give(g_block_sem);
	/* A later invocation must still complete (no leaked/mis-synced objects). */
	TEST_ASSERT_TRUE_MESSAGE(MongooseProcess_Invoke(quick_cb, NULL, 4000),
				"Invoke after timeout succeeded");
	/* The blocking callback also ran once it was released. */
	TEST_ASSERT_EQUAL_INT(2, g_callback_count);
}

static void test_reinit_cycle_is_clean(void)
{
	MongooseProcess_Init();
	TEST_ASSERT_TRUE(MongooseProcess_Invoke(counting_cb, NULL, 1000));
	MongooseProcess_Deinit();
	TEST_ASSERT_FALSE(MongooseProcess_Invoke(counting_cb, NULL, 50));

	MongooseProcess_Init();
	TEST_ASSERT_TRUE(MongooseProcess_IsRunning());
	TEST_ASSERT_TRUE(MongooseProcess_Invoke(counting_cb, NULL, 1000));
	TEST_ASSERT_EQUAL_INT(2, g_callback_count);
}

/* --------------------------------------------------------------------------
 * Concurrent invocation helpers.  Each worker passes its own context so the
 * callback records completion only for the caller it belongs to.  A caller
 * that returns true must be the only thread that consumed its own signal.
 * ------------------------------------------------------------------------ */

#define G_CONCURRENT_THREADS   6
#define G_CONCURRENT_CALLS     8

typedef struct {
	int ran;     /* Incremented only by callbacks carrying this context. */
	int success; /* Incremented only by the worker that supplied it. */
} concurrent_ctx_t;

static concurrent_ctx_t g_concurrent_ctxs[G_CONCURRENT_THREADS];
static volatile int     g_concurrent_total_ran;
static pthread_mutex_t  g_concurrent_mutex = PTHREAD_MUTEX_INITIALIZER;

static void concurrent_recording_cb(struct mg_mgr *mgr, void *user)
{
	(void)mgr;
	if (user != NULL) {
		concurrent_ctx_t *ctx = (concurrent_ctx_t *)user;
		(void)pthread_mutex_lock(&g_concurrent_mutex);
		ctx->ran++;
		g_concurrent_total_ran++;
		(void)pthread_mutex_unlock(&g_concurrent_mutex);
	}
}

static void *concurrent_worker_thread(void *arg)
{
	concurrent_ctx_t *ctx = (concurrent_ctx_t *)arg;
	for (int i = 0; i < G_CONCURRENT_CALLS; i++)
		if (MongooseProcess_Invoke(concurrent_recording_cb, ctx, 2000))
			ctx->success++;
	return NULL;
}

static void test_concurrent_callers_observe_own_completion(void)
{
	pthread_t threads[G_CONCURRENT_THREADS];

	MongooseProcess_Init();
	g_concurrent_total_ran = 0;
	for (int i = 0; i < G_CONCURRENT_THREADS; i++) {
		g_concurrent_ctxs[i].ran     = 0;
		g_concurrent_ctxs[i].success = 0;
	}

	for (int i = 0; i < G_CONCURRENT_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT_MESSAGE(
			0, pthread_create(&threads[i], NULL, concurrent_worker_thread,
					  &g_concurrent_ctxs[i]),
			"pthread_create failed");
	}
	for (int i = 0; i < G_CONCURRENT_THREADS; i++)
		pthread_join(threads[i], NULL);

	/* Every worker's completion must have been delivered back to its own
	 * context, never lost onto or stolen from another caller. */
	for (int i = 0; i < G_CONCURRENT_THREADS; i++) {
		TEST_ASSERT_EQUAL_INT_MESSAGE(
			G_CONCURRENT_CALLS, g_concurrent_ctxs[i].success,
			"Caller missed its own completion");
		TEST_ASSERT_EQUAL_INT_MESSAGE(
			G_CONCURRENT_CALLS, g_concurrent_ctxs[i].ran,
			"Callback ran on a context other than the caller's");
	}
	TEST_ASSERT_EQUAL_INT(G_CONCURRENT_THREADS * G_CONCURRENT_CALLS,
			      g_concurrent_total_ran);
}

/* --------------------------------------------------------------------------
 * A timed-out caller followed by successful independent work.  A callback
 * that holds the poll thread past the caller's timeout is released only after
 * the caller has timed out, then a fresh invocation must still succeed.
 * ------------------------------------------------------------------------ */

static osal_bin_sem_id_t g_pause_started;
static int               g_timedout_result;

static void started_blocking_cb(struct mg_mgr *mgr, void *user)
{
	(void)mgr;
	(void)user;
	g_callback_count++;
	(void)osal_bin_sem_give(g_pause_started);
	while (osal_bin_sem_take(g_block_sem) != OSAL_SUCCESS) {
	}
}

static void *timed_out_worker_thread(void *arg)
{
	(void)arg;
	/* The 850 ms timeout elapses while the callback is pinned below. */
	g_timedout_result =
		MongooseProcess_Invoke(started_blocking_cb, NULL, 850) ? 1 : 0;
	return NULL;
}

static void test_timed_out_caller_followed_by_success(void)
{
	pthread_t th;

	MongooseProcess_Init();
	g_timedout_result = 1;
	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, timed_out_worker_thread, NULL));

	/* Wait until the callback has pinned the poll thread. */
	(void)osal_bin_sem_timed_wait(g_pause_started, 2000);
	/* Let the caller's own timeout elapse while its callback is still pinned. */
	(void)osal_task_delay_ms(900);
	/* Release the pinned callback so the poll thread may proceed. */
	(void)osal_bin_sem_give(g_block_sem);
	pthread_join(th, NULL);

	/* The timed-out caller observed only its own timeout, never a success. */
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_timedout_result,
				      "Caller incorrectly reported success");
	/* A subsequent, independent invocation still completes cleanly. */
	TEST_ASSERT_TRUE_MESSAGE(MongooseProcess_Invoke(quick_cb, NULL, 2000),
				"Successful work after a timed-out caller failed");
	TEST_ASSERT_EQUAL_INT(2, g_callback_count);
}

/* --------------------------------------------------------------------------
 * Shutdown while invocations are pending: a worker keeps invoking as Deinit
 * drains the queue, so records cancelled by shutdown are freed exactly once.
 * ------------------------------------------------------------------------ */

static volatile int g_deinit_worker_finished;
static volatile int g_deinit_ok;
static volatile int g_deinit_fail;

static void *deinit_worker_thread(void *arg)
{
	(void)arg;
	while (MongooseProcess_IsRunning()) {
		if (MongooseProcess_Invoke(quick_cb, NULL, 200))
			g_deinit_ok++;
		else
			g_deinit_fail++;
	}
	g_deinit_worker_finished = 1;
	return NULL;
}

static void test_deinit_while_calls_pending(void)
{
	pthread_t th;

	MongooseProcess_Init();
	g_deinit_worker_finished = 0;
	g_deinit_ok              = 0;
	g_deinit_fail            = 0;

	TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, deinit_worker_thread, NULL));
	/* Let the worker queue at least one round of invocations. */
	(void)osal_task_delay_ms(80);
	/* Shut the process down while the worker still has calls in flight. */
	MongooseProcess_Deinit();
	pthread_join(th, NULL);

	TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_deinit_worker_finished,
				      "Deinit worker never returned");
	/* The worker must have completed or had a call cancelled by shutdown; it
	 * must not have been starved silently while Deinit was in progress. */
	TEST_ASSERT_TRUE_MESSAGE(g_deinit_ok + g_deinit_fail > 0,
				 "Deinit worker performed no invocations");
}

static void mongoose_process_tests_run(void)
{
	RUN_TEST(test_invoke_before_init_is_rejected);
	RUN_TEST(test_invoke_after_deinit_is_rejected);
	RUN_TEST(test_invoke_runs_on_poll_thread);
	RUN_TEST(test_invoke_passes_context);
	RUN_TEST(test_repeated_sequential_invocations);
	RUN_TEST(test_timeout_does_not_break_invocations);
	RUN_TEST(test_reinit_cycle_is_clean);
	RUN_TEST(test_concurrent_callers_observe_own_completion);
	RUN_TEST(test_timed_out_caller_followed_by_success);
	RUN_TEST(test_deinit_while_calls_pending);
}

#ifdef ESP_PLATFORM
void app_main(void)
{
	UNITY_BEGIN();
	mongoose_process_tests_run();
	UNITY_END();
}
#else
int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	UNITY_BEGIN();
	mongoose_process_tests_run();
	return UNITY_END();
}
#endif