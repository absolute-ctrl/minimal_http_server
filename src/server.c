#include "platform.h"

#include "server.h"

#include "client.h"
#include "log.h"
#include "response.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

volatile sig_atomic_t g_server_stop = 0;

static atomic_uint_fast64_t g_requests;
static atomic_uint_fast64_t g_connections;
static atomic_uint_fast64_t g_active;
static atomic_uint_fast64_t g_bytes;
static time_t g_started;

void stats_add_request(uint64_t bytes_sent) {
  atomic_fetch_add(&g_requests, 1);
  atomic_fetch_add(&g_bytes, bytes_sent);
}

server_stats stats_snapshot(void) {
  return (server_stats){
      .requests = atomic_load(&g_requests),
      .connections = atomic_load(&g_connections),
      .active_connections = atomic_load(&g_active),
      .bytes_sent = atomic_load(&g_bytes),
      .started = g_started,
  };
}

typedef struct {
  sock_t fd;
  char ip[INET6_ADDRSTRLEN];
} pending_conn;

typedef struct {
  pending_conn *items;
  size_t cap, head, count;
  bool closing;
  pthread_mutex_t lock;
  pthread_cond_t not_empty;
  const server_config *cfg;
} work_queue;

static bool queue_push(work_queue *q, sock_t fd, const char *ip) {
  pthread_mutex_lock(&q->lock);
  bool ok = q->count < q->cap;
  if (ok) {
    pending_conn *slot = &q->items[(q->head + q->count) % q->cap];
    slot->fd = fd;
    snprintf(slot->ip, sizeof slot->ip, "%s", ip);
    q->count++;
    pthread_cond_signal(&q->not_empty);
  }
  pthread_mutex_unlock(&q->lock);
  return ok;
}

static bool queue_pop(work_queue *q, pending_conn *out) {
  pthread_mutex_lock(&q->lock);
  while (q->count == 0 && !q->closing)
    pthread_cond_wait(&q->not_empty, &q->lock);
  bool ok = q->count > 0;
  if (ok) {
    *out = q->items[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
  }
  pthread_mutex_unlock(&q->lock);
  return ok;
}

static void *worker_main(void *arg) {
  work_queue *q = arg;
  pending_conn pc;
  while (queue_pop(q, &pc)) {
    atomic_fetch_add(&g_active, 1);
    client_handle(pc.fd, pc.ip, q->cfg);
    atomic_fetch_sub(&g_active, 1);
  }
  return NULL;
}

int server_run(const server_config *cfg) {
  g_started = time(NULL);

  sock_t listener = sock_listen(cfg->host, cfg->port, 128);
  if (listener == SOCK_INVALID) {
    log_msg(LOG_ERROR, "cannot listen on %s:%s: %s",
            cfg->host ? cfg->host : "*", cfg->port, sock_last_error());
    return 1;
  }

  work_queue q = {
      .cap = (size_t)cfg->queue_size,
      .cfg = cfg,
  };
  q.items = calloc(q.cap, sizeof *q.items);
  pthread_t *workers = calloc((size_t)cfg->threads, sizeof *workers);
  if (!q.items || !workers) {
    log_msg(LOG_ERROR, "out of memory");
    free(q.items);
    free(workers);
    sock_close(listener);
    return 1;
  }
  pthread_mutex_init(&q.lock, NULL);
  pthread_cond_init(&q.not_empty, NULL);

  int started = 0;
  for (; started < cfg->threads; started++) {
    if (pthread_create(&workers[started], NULL, worker_main, &q) != 0) {
      log_msg(LOG_WARN, "could only start %d worker threads", started);
      break;
    }
  }

  int rc = 0;
  if (started == 0) {
    log_msg(LOG_ERROR, "no worker threads could be started");
    rc = 1;
    g_server_stop = 1;
  } else {
    log_msg(LOG_INFO, "listening on http://%s:%s/ (root: %s, %d threads)",
            cfg->host ? cfg->host : "localhost", cfg->port, cfg->root, started);
  }

  while (!g_server_stop) {
    int ready = sock_wait_readable(listener, 250);
    if (ready < 0) {
      log_msg(LOG_ERROR, "select failed: %s", sock_last_error());
      rc = 1;
      break;
    }
    if (ready == 0)
      continue;

    char ip[INET6_ADDRSTRLEN];
    sock_t fd = sock_accept(listener, ip, sizeof ip);
    if (fd == SOCK_INVALID) {
      log_msg(LOG_DEBUG, "accept failed: %s", sock_last_error());
      continue;
    }
    atomic_fetch_add(&g_connections, 1);
    if (!queue_push(&q, fd, ip)) {
      sock_set_timeouts(fd, 1);
      respond_raw_error(fd, 503, "Retry-After: 1\r\n");
      sock_close(fd);
      log_msg(LOG_WARN, "queue full, rejected %s with 503", ip);
    }
  }

  log_msg(LOG_INFO, "shutting down, waiting for workers...");
  sock_close(listener);

  pthread_mutex_lock(&q.lock);
  q.closing = true;
  pthread_cond_broadcast(&q.not_empty);
  pthread_mutex_unlock(&q.lock);
  for (int i = 0; i < started; i++)
    pthread_join(workers[i], NULL);

  for (size_t i = 0; i < q.count; i++)
    sock_close(q.items[(q.head + i) % q.cap].fd);

  pthread_cond_destroy(&q.not_empty);
  pthread_mutex_destroy(&q.lock);
  free(workers);
  free(q.items);

  server_stats s = stats_snapshot();
  log_msg(LOG_INFO, "stopped after %llu requests on %llu connections",
          (unsigned long long)s.requests, (unsigned long long)s.connections);
  return rc;
}
