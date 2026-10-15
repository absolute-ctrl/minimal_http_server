#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <sys/types.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

bool net_init(void);
void net_cleanup(void);

sock_t sock_listen(const char *host, const char *port, int backlog);

int sock_wait_readable(sock_t s, int timeout_ms);

sock_t sock_accept(sock_t listener, char *ip, size_t ip_len);

ptrdiff_t sock_recv(sock_t s, void *buf, size_t len);

bool sock_send_all(sock_t s, const void *buf, size_t len);

bool sock_set_timeouts(sock_t s, int seconds);

void sock_close(sock_t s);

void sock_shutdown_send(sock_t s);

const char *sock_last_error(void);

typedef struct {
  bool exists;
  bool is_dir;
  bool is_regular;
  uint64_t size;
  int64_t mtime;
} file_info;

file_info fs_stat(const char *path);

FILE *fs_open_read(const char *path);

bool fs_seek(FILE *f, uint64_t offset);

bool fs_realpath(const char *path, char *out, size_t out_len);

bool time_utc(time_t t, struct tm *out);

double time_now_ms(void);

#endif
