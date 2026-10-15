#ifndef RESPONSE_H
#define RESPONSE_H

#include "platform.h"

#include "client.h"
#include "http.h"

bool respond_headers(client_conn *c, const http_request *req, int status,
                     const char *content_type, uint64_t content_length,
                     const char *extra_headers);

bool respond_body(client_conn *c, const http_request *req, const void *data,
                  size_t len);

bool respond(client_conn *c, const http_request *req, int status,
             const char *content_type, const void *body, size_t len,
             const char *extra_headers);

bool respond_error(client_conn *c, const http_request *req, int status,
                   const char *extra_headers);

void respond_raw_error(sock_t fd, int status, const char *extra_headers);

#endif
