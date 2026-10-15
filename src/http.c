#include "platform.h"

#include "http.h"

#include <string.h>

static char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool ascii_ieq(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (ascii_lower(*a) != ascii_lower(*b))
      return false;
  return *a == *b;
}

static bool ascii_ieq_n(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (ascii_lower(a[i]) != ascii_lower(b[i]))
      return false;
    if (a[i] == '\0')
      return true;
  }
  return true;
}

static bool is_tchar(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9'))
    return true;
  return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static bool is_token(const char *s) {
  if (*s == '\0')
    return false;
  for (; *s; s++)
    if (!is_tchar((unsigned char)*s))
      return false;
  return true;
}

static bool is_ows(char c) { return c == ' ' || c == '\t'; }

static bool is_valid_field_value(const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    if ((*p < 0x20 && *p != '\t') || *p == 0x7f)
      return false;
  return true;
}

static bool is_valid_target(const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    if (*p <= 0x20 || *p >= 0x7f)
      return false;
  return true;
}

static char *take_line(char **cursor, char *end, size_t *line_len) {
  char *start = *cursor;
  char *lf = memchr(start, '\n', (size_t)(end - start));
  if (!lf)
    return NULL;
  char *stop = lf;
  if (stop > start && stop[-1] == '\r')
    stop--;
  *stop = '\0';
  *line_len = (size_t)(stop - start);
  *cursor = lf + 1;
  return start;
}

static int parse_request_line(char *line, http_request *req) {
  char *sp1 = strchr(line, ' ');
  if (!sp1)
    return 400;
  char *sp2 = strchr(sp1 + 1, ' ');
  if (!sp2 || strchr(sp2 + 1, ' '))
    return 400;
  *sp1 = '\0';
  *sp2 = '\0';

  req->method = line;
  req->target = sp1 + 1;
  req->version = sp2 + 1;

  if (!is_token(req->method))
    return 400;
  if (req->target[0] == '\0' || !is_valid_target(req->target))
    return 400;
  if (strlen(req->target) > HTTP_MAX_URI)
    return 414;

  const char *v = req->version;
  if (strncmp(v, "HTTP/", 5) != 0 || v[5] < '0' || v[5] > '9' || v[6] != '.' ||
      v[7] < '0' || v[7] > '9' || v[8] != '\0')
    return 400;
  if (v[5] != '1')
    return 505;
  req->minor = v[7] - '0';
  if (req->minor > 1)
    req->minor = 1;

  char *uri = req->target;
  if (ascii_ieq_n(uri, "http://", 7) || ascii_ieq_n(uri, "https://", 8)) {
    char *authority = strstr(uri, "://") + 3;
    char *slash = strchr(authority, '/');
    uri = slash ? slash : (char *)"/";
  }
  if (strcmp(uri, "*") == 0) {
    if (strcmp(req->method, "OPTIONS") != 0)
      return 400;
  } else if (uri[0] != '/') {
    return 400;
  }

  size_t n = strlen(uri);
  memcpy(req->uri, uri, n + 1);
  req->path = req->uri;
  char *q = strchr(req->uri, '?');
  if (q) {
    *q = '\0';
    req->query = q + 1;
  } else {
    req->query = req->uri + n;
  }
  char *frag = strchr(req->query, '#');
  if (frag)
    *frag = '\0';
  return 0;
}

static int parse_header_line(char *line, http_request *req) {
  if (is_ows(line[0]))
    return 400;
  char *colon = strchr(line, ':');
  if (!colon)
    return 400;
  *colon = '\0';
  if (!is_token(line))
    return 400;

  char *value = colon + 1;
  while (is_ows(*value))
    value++;
  char *vend = value + strlen(value);
  while (vend > value && is_ows(vend[-1]))
    vend--;
  *vend = '\0';
  if (!is_valid_field_value(value))
    return 400;

  if (req->header_count >= HTTP_MAX_HEADERS)
    return 431;
  req->headers[req->header_count].name = line;
  req->headers[req->header_count].value = value;
  req->header_count++;
  return 0;
}

static bool parse_content_length(const char *s, uint64_t *out) {
  bool have = false;
  uint64_t result = 0;
  while (*s) {
    while (is_ows(*s))
      s++;
    if (*s < '0' || *s > '9')
      return false;
    uint64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
      if (v > (UINT64_MAX - 9) / 10)
        return false;
      v = v * 10 + (uint64_t)(*s - '0');
    }
    while (is_ows(*s))
      s++;
    if (have && v != result)
      return false;
    result = v;
    have = true;
    if (*s == ',')
      s++;
    else if (*s != '\0')
      return false;
  }
  if (!have)
    return false;
  *out = result;
  return true;
}

int http_parse_head(char *buf, size_t len, http_request *req) {
  memset(req, 0, offsetof(http_request, uri));
  req->uri[0] = '\0';
  req->header_count = 0;
  req->body = NULL;
  req->body_len = 0;
  req->keep_alive = false;
  req->expect_continue = false;
  req->has_content_length = false;
  req->content_length = 0;
  req->has_transfer_encoding = false;

  char *cursor = buf;
  char *end = buf + len;
  size_t line_len = 0;

  /* RFC 9112 2.2: ignore empty lines received before the request line. */
  char *line;
  do {
    line = take_line(&cursor, end, &line_len);
    if (!line)
      return 400;
  } while (line_len == 0);

  if (strlen(line) != line_len)
    return 400; /* embedded NUL */
  int rc = parse_request_line(line, req);
  if (rc)
    return rc;

  for (;;) {
    line = take_line(&cursor, end, &line_len);
    if (!line)
      return 400; /* head did not end with an empty line */
    if (line_len == 0)
      break;
    if (strlen(line) != line_len)
      return 400;
    rc = parse_header_line(line, req);
    if (rc)
      return rc;
  }

  /* ---- header semantics ---- */
  size_t host_count = 0;
  for (size_t i = 0; i < req->header_count; i++) {
    const http_header *h = &req->headers[i];
    if (ascii_ieq(h->name, "Host")) {
      host_count++;
    } else if (ascii_ieq(h->name, "Content-Length")) {
      uint64_t v;
      if (!parse_content_length(h->value, &v))
        return 400;
      if (req->has_content_length && v != req->content_length)
        return 400;
      req->has_content_length = true;
      req->content_length = v;
    } else if (ascii_ieq(h->name, "Transfer-Encoding")) {
      req->has_transfer_encoding = true;
    } else if (ascii_ieq(h->name, "Expect")) {
      if (!ascii_ieq(h->value, "100-continue"))
        return 417;
      req->expect_continue = req->minor >= 1;
    }
  }
  if (req->minor >= 1 && host_count != 1)
    return 400;
  if (host_count > 1)
    return 400;
  /* Both framing headers at once is a request smuggling vector. */
  if (req->has_transfer_encoding && req->has_content_length)
    return 400;
  if (req->has_transfer_encoding)
    return 501;

  const char *conn = http_header_get(req, "Connection");
  if (conn && http_has_token(conn, "close"))
    req->keep_alive = false;
  else if (req->minor >= 1)
    req->keep_alive = true;
  else
    req->keep_alive = conn && http_has_token(conn, "keep-alive");

  return 0;
}

const char *http_header_get(const http_request *req, const char *name) {
  for (size_t i = 0; i < req->header_count; i++)
    if (ascii_ieq(req->headers[i].name, name))
      return req->headers[i].value;
  return NULL;
}

bool http_has_token(const char *value, const char *token) {
  size_t tlen = strlen(token);
  const char *p = value;
  while (*p) {
    while (*p == ',' || is_ows(*p))
      p++;
    const char *start = p;
    while (*p && *p != ',')
      p++;
    const char *stop = p;
    while (stop > start && is_ows(stop[-1]))
      stop--;
    if ((size_t)(stop - start) == tlen && ascii_ieq_n(start, token, tlen))
      return true;
  }
  return false;
}

/* ---- tables ------------------------------------------------------------ */

const char *http_status_text(int status) {
  switch (status) {
  case 100:
    return "Continue";
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 204:
    return "No Content";
  case 206:
    return "Partial Content";
  case 301:
    return "Moved Permanently";
  case 302:
    return "Found";
  case 304:
    return "Not Modified";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 408:
    return "Request Timeout";
  case 411:
    return "Length Required";
  case 413:
    return "Content Too Large";
  case 414:
    return "URI Too Long";
  case 416:
    return "Range Not Satisfiable";
  case 417:
    return "Expectation Failed";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  case 503:
    return "Service Unavailable";
  case 505:
    return "HTTP Version Not Supported";
  default:
    return "Unknown";
  }
}

typedef struct {
  const char *ext;
  const char *type;
} mime_entry;

static const mime_entry MIME_TYPES[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"map", "application/json"},
    {"txt", "text/plain; charset=utf-8"},
    {"md", "text/markdown; charset=utf-8"},
    {"c", "text/plain; charset=utf-8"},
    {"h", "text/plain; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"xml", "application/xml"},
    {"svg", "image/svg+xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"avif", "image/avif"},
    {"ico", "image/x-icon"},
    {"bmp", "image/bmp"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"mp3", "audio/mpeg"},
    {"wav", "audio/wav"},
    {"ogg", "audio/ogg"},
    {"mp4", "video/mp4"},
    {"webm", "video/webm"},
    {"pdf", "application/pdf"},
    {"zip", "application/zip"},
    {"gz", "application/gzip"},
    {"tar", "application/x-tar"},
    {"wasm", "application/wasm"},
};

const char *http_mime_type(const char *path) {
  const char *slash = strrchr(path, '/');
  const char *dot = strrchr(slash ? slash : path, '.');
  if (dot && dot[1]) {
    for (size_t i = 0; i < sizeof MIME_TYPES / sizeof MIME_TYPES[0]; i++)
      if (ascii_ieq(dot + 1, MIME_TYPES[i].ext))
        return MIME_TYPES[i].type;
  }
  return "application/octet-stream";
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool http_url_decode(const char *in, char *out, size_t out_len) {
  size_t o = 0;
  for (const char *p = in; *p; p++) {
    if (o + 1 >= out_len)
      return false;
    if (*p == '%') {
      int hi = hex_value(p[1]);
      int lo = hi < 0 ? -1 : hex_value(p[2]);
      if (hi < 0 || lo < 0)
        return false;
      int v = hi * 16 + lo;
      if (v == 0)
        return false;
      out[o++] = (char)v;
      p += 2;
    } else {
      out[o++] = *p;
    }
  }
  if (o >= out_len)
    return false;
  out[o] = '\0';
  return true;
}
