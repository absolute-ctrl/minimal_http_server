#include "platform.h"

#include "client.h"

#include "http.h"
#include "log.h"
#include "response.h"
#include "router.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
  READ_OK,
  READ_CLOSED,
  READ_TIMEOUT,
  READ_TOO_BIG,
} read_result;

static size_t find_head_end(const char *buf, size_t len) {
  for (size_t i = 0; i + 1 < len; i++) {
    if (buf[i] != '\n')
      continue;
    if (buf[i + 1] == '\n')
      return i + 2;
    if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n')
      return i + 3;
  }
  return 0;
}

static bool wait_for_data(client_conn *c) {
  int waited = 0;
  int limit = c->cfg->timeout_sec * 1000;
  while (!g_server_stop && waited < limit) {
    int r = sock_wait_readable(c->fd, 250);
    if (r > 0)
      return true;
    if (r < 0)
      return false;
    waited += 250;
  }
  return false;
}

static read_result read_head(client_conn *c, size_t *head_len) {
  for (;;) {
    size_t skip = 0;
    while (skip < c->len && (c->buf[skip] == '\r' || c->buf[skip] == '\n'))
      skip++;
    if (skip) {
      memmove(c->buf, c->buf + skip, c->len - skip);
      c->len -= skip;
    }

    size_t end = find_head_end(c->buf, c->len);
    if (end) {
      *head_len = end;
      return READ_OK;
    }
    if (c->len >= CLIENT_BUF_SIZE)
      return READ_TOO_BIG;

    if (c->len == 0 && !wait_for_data(c))
      return READ_CLOSED;
    ptrdiff_t n = sock_recv(c->fd, c->buf + c->len, CLIENT_BUF_SIZE - c->len);
    if (n <= 0)
      return c->len == 0 ? READ_CLOSED : READ_TIMEOUT;
    c->len += (size_t)n;
  }
}

static bool read_body(client_conn *c, size_t already, char *dst, size_t len) {
  size_t got = already;
  while (got < len) {
    ptrdiff_t n = sock_recv(c->fd, dst + got, len - got);
    if (n <= 0)
      return false;
    got += (size_t)n;
  }
  return true;
}

static void lingering_close(client_conn *c) {
  sock_shutdown_send(c->fd);
  char sink[4096];
  for (int i = 0; i < 8; i++) {
    if (sock_wait_readable(c->fd, 250) <= 0)
      break;
    if (sock_recv(c->fd, sink, sizeof sink) <= 0)
      break;
  }
  sock_close(c->fd);
}

static void finish_request(client_conn *c, const http_request *req,
                           double started) {
  double elapsed = time_now_ms() - started;
  log_access(c->ip, req && req->method ? req->method : "-",
             req && req->target ? req->target : "-",
             req && req->version ? req->version : "-", c->status, c->bytes_sent,
             elapsed);
  stats_add_request(c->bytes_sent);
}

static bool serve_one(client_conn *c, http_request *req, bool last_allowed) {
  c->status = 0;
  c->bytes_sent = 0;
  c->headers_sent = false;
  c->close_after = last_allowed || g_server_stop;

  size_t head_len = 0;
  read_result rr = read_head(c, &head_len);
  double started = time_now_ms();

  if (rr == READ_CLOSED)
    return false;
  if (rr != READ_OK) {
    int status = 408;
    if (rr == READ_TOO_BIG)
      status = memchr(c->buf, '\n', c->len) ? 431 : 414;
    respond_error(c, NULL, status, NULL);
    finish_request(c, NULL, started);
    return false;
  }

  int rc = http_parse_head(c->buf, head_len, req);
  if (rc != 0) {
    respond_error(c, NULL, rc, NULL);
    finish_request(c, req, started);
    return false;
  }

  size_t buffered = c->len - head_len;
  size_t consumed = head_len;
  if (req->has_content_length && req->content_length > 0) {
    if (req->content_length > (uint64_t)c->cfg->max_body) {
      respond_error(c, req, 413, NULL);
      finish_request(c, req, started);
      return false;
    }
    size_t clen = (size_t)req->content_length;
    req->body = malloc(clen + 1);
    if (!req->body) {
      respond_error(c, req, 500, NULL);
      finish_request(c, req, started);
      return false;
    }
    size_t take = buffered < clen ? buffered : clen;
    memcpy(req->body, c->buf + head_len, take);
    consumed += take;

    if (take < clen && req->expect_continue) {
      static const char CONTINUE[] = "HTTP/1.1 100 Continue\r\n\r\n";
      if (!sock_send_all(c->fd, CONTINUE, sizeof CONTINUE - 1))
        return false;
    }
    if (!read_body(c, take, req->body, clen)) {
      free(req->body);
      req->body = NULL;
      respond_error(c, req, 408, NULL);
      finish_request(c, req, started);
      return false;
    }
    req->body[clen] = '\0';
    req->body_len = clen;
  }

  router_handle(c, req);
  if (!c->headers_sent)
    respond_error(c, req, 500, NULL);
  finish_request(c, req, started);

  free(req->body);
  req->body = NULL;

  memmove(c->buf, c->buf + consumed, c->len - consumed);
  c->len -= consumed;

  return !c->close_after;
}

void client_handle(sock_t fd, const char *ip, const server_config *cfg) {
  client_conn *c = malloc(sizeof *c);
  http_request *req = malloc(sizeof *req);
  if (!c || !req) {
    respond_raw_error(fd, 503, NULL);
    sock_close(fd);
    free(c);
    free(req);
    return;
  }
  c->fd = fd;
  snprintf(c->ip, sizeof c->ip, "%s", ip);
  c->cfg = cfg;
  c->len = 0;
  req->body = NULL;

  sock_set_timeouts(fd, cfg->timeout_sec);

  for (int served = 0; served < cfg->max_keepalive; served++) {
    if (!serve_one(c, req, served + 1 >= cfg->max_keepalive))
      break;
  }

  lingering_close(c);
  free(req);
  free(c);
}
