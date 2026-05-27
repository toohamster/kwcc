/* llog.h — logging wrapper
 *
 * Wraps rxi/log.h, hiding the internal level constants and
 * avoiding macOS syslog.h macro conflicts.
 *
 * Usage:
 *   #include "llog.h"
 *   log_info("value = %d", x);
 *   log_error("failed: %s", msg);
 */
#ifndef LLOG_H
#define LLOG_H

#include <stdio.h>

#ifdef __APPLE__
#undef LOG_INFO
#undef LOG_DEBUG
#undef LOG_WARNING
#undef LOG_TRACE
#undef LOG_ERROR
#endif

#include "log/log.h"

/* Internal level constants (not meant for public use) */
enum {
    _L_TRACE = LOG_TRACE,
    _L_DEBUG = LOG_DEBUG,
    _L_INFO  = LOG_INFO,
    _L_WARN  = LOG_WARN,
    _L_ERROR = LOG_ERROR,
    _L_FATAL = LOG_FATAL,
};

/* Public API — just log */
#define log_trace(...) log_log(_L_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_log(_L_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_log(_L_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_log(_L_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(_L_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(_L_FATAL, __FILE__, __LINE__, __VA_ARGS__)

#endif
