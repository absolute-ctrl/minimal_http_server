# c-httpd

A small, dependency-free HTTP/1.1 server written in pure C23. It builds and runs on
**Windows** (MSYS2 CLANG64, Winsock) and **Linux / macOS / BSD** (POSIX sockets) from the same
source, using only the C standard library, the OS socket API and pthreads.

## Build and run

From the MSYS2 CLANG64 shell (or any Linux/macOS terminal):

```sh
make            # debug build   -> build/app.exe (Windows) or build/app
make run        # build and serve ./public on http://localhost:8080
make release    # optimized build (statically linked on Windows)
make sanitize   # ASan + UBSan build (Linux/macOS)
make clean
```

Pass arguments through `make run`: `make run ARGS="-p 9000 -r C:/www"`.
GCC 13 calls C23 `c2x`, so use `make CC=gcc CSTD=-std=c2x` there.

## Options

```
-p, --port PORT        port to listen on (default 8080)
-b, --bind ADDR        address to bind (default: all interfaces, IPv6 + IPv4)
-r, --root DIR         document root (default ./public)
-t, --threads N        worker threads (default 8)
-q, --queue N          max pending connections before answering 503 (default 256)
-T, --timeout SEC      idle/read/write timeout per connection (default 10)
-k, --keepalive N      max requests per connection (default 100)
-m, --max-body SIZE    max request body, e.g. 512K, 10M (default 10M)
-L, --no-listing       disable directory listings
-Q, --quiet            disable the access log
-v, --verbose          debug logging
```

Press Ctrl+C to stop. The server lets requests that are already running finish, then exits.

## Features

**Protocol**
- HTTP/1.0 and HTTP/1.1, persistent connections (keep-alive) and pipelining
- `Expect: 100-continue`, absolute-form targets, `OPTIONS *`
- Strict parsing: rejects obsolete line folding, whitespace before a colon, embedded NULs,
  conflicting `Content-Length` headers, and `Transfer-Encoding` combined with `Content-Length`
  (a request-smuggling vector)
- Proper status codes: 400, 403, 404, 405 (with an `Allow` header), 408, 413, 414, 416, 417,
  431, 501, 503, 505
- After an error, the server closes the connection gracefully so the client receives the error
  page instead of a TCP reset

**Static files**
- MIME types, `index.html`, and automatic directory listings (HTML-escaped and URL-encoded)
- `ETag` / `If-None-Match` and `Last-Modified` / `If-Modified-Since` → `304 Not Modified`
- Single byte ranges (`Range`, `If-Range`) → `206` / `416`, so video seeking and resumed
  downloads work
- Streams files in 64 KB chunks, with 64-bit file sizes on every platform
- Security: percent-decoding with `%00` rejected; `..` cannot climb above the root; dotfiles
  (`.git`, `.env`, …) are hidden; symlinks that point outside the root are blocked; on Windows,
  `\` and `:` are rejected (drive letters, alternate data streams)

**Built-in API** (see `ROUTES[]` in `src/router.c`)
- `GET /api/health` – liveness check
- `GET /api/stats` – request, connection and byte counters
- `GET|POST|PUT|PATCH|DELETE /api/echo` – returns the request (headers, query, body) as JSON

**Runtime**
- Fixed thread pool with a bounded queue; when the queue is full, new connections get
  `503 Retry-After` instead of piling up
- Timeouts on every connection to defend against slowloris-style attacks
- Thread-safe access log in Common Log Format, plus the time each request took

## Adding your own endpoint

```c
static void handle_hello(client_conn *c, const http_request *req) {
    const char *msg = "Hello!\n";
    respond(c, req, 200, "text/plain; charset=utf-8", msg, strlen(msg), NULL);
}

static const route ROUTES[] = {
    {"GET", "/hello", handle_hello},   /* GET routes also answer HEAD */
    ...
};
```

`req` gives you `method`, `path`, `query`, `headers` (`http_header_get(req, "Name")`) and
`body` / `body_len`.

## Source layout

| File | Purpose |
|---|---|
| `main.c` | command-line options, signal handling, startup |
| `server.c` | listening socket, accept loop, thread pool, stats |
| `client.c` | per-connection loop: read head/body, keep-alive, pipelining, timeouts |
| `http.c` | request-line/header parser, status texts, MIME table, URL decoding |
| `router.c` | API routes and dispatch to static files |
| `files.c` | static files, directory listings, ETag/304, ranges, path safety |
| `response.c` | building and sending responses and error pages |
| `platform.c` | Winsock/POSIX sockets, file stat/seek, time helpers |
| `strbuf.c`, `log.c` | growable strings, HTML/URL/JSON escaping, logging |

## Limitations

- No TLS. To serve HTTPS, put the server behind a reverse proxy such as nginx or Caddy.
- Request bodies sent with `Transfer-Encoding: chunked` are refused with `501`. Responses
  always use `Content-Length`.
- Multi-range requests (`Range: bytes=0-1,5-6`) get the whole file, which RFC 9110 permits.
- On Windows, file paths use the ANSI code page, so non-ASCII file names may not resolve.
- The server uses one thread per active connection, which suits small and medium workloads. It
  is not an epoll/IOCP event loop.
