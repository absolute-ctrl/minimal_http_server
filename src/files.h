#ifndef FILES_H
#define FILES_H

#include "platform.h"

#include "client.h"
#include "http.h"

void files_serve(client_conn *c, const http_request *req);

#endif
