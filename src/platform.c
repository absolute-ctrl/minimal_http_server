#include "platform.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#include <sys/timeb.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

bool net_init(void) {
#ifdef _WIN32
  WSADATA wsa;
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
  return true;
#endif
}

void net_cleanup(void) {
#ifdef _WIN32
  WSACleanup();
#endif
}

void sock_close(sock_t s) {
  if (s == SOCK_INVALID)
    return;
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
}

void sock_shutdown_send(sock_t s) {
#ifdef _WIN32
  shutdown(s, SD_SEND);
#else
  shutdown(s, SHUT_WR);
#endif
}

const char *sock_last_error(void) {
#ifdef _WIN32
  static _Thread_local char buf[256];
  int err = WSAGetLastError();
  DWORD n =
      FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     NULL, (DWORD)err, 0, buf, (DWORD)sizeof buf, NULL);
  if (n == 0)
    snprintf(buf, sizeof buf, "WSA error %d", err);
  for (size_t len = strlen(buf);
       len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r');)
    buf[--len] = '\0';
  return buf;
#else
  return strerror(errno);
#endif
}

static sock_t try_listen(const struct addrinfo *ai, int backlog) {
  sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (s == SOCK_INVALID)
    return SOCK_INVALID;

  int yes = 1;
#ifdef _WIN32
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes,
             sizeof yes);
#else
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif
  if (ai->ai_family == AF_INET6) {
    int no = 0;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&no, sizeof no);
  }

  if (bind(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) != 0 ||
      listen(s, backlog) != 0) {
    sock_close(s);
    return SOCK_INVALID;
  }
  return s;
}

sock_t sock_listen(const char *host, const char *port, int backlog) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  sock_t s = SOCK_INVALID;
  struct addrinfo *res = NULL;

  if (host == NULL) {
    hints.ai_family = AF_INET6;
    if (getaddrinfo(NULL, port, &hints, &res) == 0) {
      for (struct addrinfo *ai = res; ai && s == SOCK_INVALID; ai = ai->ai_next)
        s = try_listen(ai, backlog);
      freeaddrinfo(res);
      res = NULL;
    }
    if (s != SOCK_INVALID)
      return s;
    hints.ai_family = AF_INET;
  } else {
    hints.ai_family = AF_UNSPEC;
  }

  if (getaddrinfo(host, port, &hints, &res) != 0)
    return SOCK_INVALID;
  for (struct addrinfo *ai = res; ai && s == SOCK_INVALID; ai = ai->ai_next)
    s = try_listen(ai, backlog);
  freeaddrinfo(res);
  return s;
}

int sock_wait_readable(sock_t s, int timeout_ms) {
  fd_set set;
#ifdef _WIN32
  set.fd_count = 1;
  set.fd_array[0] = s;
#else
  FD_ZERO(&set);
  FD_SET(s, &set);
#endif
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
  int r = select(0, &set, NULL, NULL, &tv);
  if (r == SOCKET_ERROR)
    return WSAGetLastError() == WSAEINTR ? 0 : -1;
#else
  int r = select(s + 1, &set, NULL, NULL, &tv);
  if (r < 0)
    return errno == EINTR ? 0 : -1;
#endif
  return r > 0 ? 1 : 0;
}

sock_t sock_accept(sock_t listener, char *ip, size_t ip_len) {
  struct sockaddr_storage addr;
  socklen_t addr_len = sizeof addr;
  sock_t c = accept(listener, (struct sockaddr *)&addr, &addr_len);
  if (c == SOCK_INVALID)
    return SOCK_INVALID;

#if defined(SO_NOSIGPIPE)
  int yes = 1; /* macOS: no MSG_NOSIGNAL, so disable SIGPIPE per socket */
  setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif

  if (ip && ip_len > 0) {
    ip[0] = '\0';
    const void *src = NULL;
    if (addr.ss_family == AF_INET)
      src = &((struct sockaddr_in *)&addr)->sin_addr;
    else if (addr.ss_family == AF_INET6)
      src = &((struct sockaddr_in6 *)&addr)->sin6_addr;
#ifdef _WIN32
    if (src)
      inet_ntop(addr.ss_family, src, ip, ip_len); /* takes size_t on Windows */
#else
    if (src)
      inet_ntop(addr.ss_family, src, ip, (socklen_t)ip_len);
#endif
    if (strncmp(ip, "::ffff:", 7) == 0 && strchr(ip + 7, '.'))
      memmove(ip, ip + 7, strlen(ip + 7) + 1);
  }
  return c;
}

ptrdiff_t sock_recv(sock_t s, void *buf, size_t len) {
#ifdef _WIN32
  int n = recv(s, (char *)buf, len > INT_MAX ? INT_MAX : (int)len, 0);
  return n == SOCKET_ERROR ? -1 : (ptrdiff_t)n;
#else
  for (;;) {
    ssize_t n = recv(s, buf, len, 0);
    if (n < 0 && errno == EINTR)
      continue;
    return (ptrdiff_t)n;
  }
#endif
}

bool sock_send_all(sock_t s, const void *buf, size_t len) {
  const char *p = buf;
  while (len > 0) {
#ifdef _WIN32
    int chunk = len > INT_MAX ? INT_MAX : (int)len;
    int n = send(s, p, chunk, 0);
    if (n == SOCKET_ERROR)
      return false;
#else
    ssize_t n = send(s, p, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
#endif
    if (n == 0)
      return false;
    p += n;
    len -= (size_t)n;
  }
  return true;
}

bool sock_set_timeouts(sock_t s, int seconds) {
#ifdef _WIN32
  DWORD ms = (DWORD)seconds * 1000u;
  return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms) ==
             0 &&
         setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms) ==
             0;
#else
  struct timeval tv = {.tv_sec = seconds, .tv_usec = 0};
  return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 &&
         setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0;
#endif
}

/* ======================================================================= */
/* filesystem                                                              */
/* ======================================================================= */

file_info fs_stat(const char *path) {
  file_info fi = {0};
#ifdef _WIN32
  struct _stat64 st;
  if (_stat64(path, &st) != 0)
    return fi;
  fi.is_dir = (st.st_mode & _S_IFMT) == _S_IFDIR;
  fi.is_regular = (st.st_mode & _S_IFMT) == _S_IFREG;
#else
  struct stat st;
  if (stat(path, &st) != 0)
    return fi;
  fi.is_dir = S_ISDIR(st.st_mode);
  fi.is_regular = S_ISREG(st.st_mode);
#endif
  fi.exists = true;
  fi.size = st.st_size > 0 ? (uint64_t)st.st_size : 0;
  fi.mtime = (int64_t)st.st_mtime;
  return fi;
}

FILE *fs_open_read(const char *path) {
#ifdef _WIN32
  return fopen(path, "rb");
#else
  /* O_CLOEXEC-ish and refuse FIFOs/devices that could block forever. */
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  struct stat st;
  if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
    fclose(f);
    return NULL;
  }
  return f;
#endif
}

bool fs_seek(FILE *f, uint64_t offset) {
  if (offset > (uint64_t)INT64_MAX)
    return false;
#ifdef _WIN32
  return _fseeki64(f, (long long)offset, SEEK_SET) == 0;
#else
  return fseeko(f, (off_t)offset, SEEK_SET) == 0;
#endif
}

bool fs_realpath(const char *path, char *out, size_t out_len) {
#ifdef _WIN32
  if (!_fullpath(out, path, out_len))
    return false;
  return fs_stat(out).exists;
#else
  char *r = realpath(path, NULL);
  if (!r)
    return false;
  size_t n = strlen(r);
  bool ok = n < out_len;
  if (ok)
    memcpy(out, r, n + 1);
  free(r);
  return ok;
#endif
}

/* ======================================================================= */
/* time                                                                    */
/* ======================================================================= */

bool time_utc(time_t t, struct tm *out) {
#ifdef _WIN32
  return gmtime_s(out, &t) == 0;
#else
  return gmtime_r(&t, out) != NULL;
#endif
}

double time_now_ms(void) {
#ifdef _WIN32
  static LARGE_INTEGER freq;
  LARGE_INTEGER now;
  if (freq.QuadPart == 0)
    QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
  struct timespec ts;
  if (timespec_get(&ts, TIME_UTC) != TIME_UTC)
    return 0.0;
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}
