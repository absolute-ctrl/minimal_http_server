#ifndef CLIENT_H
#define CLIENT_H

#include "platform.h"

#include "server.h"

enum {
  CLIENT_BUF_SIZE = 16384,
};

typedef struct {
  sock_t fd;
  char ip[INET6_ADDRSTRLEN];
  const server_config *cfg;

  char buf[CLIENT_BUF_SIZE + 1];
  size_t len;

  int status;
  uint64_t bytes_sent;
  bool headers_sent;
  bool close_after;
} client_conn;

void client_handle(sock_t fd, const char *ip, const server_config *cfg);

#endif
