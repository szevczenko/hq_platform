/**
 *******************************************************************************
 * @file    wifi_config.h
 * @author  Dmytro Shevchenko
 * @brief   Wi-Fi configuration persistence — credential list with rotation
 *
 *          Stores up to @c WIFI_CONFIG_MAX_CREDENTIALS entries in
 *          @c WIFI_CONFIG_FILE_PATH.  Each entry carries a monotonically
 *          increasing sequence number (@c nb).  When the list is full the
 *          entry with the lowest @c nb is replaced.  When the counter
 *          reaches 255 all entries are renumbered starting from 0.
 *
 *          JSON format:
 *          @code
 *          {
 *            "last_use": <nb>,
 *            "credentials": [
 *              { "nb": 0, "ssid": "...", "password": "..." },
 *              ...
 *            ]
 *          }
 *          @endcode
 *******************************************************************************
 */

/* Define to prevent recursive inclusion ------------------------------------*/
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include "osal_error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Public macros -------------------------------------------------------------*/

/** @brief Path to the JSON file used for credential persistence. */
#define WIFI_CONFIG_FILE_PATH        "wifi_ap.json"

/** @brief Maximum number of stored credential entries. */
#define WIFI_CONFIG_MAX_CREDENTIALS  8

/** @brief Maximum SSID length stored in config (without null terminator). */
#define WIFI_CONFIG_SSID_MAX_LEN     32

/** @brief Maximum password length stored in config (without null terminator). */
#define WIFI_CONFIG_PASSWORD_MAX_LEN 64

/* Public types --------------------------------------------------------------*/

/**
 * @brief   A single credential entry.
 *
 * The @c nb field is a monotonically increasing sequence number used
 * to determine insertion order.  The entry with the lowest @c nb is
 * the oldest and will be evicted first when the list is full.
 */
typedef struct
{
  uint8_t nb;                                      /**< Sequence number.  */
  char    ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];      /**< Network SSID.     */
  char    password[WIFI_CONFIG_PASSWORD_MAX_LEN + 1]; /**< Network password. */
} wifi_config_entry_t;

/**
 * @brief   Complete credential list loaded from / saved to persistent storage.
 *
 * @c last_use holds the @c nb of the credential that was last successfully
 * connected.  The management layer uses it to pick the initial credential
 * to try on startup.
 */
typedef struct
{
  uint8_t              last_use;                               /**< @c nb of last-used credential. */
  uint8_t              count;                                  /**< Number of valid entries.        */
  wifi_config_entry_t  entries[WIFI_CONFIG_MAX_CREDENTIALS];   /**< Credential array.               */
} wifi_config_list_t;

/* Public functions ----------------------------------------------------------*/

/**
 * @brief   Load the credential list from the JSON config file.
 * @param   [out] list - receives the credential list on success
 * @return  OSAL_SUCCESS if the file was read and parsed, error code otherwise
 */
osal_status_t wifi_config_load( wifi_config_list_t* list );

/**
 * @brief   Save the credential list to the JSON config file.
 * @param   [in] list - credential list to persist
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_config_save( const wifi_config_list_t* list );

/**
 * @brief   Add or update a credential in the list.
 *
 *          If an entry with the same SSID already exists its password and
 *          @c nb are updated.  If the list is full the oldest entry (lowest
 *          @c nb) is evicted.  When the next @c nb would exceed 255 all
 *          entries are renumbered starting from 0 before the new entry is
 *          assigned.
 *
 * @param   [in,out] list - credential list to modify
 * @param   [in]     ssid - null-terminated SSID
 * @param   [in]     password - null-terminated password
 * @return  OSAL_SUCCESS on success, error code otherwise
 */
osal_status_t wifi_config_add_credential( wifi_config_list_t* list,
                                          const char*         ssid,
                                          const char*         password );

/**
 * @brief   Find the credential entry whose @c nb matches @p nb.
 * @param   [in]  list  - credential list to search
 * @param   [in]  nb    - sequence number to look up
 * @param   [out] entry - receives a copy of the matching entry
 * @return  true if found, false otherwise
 */
bool wifi_config_get_by_nb( const wifi_config_list_t* list,
                            uint8_t                   nb,
                            wifi_config_entry_t*      entry );

/**
 * @brief   Get the credential that should be tried after a failed attempt.
 *
 *          Returns the entry whose @c nb is the next higher value after
 *          @p current_nb (wrapping around to the lowest @c nb when the
 *          end of the list is reached).
 *
 * @param   [in]  list       - credential list
 * @param   [in]  current_nb - @c nb of the credential that just failed
 * @param   [out] entry      - receives a copy of the next entry
 * @return  true if a next entry was found, false otherwise (empty list)
 */
bool wifi_config_get_next( const wifi_config_list_t* list,
                           uint8_t                   current_nb,
                           wifi_config_entry_t*      entry );

#endif
