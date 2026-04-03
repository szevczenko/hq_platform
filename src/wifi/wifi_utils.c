#include "wifi_utils.h"

#include "cJSON.h"
#include <stdio.h>
#include <string.h>

osal_status_t wifi_utils_ip_info_to_json( const wifi_mgmt_ip_info_t* info,
                                          char*  out_json,
                                          size_t out_json_size )
{
  if ( !info || !out_json || out_json_size == 0 )
  {
    return OSAL_INVALID_POINTER;
  }

  cJSON* root = cJSON_CreateObject();
  if ( !root )
  {
    return OSAL_ERROR;
  }

  (void) cJSON_AddStringToObject( root, "ssid",    info->ssid );
  (void) cJSON_AddStringToObject( root, "ip",      info->ip );
  (void) cJSON_AddStringToObject( root, "netmask", info->netmask );
  (void) cJSON_AddStringToObject( root, "gw",      info->gw );
  (void) cJSON_AddNumberToObject( root, "urc",     info->urc );

  char* printed = cJSON_PrintUnformatted( root );
  cJSON_Delete( root );
  if ( !printed )
  {
    return OSAL_ERROR;
  }

  int written = snprintf( out_json, out_json_size, "%s\n", printed );
  cJSON_free( printed );
  if ( written < 0 || (size_t) written >= out_json_size )
  {
    return OSAL_ERR_OUTPUT_TOO_LARGE;
  }

  return OSAL_SUCCESS;
}

osal_status_t wifi_utils_ap_list_to_json( const wifi_mgmt_ap_list_t* list,
                                          char*  out_json,
                                          size_t out_json_size )
{
  if ( !list || !out_json || out_json_size == 0 )
  {
    return OSAL_INVALID_POINTER;
  }

  cJSON* root = cJSON_CreateArray();
  if ( !root )
  {
    return OSAL_ERROR;
  }

  for ( uint16_t i = 0; i < list->count; ++i )
  {
    cJSON* ap = cJSON_CreateObject();
    if ( !ap )
    {
      continue;
    }

    (void) cJSON_AddStringToObject( ap, "ssid", list->items[i].ssid );
    (void) cJSON_AddNumberToObject( ap, "chan", list->items[i].chan );
    (void) cJSON_AddNumberToObject( ap, "rssi", list->items[i].rssi );
    (void) cJSON_AddNumberToObject( ap, "auth", list->items[i].auth );
    cJSON_AddItemToArray( root, ap );
  }

  char* printed = cJSON_PrintUnformatted( root );
  cJSON_Delete( root );
  if ( !printed )
  {
    return OSAL_ERROR;
  }

  int written = snprintf( out_json, out_json_size, "%s\n", printed );
  cJSON_free( printed );
  if ( written < 0 || (size_t) written >= out_json_size )
  {
    return OSAL_ERR_OUTPUT_TOO_LARGE;
  }

  return OSAL_SUCCESS;
}
