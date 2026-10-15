#ifndef LOG_H
#define LOG_H

#include "platform.h"

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } log_level;

void log_init(log_level min_level, bool access_log);
void log_shutdown(void);

[[gnu::format(printf, 2, 3)]]
void log_msg(log_level level, const char *fmt, ...);

void log_access(const char *ip, const char *method, const char *target,
                const char *version, int status, uint64_t bytes,
                double elapsed_ms);

void http_date(time_t t, char *out, size_t out_len);

#endif
