/**
 *******************************************************************************
 * @file    wifi_utils.h
 * @author  Dmytro Shevchenko
 * @brief   Wi-Fi utility functions — JSON serialization of management structs
 *******************************************************************************
 */

/* Define to prevent recursive inclusion ------------------------------------*/
#ifndef WIFI_UTILS_H
#define WIFI_UTILS_H

#include "osal_error.h"
#include "wifi_managment.h"
#include <stddef.h>
#include <stdint.h>

/* Public macros -------------------------------------------------------------*/

/** @brief Maximum size in bytes of a JSON buffer for IP info (including '\n'). */
#define WIFI_UTILS_IP_JSON_MAX_SIZE 192

/** @brief Maximum size in bytes of a JSON buffer for the full AP list. */
#define WIFI_UTILS_AP_JSON_MAX_SIZE 4096

/* Public functions ----------------------------------------------------------*/

/**
 * @brief   Serialize structured IP information into a JSON string.
 * @param   [in]  info          - IP info struct to serialize
 * @param   [out] out_json      - buffer to receive the null-terminated JSON
 * @param   [in]  out_json_size - size of @p out_json in bytes
 * @return  OSAL_SUCCESS on success, @c OSAL_ERR_OUTPUT_TOO_LARGE if the buffer
 *          is too small, or an error code on failure
 */
osal_status_t wifi_utils_ip_info_to_json( const wifi_mgmt_ip_info_t* info,
                                          char*  out_json,
                                          size_t out_json_size );

/**
 * @brief   Serialize a structured AP list into a JSON array string.
 * @param   [in]  list          - AP list struct to serialize
 * @param   [out] out_json      - buffer to receive the null-terminated JSON
 * @param   [in]  out_json_size - size of @p out_json in bytes
 * @return  OSAL_SUCCESS on success, @c OSAL_ERR_OUTPUT_TOO_LARGE if the buffer
 *          is too small, or an error code on failure
 */
osal_status_t wifi_utils_ap_list_to_json( const wifi_mgmt_ap_list_t* list,
                                          char*  out_json,
                                          size_t out_json_size );

#endif
