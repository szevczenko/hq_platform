#include "wifi_config.h"

#include "cJSON.h"
#include "osal_file.h"
#include "osal_log.h"
#include <stdlib.h>
#include <string.h>

#define WIFI_CONFIG_MAX_JSON_SIZE 4096
#define WIFI_CONFIG_NB_MAX        255

/* -------------------------------------------------------------------------- */
/*  File I/O helpers                                                          */
/* -------------------------------------------------------------------------- */

static osal_status_t _read_file( const char* path, char** out_buf )
{
  if ( !path || !out_buf )
  {
    return OSAL_INVALID_POINTER;
  }

  osal_fstat_t stat = { 0 };
  int32_t stat_rc = osal_stat( path, &stat );
  if ( stat_rc != OSAL_SUCCESS )
  {
    return (osal_status_t) stat_rc;
  }

  if ( stat.file_size == 0 || stat.file_size > WIFI_CONFIG_MAX_JSON_SIZE )
  {
    return OSAL_ERR_OUTPUT_TOO_LARGE;
  }

  osal_file_id_t fd = osal_open_create( path, OSAL_FILE_FLAG_NONE, OSAL_READ_ONLY );
  if ( fd < 0 )
  {
    return (osal_status_t) fd;
  }

  char* buf = (char*) calloc( 1u, stat.file_size + 1u );
  if ( !buf )
  {
    (void) osal_close( fd );
    return OSAL_ERROR;
  }

  int32_t read_rc = osal_read( fd, buf, stat.file_size );
  (void) osal_close( fd );
  if ( read_rc < 0 )
  {
    free( buf );
    return (osal_status_t) read_rc;
  }

  buf[read_rc] = '\0';
  *out_buf = buf;
  return OSAL_SUCCESS;
}

static osal_status_t _write_file( const char* path, const char* data, size_t len )
{
  osal_file_id_t fd = osal_open_create( path,
                                        OSAL_FILE_FLAG_CREATE | OSAL_FILE_FLAG_TRUNCATE,
                                        OSAL_WRITE_ONLY );
  if ( fd < 0 )
  {
    return (osal_status_t) fd;
  }

  int32_t write_rc = osal_write( fd, data, len );
  (void) osal_close( fd );

  if ( write_rc < 0 )
  {
    return (osal_status_t) write_rc;
  }

  if ( (size_t) write_rc != len )
  {
    osal_log_warning( "wifi config write truncated: %d/%zu", (int) write_rc, len );
    return OSAL_ERROR;
  }

  return OSAL_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/*  Internal helpers                                                          */
/* -------------------------------------------------------------------------- */

static uint8_t _find_max_nb( const wifi_config_list_t* list )
{
  uint8_t max_nb = 0;
  for ( uint8_t i = 0; i < list->count; ++i )
  {
    if ( list->entries[i].nb > max_nb )
    {
      max_nb = list->entries[i].nb;
    }
  }
  return max_nb;
}

static uint8_t _find_min_nb_index( const wifi_config_list_t* list )
{
  uint8_t idx = 0;
  for ( uint8_t i = 1; i < list->count; ++i )
  {
    if ( list->entries[i].nb < list->entries[idx].nb )
    {
      idx = i;
    }
  }
  return idx;
}

static void _renumber( wifi_config_list_t* list )
{
  /* Sort entries by nb ascending using simple insertion sort, then assign
     sequential numbers 0..count-1. */
  for ( uint8_t i = 1; i < list->count; ++i )
  {
    wifi_config_entry_t tmp = list->entries[i];
    uint8_t j = i;
    while ( j > 0 && list->entries[j - 1].nb > tmp.nb )
    {
      list->entries[j] = list->entries[j - 1];
      --j;
    }
    list->entries[j] = tmp;
  }

  /* Update last_use to reflect the new nb of the entry that was last_use. */
  uint8_t old_last_use = list->last_use;
  bool found = false;
  for ( uint8_t i = 0; i < list->count; ++i )
  {
    if ( !found && list->entries[i].nb == old_last_use )
    {
      list->last_use = i;
      found = true;
    }
    list->entries[i].nb = i;
  }

  if ( !found && list->count > 0 )
  {
    list->last_use = 0;
  }
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

osal_status_t wifi_config_load( wifi_config_list_t* list )
{
  if ( !list )
  {
    return OSAL_INVALID_POINTER;
  }

  memset( list, 0, sizeof( *list ) );

  char* content = NULL;
  osal_status_t status = _read_file( WIFI_CONFIG_FILE_PATH, &content );
  if ( status != OSAL_SUCCESS )
  {
    return status;
  }

  cJSON* root = cJSON_Parse( content );
  free( content );
  if ( !root )
  {
    return OSAL_ERROR;
  }

  cJSON* last_use = cJSON_GetObjectItemCaseSensitive( root, "last_use" );
  if ( cJSON_IsNumber( last_use ) )
  {
    list->last_use = (uint8_t) last_use->valueint;
  }

  cJSON* creds = cJSON_GetObjectItemCaseSensitive( root, "credentials" );
  if ( cJSON_IsArray( creds ) )
  {
    cJSON* item = NULL;
    cJSON_ArrayForEach( item, creds )
    {
      if ( list->count >= WIFI_CONFIG_MAX_CREDENTIALS )
      {
        break;
      }

      wifi_config_entry_t* e = &list->entries[list->count];

      cJSON* nb   = cJSON_GetObjectItemCaseSensitive( item, "nb" );
      cJSON* ssid = cJSON_GetObjectItemCaseSensitive( item, "ssid" );
      cJSON* pass = cJSON_GetObjectItemCaseSensitive( item, "password" );

      if ( cJSON_IsNumber( nb ) )
      {
        e->nb = (uint8_t) nb->valueint;
      }

      if ( cJSON_IsString( ssid ) && ssid->valuestring )
      {
        strncpy( e->ssid, ssid->valuestring, WIFI_CONFIG_SSID_MAX_LEN );
      }

      if ( cJSON_IsString( pass ) && pass->valuestring )
      {
        strncpy( e->password, pass->valuestring, WIFI_CONFIG_PASSWORD_MAX_LEN );
      }

      list->count++;
    }
  }

  cJSON_Delete( root );
  return OSAL_SUCCESS;
}

osal_status_t wifi_config_save( const wifi_config_list_t* list )
{
  if ( !list )
  {
    return OSAL_INVALID_POINTER;
  }

  cJSON* root = cJSON_CreateObject();
  if ( !root )
  {
    return OSAL_ERROR;
  }

  if ( !cJSON_AddNumberToObject( root, "last_use", list->last_use ) )
  {
    cJSON_Delete( root );
    return OSAL_ERROR;
  }

  cJSON* arr = cJSON_AddArrayToObject( root, "credentials" );
  if ( !arr )
  {
    cJSON_Delete( root );
    return OSAL_ERROR;
  }

  for ( uint8_t i = 0; i < list->count; ++i )
  {
    cJSON* item = cJSON_CreateObject();
    if ( !item )
    {
      cJSON_Delete( root );
      return OSAL_ERROR;
    }

    if ( !cJSON_AddNumberToObject( item, "nb", list->entries[i].nb ) ||
         !cJSON_AddStringToObject( item, "ssid", list->entries[i].ssid ) ||
         !cJSON_AddStringToObject( item, "password", list->entries[i].password ) )
    {
      cJSON_Delete( item );
      cJSON_Delete( root );
      return OSAL_ERROR;
    }

    cJSON_AddItemToArray( arr, item );
  }

  char* json = cJSON_PrintUnformatted( root );
  cJSON_Delete( root );
  if ( !json )
  {
    return OSAL_ERROR;
  }

  osal_status_t st = _write_file( WIFI_CONFIG_FILE_PATH, json, strlen( json ) );
  free( json );
  return st;
}

osal_status_t wifi_config_add_credential( wifi_config_list_t* list,
                                          const char*         ssid,
                                          const char*         password )
{
  if ( !list || !ssid || !password )
  {
    return OSAL_INVALID_POINTER;
  }

  /* Check if SSID already exists — update in place. */
  for ( uint8_t i = 0; i < list->count; ++i )
  {
    if ( strncmp( list->entries[i].ssid, ssid, WIFI_CONFIG_SSID_MAX_LEN ) == 0 )
    {
      memset( list->entries[i].password, 0, sizeof( list->entries[i].password ) );
      strncpy( list->entries[i].password, password, WIFI_CONFIG_PASSWORD_MAX_LEN );

      uint8_t max_nb = _find_max_nb( list );
      if ( max_nb >= WIFI_CONFIG_NB_MAX )
      {
        _renumber( list );
        max_nb = _find_max_nb( list );
      }
      list->entries[i].nb = max_nb + 1;
      list->last_use = list->entries[i].nb;
      return OSAL_SUCCESS;
    }
  }

  /* Need to check if renumbering is required before assigning new nb. */
  uint8_t max_nb = ( list->count > 0 ) ? _find_max_nb( list ) : 0;
  if ( max_nb >= WIFI_CONFIG_NB_MAX && list->count > 0 )
  {
    _renumber( list );
    max_nb = _find_max_nb( list );
  }

  uint8_t new_nb = ( list->count > 0 ) ? (uint8_t)( max_nb + 1 ) : 0;

  /* If list is full, evict the oldest entry (lowest nb). */
  if ( list->count >= WIFI_CONFIG_MAX_CREDENTIALS )
  {
    uint8_t evict_idx = _find_min_nb_index( list );
    wifi_config_entry_t* e = &list->entries[evict_idx];
    memset( e, 0, sizeof( *e ) );
    e->nb = new_nb;
    strncpy( e->ssid, ssid, WIFI_CONFIG_SSID_MAX_LEN );
    strncpy( e->password, password, WIFI_CONFIG_PASSWORD_MAX_LEN );
    list->last_use = new_nb;
    return OSAL_SUCCESS;
  }

  /* Append new entry. */
  wifi_config_entry_t* e = &list->entries[list->count];
  memset( e, 0, sizeof( *e ) );
  e->nb = new_nb;
  strncpy( e->ssid, ssid, WIFI_CONFIG_SSID_MAX_LEN );
  strncpy( e->password, password, WIFI_CONFIG_PASSWORD_MAX_LEN );
  list->last_use = new_nb;
  list->count++;
  return OSAL_SUCCESS;
}

bool wifi_config_get_by_nb( const wifi_config_list_t* list,
                            uint8_t                   nb,
                            wifi_config_entry_t*      entry )
{
  if ( !list || !entry )
  {
    return false;
  }

  for ( uint8_t i = 0; i < list->count; ++i )
  {
    if ( list->entries[i].nb == nb )
    {
      *entry = list->entries[i];
      return true;
    }
  }
  return false;
}

bool wifi_config_get_next( const wifi_config_list_t* list,
                           uint8_t                   current_nb,
                           wifi_config_entry_t*      entry )
{
  if ( !list || !entry || list->count == 0 )
  {
    return false;
  }

  /* Find the entry with the smallest nb that is strictly greater than
     current_nb.  If none exists, wrap around to the entry with the
     smallest nb overall. */
  const wifi_config_entry_t* best_next = NULL;
  const wifi_config_entry_t* best_min  = &list->entries[0];

  for ( uint8_t i = 0; i < list->count; ++i )
  {
    const wifi_config_entry_t* e = &list->entries[i];

    if ( e->nb > current_nb )
    {
      if ( !best_next || e->nb < best_next->nb )
      {
        best_next = e;
      }
    }

    if ( e->nb < best_min->nb )
    {
      best_min = e;
    }
  }

  *entry = best_next ? *best_next : *best_min;
  return true;
}
