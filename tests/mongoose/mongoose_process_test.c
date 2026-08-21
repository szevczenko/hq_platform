/*
 * MongooseProcess_Invoke() unit tests (POSIX).
 *
 * Validates that the poll-thread invocation queue:
 *  - rejects invocations before init and after deinit,
 *  - runs callbacks on the Mongoose poll thread,
 *  - passes the caller context through to the callback,
 *  - supports repeated sequential invocations,
 *  - survives a caller timeout without leaking OSAL objects,
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
}

void tearDown(void)
{
	/* Make sure no callback is left blocking the poll thread. */
	(void)osal_bin_sem_give(g_block_sem);
	if (g_block_sem != NULL) {
		(void)osal_bin_sem_delete(g_block_sem);
		g_block_sem = NULL;
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

static void mongoose_process_tests_run(void)
{
	RUN_TEST(test_invoke_before_init_is_rejected);
	RUN_TEST(test_invoke_after_deinit_is_rejected);
	RUN_TEST(test_invoke_runs_on_poll_thread);
	RUN_TEST(test_invoke_passes_context);
	RUN_TEST(test_repeated_sequential_invocations);
	RUN_TEST(test_timeout_does_not_break_invocations);
	RUN_TEST(test_reinit_cycle_is_clean);
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