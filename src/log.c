#include "platform.h"

#include "log.h"

#include <pthread.h>
#include <stdarg.h>

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static log_level g_min_level = LOG_INFO;
static bool g_access_log = true;

static const char *const LEVEL_NAMES[] = {"DEBUG", "INFO", "WARN", "ERROR"};

void log_init(log_level min_level, bool access_log) {
  g_min_level = min_level;
  g_access_log = access_log;
}

void log_shutdown(void) {
  pthread_mutex_lock(&g_log_lock);
  fflush(stdout);
  fflush(stderr);
  pthread_mutex_unlock(&g_log_lock);
}

static void timestamp(char *out, size_t len) {
  struct tm tm;
  if (!time_utc(time(NULL), &tm) ||
      strftime(out, len, "%Y-%m-%d %H:%M:%S", &tm) == 0)
    snprintf(out, len, "0000-00-00 00:00:00");
}

void log_msg(log_level level, const char *fmt, ...) {
  if (level < g_min_level)
    return;
  char ts[32];
  timestamp(ts, sizeof ts);

  FILE *out = level >= LOG_WARN ? stderr : stdout;
  va_list ap;
  va_start(ap, fmt);
  pthread_mutex_lock(&g_log_lock);
  fprintf(out, "%s [%s] ", ts, LEVEL_NAMES[level]);
  vfprintf(out, fmt, ap);
  fputc('\n', out);
  fflush(out);
  pthread_mutex_unlock(&g_log_lock);
  va_end(ap);
}

void log_access(const char *ip, const char *method, const char *target,
                const char *version, int status, uint64_t bytes,
                double elapsed_ms) {
  if (!g_access_log)
    return;
  char ts[40];
  struct tm tm;
  if (!time_utc(time(NULL), &tm) ||
      strftime(ts, sizeof ts, "%d/%b/%Y:%H:%M:%S +0000", &tm) == 0)
    ts[0] = '\0';

  pthread_mutex_lock(&g_log_lock);
  fprintf(stdout, "%s - - [%s] \"%s %s %s\" %d %llu %.2fms\n", ip, ts,
          method ? method : "-", target ? target : "-", version ? version : "-",
          status, (unsigned long long)bytes, elapsed_ms);
  fflush(stdout);
  pthread_mutex_unlock(&g_log_lock);
}

void http_date(time_t t, char *out, size_t out_len) {
  static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed",
                                     "Thu", "Fri", "Sat"};
  static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr",
                                       "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec"};
  struct tm tm;
  if (!time_utc(t, &tm)) {
    snprintf(out, out_len, "Thu, 01 Jan 1970 00:00:00 GMT");
    return;
  }
  snprintf(out, out_len, "%s, %02d %s %04d %02d:%02d:%02d GMT",
           DAYS[tm.tm_wday % 7], tm.tm_mday, MONTHS[tm.tm_mon % 12],
           tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}
