#include "platform.h"

#include "files.h"

#include "log.h"
#include "response.h"
#include "strbuf.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

enum {
  PATH_BUF = 4096 + HTTP_MAX_URI + 64,
  SEND_CHUNK = 64 * 1024,
};

static int normalize_path(const char *in, char *out, size_t out_len) {
  size_t o = 0;
  out[0] = '\0';
  const char *p = in;
  while (*p) {
    while (*p == '/')
      p++;
    const char *seg = p;
    while (*p && *p != '/')
      p++;
    size_t n = (size_t)(p - seg);
    if (n == 0 || (n == 1 && seg[0] == '.'))
      continue;
    if (n == 2 && seg[0] == '.' && seg[1] == '.') {
      if (o == 0)
        return 400;
      while (o > 0 && out[o - 1] != '/')
        o--;
      if (o > 0)
        o--;
      out[o] = '\0';
      continue;
    }
    if (seg[0] == '.')
      return 404;
    for (size_t i = 0; i < n; i++) {
      if (seg[i] == '\\')
        return 400;
#ifdef _WIN32
      if (seg[i] == ':')
        return 400;
#endif
    }
    if (o + 1 + n + 1 > out_len)
      return 414;
    out[o++] = '/';
    memcpy(out + o, seg, n);
    o += n;
    out[o] = '\0';
  }
  if (o == 0) {
    out[0] = '/';
    out[1] = '\0';
  }
  return 0;
}

static bool path_prefix_eq(const char *a, const char *b, size_t n) {
#ifdef _WIN32
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x == '\\')
      x = '/';
    if (y == '\\')
      y = '/';
    if (x >= 'A' && x <= 'Z')
      x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z')
      y = (char)(y - 'A' + 'a');
    if (x != y)
      return false;
  }
  return true;
#else
  return strncmp(a, b, n) == 0;
#endif
}

static bool inside_root(const char *root, const char *path) {
  char real[PATH_BUF];
  if (!fs_realpath(path, real, sizeof real))
    return false;
  size_t n = strlen(root);
  if (!path_prefix_eq(real, root, n))
    return false;
  if (real[n] == '\0' || real[n] == '/' || real[n] == '\\')
    return true;
  return n > 0 && (root[n - 1] == '/' || root[n - 1] == '\\');
}

static bool etag_matches(const char *header, const char *etag) {
  const char *p = header;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',')
      p++;
    if (*p == '*')
      return true;
    if (p[0] == 'W' && p[1] == '/')
      p += 2;
    const char *start = p;
    while (*p && *p != ',')
      p++;
    const char *stop = p;
    while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t'))
      stop--;
    if ((size_t)(stop - start) == strlen(etag) &&
        strncmp(start, etag, strlen(etag)) == 0)
      return true;
  }
  return false;
}

static bool parse_u64(const char **pp, uint64_t *out) {
  const char *p = *pp;
  if (*p < '0' || *p > '9')
    return false;
  uint64_t v = 0;
  for (; *p >= '0' && *p <= '9'; p++) {
    if (v > (UINT64_MAX - 9) / 10)
      return false;
    v = v * 10 + (uint64_t)(*p - '0');
  }
  *pp = p;
  *out = v;
  return true;
}

static int parse_range(const char *h, uint64_t size, uint64_t *start,
                       uint64_t *end) {
  if (strncmp(h, "bytes=", 6) != 0)
    return 0;
  const char *p = h + 6;
  while (*p == ' ')
    p++;
  if (strchr(p, ','))
    return 0;

  uint64_t a = 0, b = 0;
  if (*p == '-') {
    p++;
    if (!parse_u64(&p, &b))
      return 0;
    while (*p == ' ')
      p++;
    if (*p)
      return 0;
    if (b == 0 || size == 0)
      return -1;
    *start = b >= size ? 0 : size - b;
    *end = size - 1;
    return 1;
  }
  if (!parse_u64(&p, &a) || *p != '-')
    return 0;
  p++;
  bool open_ended = (*p < '0' || *p > '9');
  if (!open_ended && !parse_u64(&p, &b))
    return 0;
  while (*p == ' ')
    p++;
  if (*p)
    return 0;
  if (!open_ended && b < a)
    return 0;
  if (a >= size)
    return -1;
  *start = a;
  *end = (open_ended || b >= size) ? size - 1 : b;
  return 1;
}

static void serve_file(client_conn *c, const http_request *req,
                       const char *fs_path, const file_info *fi) {
  char etag[64];
  snprintf(etag, sizeof etag, "\"%llx-%llx\"", (unsigned long long)fi->size,
           (unsigned long long)fi->mtime);
  char last_mod[64];
  http_date((time_t)fi->mtime, last_mod, sizeof last_mod);

  strbuf hdr;
  sb_init(&hdr);
  sb_printf(&hdr,
            "ETag: %s\r\nLast-Modified: %s\r\nCache-Control: no-cache\r\n",
            etag, last_mod);

  const char *inm = http_header_get(req, "If-None-Match");
  const char *ims = http_header_get(req, "If-Modified-Since");
  if ((inm && etag_matches(inm, etag)) ||
      (!inm && ims && strcmp(ims, last_mod) == 0)) {
    respond(c, req, 304, NULL, NULL, 0, hdr.data);
    sb_free(&hdr);
    return;
  }

  sb_puts(&hdr, "Accept-Ranges: bytes\r\n");

  uint64_t start = 0, end = fi->size ? fi->size - 1 : 0, length = fi->size;
  int status = 200;
  const char *range = http_header_get(req, "Range");
  const char *if_range = http_header_get(req, "If-Range");
  if (range && (!if_range || strcmp(if_range, etag) == 0 ||
                strcmp(if_range, last_mod) == 0)) {
    int r = parse_range(range, fi->size, &start, &end);
    if (r < 0) {
      sb_printf(&hdr, "Content-Range: bytes */%llu\r\n",
                (unsigned long long)fi->size);
      respond_error(c, req, 416, hdr.data);
      sb_free(&hdr);
      return;
    }
    if (r > 0) {
      status = 206;
      length = end - start + 1;
      sb_printf(&hdr, "Content-Range: bytes %llu-%llu/%llu\r\n",
                (unsigned long long)start, (unsigned long long)end,
                (unsigned long long)fi->size);
    }
  }

  FILE *f = fs_open_read(fs_path);
  if (!f || (start > 0 && !fs_seek(f, start))) {
    if (f)
      fclose(f);
    sb_free(&hdr);
    respond_error(c, req, 403, NULL);
    return;
  }

  bool ok =
      !hdr.failed && respond_headers(c, req, status, http_mime_type(fs_path),
                                     length, hdr.data);
  sb_free(&hdr);

  if (ok && strcmp(req->method, "HEAD") != 0) {
    char *chunk = malloc(SEND_CHUNK);
    uint64_t remaining = length;
    while (chunk && remaining > 0) {
      size_t want = remaining < SEND_CHUNK ? (size_t)remaining : SEND_CHUNK;
      size_t got = fread(chunk, 1, want, f);
      if (got == 0 || !respond_body(c, req, chunk, got))
        break;
      remaining -= got;
    }
    if (remaining > 0)
      c->close_after = true;
    free(chunk);
  }
  fclose(f);
}

typedef struct {
  char *name;
  bool is_dir;
  uint64_t size;
  int64_t mtime;
} dir_entry;

static int compare_entries(const void *a, const void *b) {
  const dir_entry *x = a, *y = b;
  if (x->is_dir != y->is_dir)
    return x->is_dir ? -1 : 1;
  return strcmp(x->name, y->name);
}

static void format_size(uint64_t size, char *out, size_t len) {
  static const char *const UNITS[] = {"B", "KB", "MB", "GB", "TB"};
  double v = (double)size;
  size_t u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    u++;
  }
  if (u == 0)
    snprintf(out, len, "%llu B", (unsigned long long)size);
  else
    snprintf(out, len, "%.1f %s", v, UNITS[u]);
}

static void serve_listing(client_conn *c, const http_request *req,
                          const char *fs_dir, const char *url_path) {
  DIR *d = opendir(fs_dir);
  if (!d) {
    respond_error(c, req, 403, NULL);
    return;
  }

  dir_entry *entries = NULL;
  size_t count = 0, cap = 0;
  bool oom = false;
  struct dirent *de;
  while ((de = readdir(d)) != NULL && !oom) {
    if (de->d_name[0] == '.')
      continue; /* ., .., and hidden files */
    char child[PATH_BUF];
    int n = snprintf(child, sizeof child, "%s/%s", fs_dir, de->d_name);
    if (n < 0 || (size_t)n >= sizeof child)
      continue;
    file_info fi = fs_stat(child);
    if (!fi.exists || (!fi.is_dir && !fi.is_regular))
      continue;

    if (count == cap) {
      size_t ncap = cap ? cap * 2 : 64;
      dir_entry *grown = realloc(entries, ncap * sizeof *entries);
      if (!grown) {
        oom = true;
        break;
      }
      entries = grown;
      cap = ncap;
    }
    size_t name_len = strlen(de->d_name);
    char *name = malloc(name_len + 1);
    if (!name) {
      oom = true;
      break;
    }
    memcpy(name, de->d_name, name_len + 1);
    entries[count++] = (dir_entry){name, fi.is_dir, fi.size, fi.mtime};
  }
  closedir(d);

  if (count)
    qsort(entries, count, sizeof *entries, compare_entries);

  strbuf sb;
  sb_init(&sb);
  sb_puts(
      &sb,
      "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>Index of ");
  sb_html_escape(&sb, url_path);
  sb_puts(&sb,
          "</title><style>"
          "body{font-family:system-ui,sans-serif;max-width:60em;margin:2em "
          "auto;padding:0 1em;"
          "color:#222}h1{font-weight:600;font-size:1.4em;word-break:break-all}"
          "table{border-collapse:collapse;width:100%}th,td{text-align:left;"
          "padding:.35em .6em;"
          "border-bottom:1px solid "
          "#eee}td.n{text-align:right;white-space:nowrap;color:#555}"
          "th.n{text-align:right}a{color:#0645ad;text-decoration:none}a:hover{"
          "text-decoration:"
          "underline}small{color:#777}"
          "@media(prefers-color-scheme:dark){body{background:#111;color:#ddd}"
          "th,td{border-color:#333}a{color:#8ab4f8}td.n{color:#aaa}}"
          "</style></head>\n<body><h1>Index of ");
  sb_html_escape(&sb, url_path);
  sb_puts(&sb, "</h1>\n<table><thead><tr><th>Name</th><th class=\"n\">Size</th>"
               "<th class=\"n\">Modified (UTC)</th></tr></thead><tbody>\n");
  if (strcmp(url_path, "/") != 0)
    sb_puts(&sb,
            "<tr><td><a href=\"../\">../</a></td><td></td><td></td></tr>\n");

  for (size_t i = 0; i < count; i++) {
    char size[32] = "-";
    if (!entries[i].is_dir)
      format_size(entries[i].size, size, sizeof size);
    char when[32] = "";
    struct tm tm;
    if (time_utc((time_t)entries[i].mtime, &tm))
      strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);

    sb_puts(&sb, "<tr><td><a href=\"");
    sb_url_encode(&sb, entries[i].name);
    if (entries[i].is_dir)
      sb_puts(&sb, "/");
    sb_puts(&sb, "\">");
    sb_html_escape(&sb, entries[i].name);
    if (entries[i].is_dir)
      sb_puts(&sb, "/");
    sb_printf(&sb,
              "</a></td><td class=\"n\">%s</td><td class=\"n\">%s</td></tr>\n",
              size, when);
    free(entries[i].name);
  }
  free(entries);
  sb_puts(&sb, "</tbody></table>\n<p><small>" SERVER_NAME
               "</small></p></body></html>\n");

  if (oom || sb.failed)
    respond_error(c, req, 500, NULL);
  else
    respond(c, req, 200, "text/html; charset=utf-8", sb.data, sb.len,
            "Cache-Control: no-cache\r\n");
  sb_free(&sb);
}

void files_serve(client_conn *c, const http_request *req) {
  const char *root = c->cfg->root;

  char decoded[HTTP_MAX_URI + 1];
  if (!http_url_decode(req->path, decoded, sizeof decoded)) {
    respond_error(c, req, 400, NULL);
    return;
  }
  char clean[HTTP_MAX_URI + 2];
  int rc = normalize_path(decoded, clean, sizeof clean);
  if (rc) {
    respond_error(c, req, rc, NULL);
    return;
  }

  char fs_path[PATH_BUF];
  int n = snprintf(fs_path, sizeof fs_path, "%s%s", root,
                   strcmp(clean, "/") == 0 ? "" : clean);
  if (n < 0 || (size_t)n >= sizeof fs_path) {
    respond_error(c, req, 414, NULL);
    return;
  }

  file_info fi = fs_stat(fs_path);
  if (!fi.exists) {
    respond_error(c, req, 404, NULL);
    return;
  }
  if (!inside_root(root, fs_path)) {
    log_msg(LOG_WARN, "%s: blocked path outside root: %s", c->ip, fs_path);
    respond_error(c, req, 403, NULL);
    return;
  }

  if (fi.is_dir) {
    size_t plen = strlen(req->path);
    if (plen == 0 || req->path[plen - 1] != '/') {
      strbuf loc;
      sb_init(&loc);
      sb_printf(&loc, "Location: %s/%s%s\r\n", req->path,
                req->query[0] ? "?" : "", req->query);
      if (loc.failed)
        respond_error(c, req, 500, NULL);
      else
        respond_error(c, req, 301, loc.data);
      sb_free(&loc);
      return;
    }

    char index[PATH_BUF];
    n = snprintf(index, sizeof index, "%s/index.html", fs_path);
    if (n > 0 && (size_t)n < sizeof index) {
      file_info ifi = fs_stat(index);
      if (ifi.exists && ifi.is_regular) {
        serve_file(c, req, index, &ifi);
        return;
      }
    }
    if (!c->cfg->dir_listing) {
      respond_error(c, req, 403, NULL);
      return;
    }
    char shown[HTTP_MAX_URI + 3];
    snprintf(shown, sizeof shown, "%s%s", clean,
             strcmp(clean, "/") == 0 ? "" : "/");
    serve_listing(c, req, fs_path, shown);
    return;
  }

  if (!fi.is_regular) {
    respond_error(c, req, 403, NULL);
    return;
  }
  size_t plen = strlen(req->path);
  if (plen > 1 && req->path[plen - 1] == '/') {
    respond_error(c, req, 404, NULL);
    return;
  }
  serve_file(c, req, fs_path, &fi);
}
