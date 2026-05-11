#include "mongoose_process.h"

#include <stdbool.h>

#include "hq_config.h"
#include "osal_bin_sem.h"
#include "osal_task.h"

#ifndef CONFIG_MONGOOSE_LOG_LEVEL
#define CONFIG_MONGOOSE_LOG_LEVEL 2
#endif

#define MONGOOSE_POLL_STACK_SIZE (OSAL_TASK_MIN_STACK_SIZE * 8)
#define MONGOOSE_POLL_MS         10

struct mg_mgr mgr;

static volatile bool     s_running        = false;
static volatile bool     s_stop_requested = false;
static osal_bin_sem_id_t s_stopped_sem;
static osal_task_id_t    s_poll_task_id;

static void poll_task(void *arg)
{
	(void)arg;
	while (!s_stop_requested)
		mg_mgr_poll(&mgr, MONGOOSE_POLL_MS);
	(void)osal_bin_sem_give(s_stopped_sem);
}

void MongooseProcess_Init(void)
{
	if (s_running)
		return;

	s_stop_requested = false;
	mg_mgr_init(&mgr);
	mg_log_set(CONFIG_MONGOOSE_LOG_LEVEL);

	if (osal_bin_sem_create(&s_stopped_sem, "mg_stopped",
				OSAL_SEM_EMPTY) != OSAL_SUCCESS)
		goto err_mgr_free;

	if (osal_task_create(&s_poll_task_id, "mg_poll", poll_task, NULL, NULL,
			     MONGOOSE_POLL_STACK_SIZE, 5, NULL) != OSAL_SUCCESS)
		goto err_sem_delete;

	s_running = true;
	return;

err_sem_delete:
	(void)osal_bin_sem_delete(s_stopped_sem);
err_mgr_free:
	mg_mgr_free(&mgr);
}

void MongooseProcess_Deinit(void)
{
	if (!s_running)
		return;

	s_stop_requested = true;
	(void)osal_bin_sem_timed_wait(s_stopped_sem, 2000);
	(void)osal_task_delete(s_poll_task_id);
	(void)osal_bin_sem_delete(s_stopped_sem);
	s_running = false;
	mg_mgr_free(&mgr);
}
