#ifndef MONGOOSE_PROCESS_H
#define MONGOOSE_PROCESS_H

#include "mongoose.h"

extern struct mg_mgr mgr;

void MongooseProcess_Init(void);
void MongooseProcess_Deinit(void);

#endif    /* MONGOOSE_PROCESS_H */
