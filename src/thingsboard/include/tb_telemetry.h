/**
 *******************************************************************************
 * @file    tb_telemetry.h
 * @brief   ThingsBoard client – telemetry API
 *******************************************************************************
 */

#ifndef TB_TELEMETRY_H
#define TB_TELEMETRY_H

#include "tb_client.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send integer telemetry
 */
int tb_telemetry_send_int(tb_client_t *client, const char *key, int64_t value);

/**
 * @brief Send floating-point telemetry
 */
int tb_telemetry_send_double(tb_client_t *client, const char *key, double value);

/**
 * @brief Send boolean telemetry
 */
int tb_telemetry_send_bool(tb_client_t *client, const char *key, bool value);

/**
 * @brief Send string telemetry
 */
int tb_telemetry_send_string(tb_client_t *client, const char *key, const char *value);

/**
 * @brief Send raw JSON telemetry string
 */
int tb_telemetry_send_json(tb_client_t *client, const char *json);

#ifdef __cplusplus
}
#endif

#endif /* TB_TELEMETRY_H */
