/*
 * server.h - listening socket, accept loop and worker thread pool.
 */
#ifndef SERVER_H
#define SERVER_H

#include "platform.h"

#include <signal.h>

#define SERVER_NAME "c-httpd/1.0"

typedef struct {
  const char *host;
  const char *port;
  char root[4096];
  int threads;
  int queue_size;
  int timeout_sec;
  int max_keepalive;
  size_t max_body;
  bool dir_listing;
} server_config;

typedef struct {
  uint64_t requests;
  uint64_t connections;
  uint64_t active_connections;
  uint64_t bytes_sent;
  time_t started;
} server_stats;

extern volatile sig_atomic_t g_server_stop;

int server_run(const server_config *cfg);

void stats_add_request(uint64_t bytes_sent);
server_stats stats_snapshot(void);

#endif
