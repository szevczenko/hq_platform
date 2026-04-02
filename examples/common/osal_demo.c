/*
 * OSAL Demo Application
 *
 * This example demonstrates core OSAL functionality:
 * - Task management (creating and managing multiple tasks)
 * - Message queue (inter-task communication)
 * - Binary semaphore (task synchronization)
 * - Software timer (periodic callbacks)
 *
 * Scenario: Sensor data processing pipeline
 * - Producer task: Simulates sensor readings and sends to queue
 * - Consumer task: Receives readings and processes them
 * - Monitor task: Prints statistics
 * - Timer: Periodic status updates
 * - Semaphore: Synchronizes monitoring
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osal_task.h"
#include "osal_bin_sem.h"
#include "osal_queue.h"
#include "osal_timer.h"
#include "osal_log.h"

#define LOG_DEBUG(fmt, ...)     osal_log_debug(fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)      osal_log_info(fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...)   osal_log_warning(fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)     osal_log_error(fmt, ##__VA_ARGS__)

typedef struct {
    uint32_t timestamp;
    uint16_t sensor_id;
    int16_t temperature;
    uint16_t humidity;
} sensor_reading_t;

static struct {
    uint32_t readings_produced;
    uint32_t readings_processed;
    uint32_t timer_expires;
    volatile uint32_t app_time;
} app_state = {0};

static osal_queue_id_t data_queue;
static osal_bin_sem_id_t monitor_semaphore;
static osal_timer_id_t status_timer;

#define TASK_STACK_SIZE  (64 * 1024)

static void producer_task_func(void *arg)
{
    (void)arg;
    uint16_t sensor_id = 1;
    sensor_reading_t reading;
    osal_status_t status;

    LOG_INFO("Producer: Starting sensor simulation");

    while (1) {
        reading.timestamp = osal_task_get_time_ms();
        reading.sensor_id = sensor_id;
        reading.temperature = 20 * 10 + (rand() % 100) - 50;
        reading.humidity = 60 * 10 + (rand() % 200) - 100;

        status = osal_queue_send(data_queue, &reading, 100);

        if (status == OSAL_SUCCESS) {
            app_state.readings_produced++;
            LOG_DEBUG("Producer: Sent reading #%u (T=%.1fC, H=%.1f%%)",
                      app_state.readings_produced,
                      reading.temperature / 10.0,
                      reading.humidity / 10.0);
        } else if (status == OSAL_QUEUE_FULL) {
            LOG_WARNING("Producer: Queue full, dropping reading");
        } else {
            LOG_ERROR("Producer: Send failed with status %d", status);
        }

        osal_task_delay_ms(500);
    }
}

static void consumer_task_func(void *arg)
{
    (void)arg;
    sensor_reading_t reading;
    osal_status_t status;

    int32_t temp_sum = 0;
    int32_t humidity_sum = 0;
    uint32_t count = 0;

    LOG_INFO("Consumer: Starting data processing");

    while (1) {
        status = osal_queue_receive(data_queue, &reading, 2000);

        if (status == OSAL_SUCCESS) {
            app_state.readings_processed++;
            count++;
            temp_sum += reading.temperature;
            humidity_sum += reading.humidity;

            LOG_DEBUG("Consumer: Processing reading #%u from sensor %u",
                      app_state.readings_processed,
                      reading.sensor_id);

            if (count >= 10) {
                int16_t avg_temp = temp_sum / count;
                uint16_t avg_humidity = humidity_sum / count;

                LOG_INFO("Consumer: Statistics (10 samples) - Avg Temp: %.1fC, Avg Humidity: %.1f%%",
                         avg_temp / 10.0,
                         avg_humidity / 10.0);

                temp_sum = 0;
                humidity_sum = 0;
                count = 0;

                osal_bin_sem_give(monitor_semaphore);
            }
        } else if (status == OSAL_QUEUE_TIMEOUT) {
            LOG_WARNING("Consumer: Queue receive timeout (no data for 2s)");
        } else {
            LOG_ERROR("Consumer: Receive failed with status %d", status);
        }
    }
}

static void monitor_task_func(void *arg)
{
    (void)arg;
    osal_status_t status;

    LOG_INFO("Monitor: Starting statistics monitor");

    while (1) {
        status = osal_bin_sem_take(monitor_semaphore);

        if (status == OSAL_SUCCESS) {
            LOG_INFO("Monitor: === APPLICATION STATISTICS ===");
            LOG_INFO("Monitor: Readings produced: %u", app_state.readings_produced);
            LOG_INFO("Monitor: Readings processed: %u", app_state.readings_processed);
            LOG_INFO("Monitor: Timer expirations: %u", app_state.timer_expires);
            LOG_INFO("Monitor: System uptime: %u ms", osal_task_get_time_ms());
            LOG_INFO("Monitor: ==================================");
        } else {
            LOG_ERROR("Monitor: Semaphore take failed with status %d", status);
        }
    }
}

static void status_timer_callback(osal_timer_id_t timer_id)
{
    (void)timer_id;

    app_state.timer_expires++;

    LOG_WARNING("Timer[%p]: Periodic notification #%u",
                (void *)timer_id,
                app_state.timer_expires);
}

static int app_init(void)
{
    osal_status_t status;
    osal_task_id_t task_id;
    osal_task_attr_t task_attr;

    LOG_INFO("App: Initializing OSAL demo application");

    status = osal_queue_create(&data_queue, "sensor_data_queue",
                               20,
                               sizeof(sensor_reading_t));
    if (status != OSAL_SUCCESS) {
        LOG_ERROR("App: Failed to create queue (status %d)", status);
        return -1;
    }
    LOG_INFO("App: Data queue created");

    status = osal_bin_sem_create(&monitor_semaphore, "monitor_signal",
                                 OSAL_SEM_EMPTY);
    if (status != OSAL_SUCCESS) {
        LOG_ERROR("App: Failed to create semaphore (status %d)", status);
        return -1;
    }
    LOG_INFO("App: Monitor semaphore created");

    status = osal_timer_create(&status_timer, "status_timer",
                               3000,
                               true,
                               status_timer_callback,
                               NULL,
                               NULL,
                               0);
    if (status != OSAL_SUCCESS) {
        LOG_ERROR("App: Failed to create timer (status %d)", status);
        return -1;
    }
    LOG_INFO("App: Periodic timer created");

    status = osal_task_attributes_init(&task_attr);
    if (status != OSAL_SUCCESS) {
        LOG_ERROR("App: Failed to init task attributes (status %d)", status);
        return -1;
    }

    status = osal_task_create(&task_id, "producer_task",
                              producer_task_func, NULL,
                              NULL,
                              TASK_STACK_SIZE,
                              10,
                              &task_attr);
    if (status != OSAL_SUCCESS) {
        LOG_ERROR("App: Failed to create producer task (status %d)", status);
        return -1;
    }
    LOG_INFO("App: Producer task created (ID: %p)", (void *)task_id);

    status = osal_task_create(&task_id, "consumer_task",
                              consumer_task_func, NULL,
                              NULL,
                              TASK_STACK_SIZE,
                              10,
                              &task_attr);
    if (status != OSAL_SUCCESS) {
        LOG_ERROR("App: Failed to create consumer task (status %d)", status);
        return -1;
    }
    LOG_INFO("App: Consumer task created (ID: %p)", (void *)task_id);

    status = osal_task_create(&task_id, "monitor_task",
                              monitor_task_func, NULL,
                              NULL,
                              TASK_STACK_SIZE,
                              5,
                              &task_attr);
    if (status != OSAL_SUCCESS) {
        LOG_ERROR("App: Failed to create monitor task (status %d)", status);
        return -1;
    }
    LOG_INFO("App: Monitor task created (ID: %p)", (void *)task_id);

    status = osal_timer_start(status_timer, 1000);
    if (status != OSAL_SUCCESS) {
        LOG_ERROR("App: Failed to start timer (status %d)", status);
        return -1;
    }
    LOG_INFO("App: Status timer started");
    LOG_INFO("App: Initialization complete - all tasks running");

    return 0;
}

#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(int argc, char *argv[])
#endif
{
#ifndef ESP_PLATFORM
    (void)argc;
    (void)argv;
#endif

    printf("\n");
    printf("=============================================================\n");
    printf("           OSAL Demo Application - Sensor Pipeline           \n");
    printf("=============================================================\n\n");
    printf("Features Demonstrated:\n");
    printf("  - Task Management: 3 concurrent tasks (producer, consumer, monitor)\n");
    printf("  - Message Queue: Producer -> Consumer communication\n");
    printf("  - Binary Semaphore: Consumer -> Monitor synchronization\n");
    printf("  - Software Timer: Periodic status notifications\n\n");

    if (app_init() != 0) {
        LOG_ERROR("Main: Application initialization failed");
#ifndef ESP_PLATFORM
        return 1;
#else
        return;
#endif
    }

    LOG_INFO("Main: Entering idle loop (application will run indefinitely)");
#ifndef ESP_PLATFORM
    LOG_INFO("Main: Press Ctrl-C to exit");
#endif

    while (1) {
        osal_task_delay_ms(5000);
        LOG_DEBUG("Main: Still running... (up %u ms)", osal_task_get_time_ms());
    }

#ifndef ESP_PLATFORM
    return 0;
#endif
}