#ifndef HTTP_H
#define HTTP_H

#include "platform.h"

enum {
  HTTP_MAX_HEADERS = 64,
  HTTP_MAX_URI = 8192,
};

typedef struct {
  char *name;
  char *value;
} http_header;

typedef struct {
  char *method;
  char *target;
  char *version;
  int minor;

  char uri[HTTP_MAX_URI + 1];
  char *path;
  char *query;

  http_header headers[HTTP_MAX_HEADERS];
  size_t header_count;

  char *body;
  size_t body_len;

  bool keep_alive;
  bool expect_continue;
  bool has_content_length;
  uint64_t content_length;
  bool has_transfer_encoding;
} http_request;

int http_parse_head(char *buf, size_t len, http_request *req);

const char *http_header_get(const http_request *req, const char *name);

bool http_has_token(const char *value, const char *token);

const char *http_status_text(int status);
const char *http_mime_type(const char *path);

bool http_url_decode(const char *in, char *out, size_t out_len);

bool ascii_ieq(const char *a, const char *b);

#endif
