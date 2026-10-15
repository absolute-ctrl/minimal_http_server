#include "platform.h"

#include "router.h"

#include "files.h"
#include "response.h"
#include "strbuf.h"

#include <string.h>

typedef void (*route_handler)(client_conn *c, const http_request *req);

typedef struct {
  const char *method;
  const char *path;
  route_handler handler;
} route;

static void send_json(client_conn *c, const http_request *req, int status,
                      strbuf *sb) {
  if (sb->failed) {
    respond_error(c, req, 500, NULL);
    return;
  }
  respond(c, req, status, "application/json", sb->data, sb->len,
          "Cache-Control: no-store\r\n");
}

static void handle_health(client_conn *c, const http_request *req) {
  server_stats s = stats_snapshot();
  strbuf sb;
  sb_init(&sb);
  sb_printf(&sb,
            "{\"status\":\"ok\",\"server\":\"" SERVER_NAME
            "\",\"uptime_seconds\":%lld}\n",
            (long long)(time(NULL) - s.started));
  send_json(c, req, 200, &sb);
  sb_free(&sb);
}

static void handle_stats(client_conn *c, const http_request *req) {
  server_stats s = stats_snapshot();
  strbuf sb;
  sb_init(&sb);
  sb_printf(
      &sb,
      "{\"requests\":%llu,\"connections\":%llu,\"active_connections\":%llu,"
      "\"bytes_sent\":%llu,\"uptime_seconds\":%lld,\"threads\":%d}\n",
      (unsigned long long)s.requests, (unsigned long long)s.connections,
      (unsigned long long)s.active_connections,
      (unsigned long long)s.bytes_sent, (long long)(time(NULL) - s.started),
      c->cfg->threads);
  send_json(c, req, 200, &sb);
  sb_free(&sb);
}

static void handle_echo(client_conn *c, const http_request *req) {
  strbuf sb;
  sb_init(&sb);
  sb_puts(&sb, "{\"method\":\"");
  sb_json_escape(&sb, req->method, strlen(req->method));
  sb_puts(&sb, "\",\"path\":\"");
  sb_json_escape(&sb, req->path, strlen(req->path));
  sb_puts(&sb, "\",\"query\":\"");
  sb_json_escape(&sb, req->query, strlen(req->query));
  sb_puts(&sb, "\",\"version\":\"");
  sb_json_escape(&sb, req->version, strlen(req->version));
  sb_puts(&sb, "\",\"remote_addr\":\"");
  sb_json_escape(&sb, c->ip, strlen(c->ip));
  sb_puts(&sb, "\",\"headers\":{");
  for (size_t i = 0; i < req->header_count; i++) {
    if (i)
      sb_puts(&sb, ",");
    sb_puts(&sb, "\"");
    sb_json_escape(&sb, req->headers[i].name, strlen(req->headers[i].name));
    sb_puts(&sb, "\":\"");
    sb_json_escape(&sb, req->headers[i].value, strlen(req->headers[i].value));
    sb_puts(&sb, "\"");
  }
  sb_printf(&sb, "},\"body_length\":%llu,\"body\":\"",
            (unsigned long long)req->body_len);
  if (req->body)
    sb_json_escape(&sb, req->body, req->body_len);
  sb_puts(&sb, "\"}\n");
  send_json(c, req, 200, &sb);
  sb_free(&sb);
}

static const route ROUTES[] = {
    {"GET", "/api/health", handle_health}, {"GET", "/api/stats", handle_stats},
    {"GET", "/api/echo", handle_echo},     {"POST", "/api/echo", handle_echo},
    {"PUT", "/api/echo", handle_echo},     {"PATCH", "/api/echo", handle_echo},
    {"DELETE", "/api/echo", handle_echo},
};
enum { ROUTE_COUNT = sizeof ROUTES / sizeof ROUTES[0] };

static bool method_matches(const char *route_method, const char *method) {
  if (strcmp(route_method, method) == 0)
    return true;
  return strcmp(route_method, "GET") == 0 && strcmp(method, "HEAD") == 0;
}

static void build_allow(strbuf *sb, const char *path) {
  sb_puts(sb, "Allow: ");
  for (size_t i = 0; i < ROUTE_COUNT; i++) {
    if (strcmp(ROUTES[i].path, path) != 0)
      continue;
    sb_printf(sb, "%s, ", ROUTES[i].method);
    if (strcmp(ROUTES[i].method, "GET") == 0)
      sb_puts(sb, "HEAD, ");
  }
  sb_puts(sb, "OPTIONS\r\n");
}

static bool is_known_method(const char *m) {
  static const char *const KNOWN[] = {"GET",     "HEAD",   "POST",
                                      "PUT",     "DELETE", "CONNECT",
                                      "OPTIONS", "TRACE",  "PATCH"};
  for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++)
    if (strcmp(m, KNOWN[i]) == 0)
      return true;
  return false;
}

void router_handle(client_conn *c, const http_request *req) {
  if (!is_known_method(req->method)) {
    respond_error(c, req, 501, NULL);
    return;
  }

  if (strcmp(req->path, "*") == 0) {
    respond(c, req, 204, NULL, NULL, 0,
            "Allow: GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS\r\n");
    return;
  }

  bool path_known = false;
  for (size_t i = 0; i < ROUTE_COUNT; i++) {
    if (strcmp(ROUTES[i].path, req->path) != 0)
      continue;
    path_known = true;
    if (method_matches(ROUTES[i].method, req->method)) {
      ROUTES[i].handler(c, req);
      return;
    }
  }
  if (path_known) {
    strbuf allow;
    sb_init(&allow);
    build_allow(&allow, req->path);
    int status = strcmp(req->method, "OPTIONS") == 0 ? 204 : 405;
    if (status == 204)
      respond(c, req, 204, NULL, NULL, 0, allow.data);
    else
      respond_error(c, req, 405, allow.data);
    sb_free(&allow);
    return;
  }

  if (strcmp(req->method, "OPTIONS") == 0) {
    respond(c, req, 204, NULL, NULL, 0, "Allow: GET, HEAD, OPTIONS\r\n");
    return;
  }
  if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
    respond_error(c, req, 405, "Allow: GET, HEAD, OPTIONS\r\n");
    return;
  }
  files_serve(c, req);
}
