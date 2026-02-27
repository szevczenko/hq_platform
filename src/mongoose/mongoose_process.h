#ifndef MONGOOSE_PROCESS_H
#define MONGOOSE_PROCESS_H

#include "mongoose.h"

extern struct mg_mgr mgr;

/**
 * @brief Initializes the mongoose process and manager.
 */
void MongooseProcess_Init( void );

/**
 * @brief Deinitializes the mongoose process and manager.
 */
void MongooseProcess_Deinit( void );

#endif    // MONGOOSE_PROCESS_H
