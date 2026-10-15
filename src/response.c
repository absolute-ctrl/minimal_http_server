#include "platform.h"

#include "response.h"

#include "log.h"
#include "strbuf.h"

#include <string.h>

static bool is_head(const http_request *req) {
  return req && req->method && strcmp(req->method, "HEAD") == 0;
}

static bool status_has_body(int status) {
  return !(status < 200 || status == 204 || status == 304);
}

bool respond_headers(client_conn *c, const http_request *req, int status,
                     const char *content_type, uint64_t content_length,
                     const char *extra_headers) {
  if (!req || !req->keep_alive)
    c->close_after = true;

  char date[64];
  http_date(time(NULL), date, sizeof date);

  char head[2048];
  int n = snprintf(head, sizeof head,
                   "HTTP/1.1 %d %s\r\n"
                   "Date: %s\r\n"
                   "Server: " SERVER_NAME "\r\n"
                   "X-Content-Type-Options: nosniff\r\n"
                   "Connection: %s\r\n",
                   status, http_status_text(status), date,
                   c->close_after ? "close" : "keep-alive");
  if (n < 0 || (size_t)n >= sizeof head)
    return false;
  size_t used = (size_t)n;

  if (status_has_body(status)) {
    if (content_type) {
      n = snprintf(head + used, sizeof head - used, "Content-Type: %s\r\n",
                   content_type);
      if (n < 0 || (size_t)n >= sizeof head - used)
        return false;
      used += (size_t)n;
    }
    n = snprintf(head + used, sizeof head - used, "Content-Length: %llu\r\n",
                 (unsigned long long)content_length);
    if (n < 0 || (size_t)n >= sizeof head - used)
      return false;
    used += (size_t)n;
  }

  strbuf sb;
  sb_init(&sb);
  sb_append(&sb, head, used);
  if (extra_headers)
    sb_puts(&sb, extra_headers);
  sb_puts(&sb, "\r\n");

  bool ok = !sb.failed && sock_send_all(c->fd, sb.data, sb.len);
  sb_free(&sb);

  c->status = status;
  c->headers_sent = true;
  if (!ok)
    c->close_after = true;
  return ok;
}

bool respond_body(client_conn *c, const http_request *req, const void *data,
                  size_t len) {
  if (is_head(req) || len == 0)
    return true;
  if (!sock_send_all(c->fd, data, len)) {
    c->close_after = true;
    return false;
  }
  c->bytes_sent += len;
  return true;
}

bool respond(client_conn *c, const http_request *req, int status,
             const char *content_type, const void *body, size_t len,
             const char *extra_headers) {
  if (!respond_headers(c, req, status, content_type, len, extra_headers))
    return false;
  if (!status_has_body(status))
    return true;
  return respond_body(c, req, body, len);
}

static bool error_closes_connection(int status) {
  switch (status) {
  case 400:
  case 408:
  case 411:
  case 413:
  case 414:
  case 417:
  case 431:
  case 500:
  case 501:
  case 503:
  case 505:
    return true;
  default:
    return false;
  }
}

static void build_error_page(strbuf *sb, int status) {
  const char *text = http_status_text(status);
  sb_printf(
      sb,
      "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>%d "
      "%s</title>"
      "<style>body{font-family:system-ui,sans-serif;max-width:40em;margin:4em "
      "auto;"
      "padding:0 1em;color:#222}h1{font-weight:600}hr{border:0;border-top:1px "
      "solid #ccc}"
      "small{color:#777}</style></head>\n<body><h1>%d "
      "%s</h1><hr><small>" SERVER_NAME "</small></body></html>\n",
      status, text, status, text);
}

bool respond_error(client_conn *c, const http_request *req, int status,
                   const char *extra_headers) {
  if (error_closes_connection(status))
    c->close_after = true;
  strbuf sb;
  sb_init(&sb);
  build_error_page(&sb, status);
  bool ok = !sb.failed && respond(c, req, status, "text/html; charset=utf-8",
                                  sb.data, sb.len, extra_headers);
  sb_free(&sb);
  return ok;
}

void respond_raw_error(sock_t fd, int status, const char *extra_headers) {
  strbuf body;
  sb_init(&body);
  build_error_page(&body, status);

  char date[64];
  http_date(time(NULL), date, sizeof date);
  strbuf sb;
  sb_init(&sb);
  sb_printf(&sb,
            "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: " SERVER_NAME "\r\n"
            "Content-Type: text/html; charset=utf-8\r\nContent-Length: %llu\r\n"
            "Connection: close\r\n%s\r\n",
            status, http_status_text(status), date,
            (unsigned long long)body.len, extra_headers ? extra_headers : "");
  sb_append(&sb, body.data ? body.data : "", body.len);
  if (!sb.failed)
    sock_send_all(fd, sb.data, sb.len);
  sb_free(&sb);
  sb_free(&body);
}
