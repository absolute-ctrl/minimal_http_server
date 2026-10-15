#ifndef ROUTER_H
#define ROUTER_H

#include "platform.h"

#include "client.h"
#include "http.h"

void router_handle(client_conn *c, const http_request *req);

#endif