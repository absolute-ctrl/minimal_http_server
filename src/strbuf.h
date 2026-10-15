#ifndef STRBUF_H
#define STRBUF_H

#include "platform.h"

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  bool failed;
} strbuf;

void sb_init(strbuf *sb);
void sb_free(strbuf *sb);
bool sb_reserve(strbuf *sb, size_t extra);
void sb_append(strbuf *sb, const char *s, size_t n);
void sb_puts(strbuf *sb, const char *s);

[[gnu::format(printf, 2, 3)]]
void sb_printf(strbuf *sb, const char *fmt, ...);

void sb_html_escape(strbuf *sb, const char *s);

void sb_url_encode(strbuf *sb, const char *s);

void sb_json_escape(strbuf *sb, const char *s, size_t n);

#endif
