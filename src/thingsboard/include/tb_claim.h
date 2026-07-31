/**
 *******************************************************************************
 * @file    tb_claim.h
 * @brief   ThingsBoard client – device claiming API
 *******************************************************************************
 */

#ifndef TB_CLAIM_H
#define TB_CLAIM_H

#include "tb_client.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send a device claiming request
 * @param client       Client handle
 * @param secret_key   Claiming secret key (NULL for no secret)
 * @param duration_ms  Claiming window duration in milliseconds
 * @return 0 on success, negative on error
 */
int tb_claim_device(tb_client_t *client, const char *secret_key,
                    uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* TB_CLAIM_H */
