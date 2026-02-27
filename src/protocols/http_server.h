/**
 *******************************************************************************
 * @file    http_server.h
 * @author  Dmytro Shevchenko
 * @brief   Mongoose component implementation header file
 *******************************************************************************
 */

/* Define to prevent recursive inclusion ------------------------------------*/

#ifndef _HTTP_SERVER_H_
#define _HTTP_SERVER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "mongoose.h"

/* Public types --------------------------------------------------------------*/

typedef enum
{
  HTTP_SERVER_METHOD_PUT,
  HTTP_SERVER_METHOD_GET,
  HTTP_SERVER_METHOD_POST,
  HTTP_SERVER_METHOD_DELETE,
  HTTP_SERVER_METHOD_PATCH,
  HTTP_SERVER_METHOD_UNHALLOWED,
  HTTP_SERVER_METHOD_LAST
} HTTPServerMethod_t;

typedef struct
{
  uint32_t code;
  const char* headers;
  const char* msg;
} HTTPServerResponse_t;

typedef HTTPServerResponse_t ( *HTTPServerCb_t )( struct mg_str* uri, struct mg_str* data, HTTPServerMethod_t method );

typedef struct
{
  const char* api_name; /* API name after /api/ */
  HTTPServerCb_t cb;
} HTTPServerApiToken_t;

/* Public functions ----------------------------------------------------------*/

/**
 * @brief   Init HTTP server driver.
 */
void HTTPServer_Init( void );

/**
 * @brief   Deinit HTTP server driver.
 */
void HTTPServer_Deinit( void );

/**
 * @brief   Add API token.
 */
void HTTPServer_AddApiToken( HTTPServerApiToken_t* token );

/**
 * @brief   Checks if any client send data last 5 seconds.
 */
bool HTTPServer_IsClientConnected( void );

#endif