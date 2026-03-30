#include "mongoose_process.h"

#include <stdbool.h>

#include "hq_config.h"
#include "osal_task.h"

#ifndef CONFIG_MONGOOSE_LOG_LEVEL
#define CONFIG_MONGOOSE_LOG_LEVEL 2
#endif

struct mg_mgr mgr;
static osal_task_id_t mongooseProcessId;
static bool mongooseProcessRunning = false;

static void _process( void* arg )
{
  (void)arg;
  while ( 1 )
  {
    mg_mgr_poll( &mgr, 1000 );
  }
}

void MongooseProcess_Init( void )
{
  if ( mongooseProcessRunning )
  {
    return;
  }

  mg_mgr_init( &mgr );
  mg_log_set( CONFIG_MONGOOSE_LOG_LEVEL );

  if ( osal_task_create( &mongooseProcessId,
                         "mg_poll",
                         _process,
                         NULL,
                         NULL,
                         16384,
                         5,
                         NULL ) != OSAL_SUCCESS )
  {
    mg_mgr_free( &mgr );
    return;
  }

  mongooseProcessRunning = true;
}

void MongooseProcess_Deinit( void )
{
  if ( mongooseProcessRunning )
  {
    (void)osal_task_delete( mongooseProcessId );
    mongooseProcessRunning = false;
  }

  mg_mgr_free( &mgr );
}
