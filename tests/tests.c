/*
 * OSAL Aggregated Tests Runner
 *
 * Runs all OSAL tests located in tests/osal.
 */

#include <stdio.h>

int osal_task_tests_run(void);
int osal_sync_tests_run(void);
int osal_queue_tests_run(void);
int osal_timer_tests_run(void);

#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(void)
#endif
{
    int failed_total = 0;

    printf("\n==================================================\n");
    printf("            OSAL Aggregated Test Run             \n");
    printf("==================================================\n\n");

    failed_total += osal_task_tests_run();
    failed_total += osal_sync_tests_run();
    failed_total += osal_queue_tests_run();
    failed_total += osal_timer_tests_run();

    printf("\n==================================================\n");
    printf("              AGGREGATED SUMMARY                 \n");
    printf("==================================================\n");
    printf("  Total failed tests: %d\n", failed_total);
    printf("==================================================\n");

#ifndef ESP_PLATFORM
    return (failed_total == 0) ? 0 : 1;
#endif
}
