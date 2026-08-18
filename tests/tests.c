/*
 * OSAL Aggregated Tests Runner
 *
 * Runs all OSAL tests located in tests/osal.
 */

#include <stdio.h>

#include "unity.h"

void osal_task_tests_run(void);
void osal_sync_tests_run(void);
void osal_queue_tests_run(void);
void osal_timer_tests_run(void);
void osal_file_tests_run(void);
void osal_mount_tests_run(void);
void osal_dir_tests_run(void);

void setUp(void)
{
}

void tearDown(void)
{
}

#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(void)
#endif
{
    UNITY_BEGIN();

    osal_task_tests_run();
    osal_sync_tests_run();
    osal_queue_tests_run();
    osal_timer_tests_run();
    osal_mount_tests_run();
    osal_file_tests_run();
    osal_dir_tests_run();

#ifndef ESP_PLATFORM
    return UNITY_END();
#else
    UNITY_END();
#endif
}
