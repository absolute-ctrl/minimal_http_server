#include "platform.h"

#include "strbuf.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void sb_init(strbuf *sb) {
  sb->data = NULL;
  sb->len = 0;
  sb->cap = 0;
  sb->failed = false;
}

void sb_free(strbuf *sb) {
  free(sb->data);
  sb_init(sb);
}

bool sb_reserve(strbuf *sb, size_t extra) {
  if (sb->failed)
    return false;
  if (extra > SIZE_MAX - sb->len - 1) {
    sb->failed = true;
    return false;
  }
  size_t need = sb->len + extra + 1; /* +1 keeps room for a terminating NUL */
  if (need <= sb->cap)
    return true;

  size_t cap = sb->cap ? sb->cap : 256;
  while (cap < need)
    cap = cap > SIZE_MAX / 2 ? need : cap * 2;
  char *p = realloc(sb->data, cap);
  if (!p) {
    sb->failed = true;
    return false;
  }
  sb->data = p;
  sb->cap = cap;
  return true;
}

void sb_append(strbuf *sb, const char *s, size_t n) {
  if (!sb_reserve(sb, n))
    return;
  if (n)
    memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}

void sb_puts(strbuf *sb, const char *s) { sb_append(sb, s, strlen(s)); }

void sb_printf(strbuf *sb, const char *fmt, ...) {
  if (sb->failed)
    return;
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    sb->failed = true;
    va_end(ap2);
    return;
  }
  if (sb_reserve(sb, (size_t)n)) {
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    sb->len += (size_t)n;
  }
  va_end(ap2);
}

void sb_html_escape(strbuf *sb, const char *s) {
  for (; *s; s++) {
    switch (*s) {
    case '&':
      sb_puts(sb, "&amp;");
      break;
    case '<':
      sb_puts(sb, "&lt;");
      break;
    case '>':
      sb_puts(sb, "&gt;");
      break;
    case '"':
      sb_puts(sb, "&quot;");
      break;
    case '\'':
      sb_puts(sb, "&#39;");
      break;
    default:
      sb_append(sb, s, 1);
      break;
    }
  }
}

void sb_url_encode(strbuf *sb, const char *s) {
  static const char HEX[] = "0123456789ABCDEF";
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned char c = *p;
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                c == '~';
    if (safe) {
      sb_append(sb, (const char *)p, 1);
    } else {
      char esc[3] = {'%', HEX[c >> 4], HEX[c & 15]};
      sb_append(sb, esc, 3);
    }
  }
}

void sb_json_escape(strbuf *sb, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
    case '"':
      sb_puts(sb, "\\\"");
      break;
    case '\\':
      sb_puts(sb, "\\\\");
      break;
    case '\n':
      sb_puts(sb, "\\n");
      break;
    case '\r':
      sb_puts(sb, "\\r");
      break;
    case '\t':
      sb_puts(sb, "\\t");
      break;
    default:
      if (c < 0x20)
        sb_printf(sb, "\\u%04x", c);
      else
        sb_append(sb, (const char *)&s[i], 1);
    }
  }
}
