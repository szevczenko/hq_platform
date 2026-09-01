#ifndef MONGOOSE_PROCESS_H
#define MONGOOSE_PROCESS_H

#include <stdbool.h>
#include <stdint.h>

#include "mongoose.h"

extern struct mg_mgr mgr;

/**
 * @brief Callback executed on the Mongoose poll thread.
 * @param[in] mgr  Message manager that owns the poll thread.
 * @param[in] user Caller-provided context.
 */
typedef void (*mongoose_process_fn_t)(struct mg_mgr *mgr, void *user);

void MongooseProcess_Init(void);
void MongooseProcess_Deinit(void);
bool MongooseProcess_IsRunning(void);

/**
 * @brief Execute a callback on the Mongoose poll thread.
 *
 * The callback is queued and woken via mg_wakeup(), so it always runs on the
 * shared Mongoose poll thread. The caller blocks until the callback completes
 * or the timeout elapses.
 *
 * Each invocation carries its own heap-owned completion object, so concurrent
 * callers each observe only their own callback completion and a timed-out
 * caller can never consume (or miss) another caller's signal. When the process
 * is shutting down, a queued invocation is cancelled and the caller unblocks
 * with a false return.
 *
 * @param[in] fn         Callback to execute on the poll thread.
 * @param[in] user       Caller context passed to the callback.
 * @param[in] timeout_ms Maximum time to wait for completion.
 * @return true if the callback ran to completion, false if the process is not
 *         initialised, is shutting down, or the invocation timed out or was
 *         cancelled by shutdown.
 */
bool MongooseProcess_Invoke(mongoose_process_fn_t fn, void *user,
                            uint32_t timeout_ms);

#endif    /* MONGOOSE_PROCESS_H */
