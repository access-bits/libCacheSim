/*
 * log.h — libCacheSim multi-stream structured logger
 *
 * Single public header for all logging.  Include this directly, or include
 * "libCacheSim/logging.h" which is now a thin alias.
 *
 * Usage
 * -----
 *   LOG(WARN, STREAM_Cache,   "evicted %lu objects", n);
 *   LOG(DEBUG, STREAM_Profiler, "hit ratio %.3f", ratio);
 *   LOG(ERROR, STREAM_Main,  "fatal: %s", msg);   // logs then abort()s
 *
 * Fan-out
 * -------
 * When a worker thread has called log_set_thread_stream(config_stream), every
 * LOG() call writes to stdout + all.log + explicit stream file + config stream
 * file (if config_stream != explicit stream).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Log levels
 * ----------------------------------------------------------------------- */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
} log_level_t;

/* -----------------------------------------------------------------------
 * Stream identity — FNV-1a hash of the stream name
 * ----------------------------------------------------------------------- */

typedef uint32_t stream_id_t;

static inline uint32_t log_stream_hash(const char *name) {
    uint32_t h = 2166136261u;
    for (; *name; name++) {
        h ^= (uint8_t)*name;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

/* -----------------------------------------------------------------------
 * Stream definition / declaration macros
 *
 *   DEFINE_LOG_STREAM(Foo)          — in exactly one .c file
 *   DECLARE_LOG_STREAM_EXTERN(Foo)  — in headers / other .c files
 * ----------------------------------------------------------------------- */

#define DEFINE_LOG_STREAM(Name)                                     \
    stream_id_t STREAM_##Name = 0;                                  \
    __attribute__((constructor))                                     \
    static void _register_stream_##Name(void) {                     \
        STREAM_##Name = log_register_stream(#Name);                 \
    }

#define DECLARE_LOG_STREAM_EXTERN(Name) \
    extern stream_id_t STREAM_##Name;

/* -----------------------------------------------------------------------
 * Built-in stream declarations (defined in logger/log.c)
 * ----------------------------------------------------------------------- */

DECLARE_LOG_STREAM_EXTERN(Main)
DECLARE_LOG_STREAM_EXTERN(Reader)
DECLARE_LOG_STREAM_EXTERN(Cache)
DECLARE_LOG_STREAM_EXTERN(Eviction)
DECLARE_LOG_STREAM_EXTERN(Profiler)
DECLARE_LOG_STREAM_EXTERN(Utils)

/* -----------------------------------------------------------------------
 * Initialization config
 * ----------------------------------------------------------------------- */

typedef struct {
    const char *dir;     /* log directory; NULL or "" → stderr fallback */
    const char *level;   /* "DEBUG"/"INFO"/"WARN"/"ERROR"               */
    const char *streams; /* "ALL" or comma-separated stream names        */
} log_config_t;

/* -----------------------------------------------------------------------
 * API
 * ----------------------------------------------------------------------- */

stream_id_t  log_register_stream(const char *name);
void         log_init(const log_config_t *cfg);
void         log_shutdown(void);
void         log_set_thread_stream(stream_id_t stream);
stream_id_t  log_get_thread_stream(void);
bool         log_enabled(log_level_t level, stream_id_t stream);
void         log_writef(log_level_t level, stream_id_t stream,
                        const char *fmt, ...)
             __attribute__((format(printf, 3, 4)));
void         log_set_pre_log_callback(void (*cb)(void));
log_level_t  log_parse_level(const char *s);
const char  *log_level_str(log_level_t level);

/* -----------------------------------------------------------------------
 * LOG — primary write macro
 *
 *   LOG(DEBUG, STREAM_Cache, "hit ratio %.3f", ratio)
 *   LOG(ERROR, STREAM_Main,  "fatal: %s", msg)    ← always writes + abort()
 * ----------------------------------------------------------------------- */

#define LOG(level, stream, ...)                                              \
    do {                                                                     \
        if ((LOG_##level) == LOG_ERROR || log_enabled(LOG_##level, (stream)))\
            log_writef(LOG_##level, (stream), __VA_ARGS__);                  \
        if ((LOG_##level) == LOG_ERROR)                                      \
            abort();                                                         \
    } while (0)

/* -----------------------------------------------------------------------
 * Legacy macros — kept so old code that was not yet migrated still compiles.
 * All route to STREAM_Utils.  Prefer explicit LOG(level, STREAM_xxx, ...)
 * in new or migrated code.
 * ----------------------------------------------------------------------- */

#include <pthread.h>

/* Defined in utils/logging.c — kept for backward compat. */
extern pthread_mutex_t log_mtx;

void print_stack_trace(void);

#define VERBOSE(...) LOG(DEBUG, STREAM_Utils, __VA_ARGS__)
#define DEBUG(...)   LOG(DEBUG, STREAM_Utils, __VA_ARGS__)
#define INFO(...)    LOG(INFO,  STREAM_Utils, __VA_ARGS__)
#define WARN(...)    LOG(WARN,  STREAM_Utils, __VA_ARGS__)
/* ERROR already aborts via LOG(ERROR,...) — no extra abort() needed here. */
#define ERROR(...)   LOG(ERROR, STREAM_Utils, __VA_ARGS__)

#define WARN_ONCE(...)           \
  do {                           \
    static bool printed = false; \
    if (!printed) {              \
      WARN(__VA_ARGS__);         \
      printed = true;            \
    }                            \
  } while (0)

#define DEBUG_ONCE(...)          \
  do {                           \
    static bool printed = false; \
    if (!printed) {              \
      DEBUG(__VA_ARGS__);        \
      printed = true;            \
    }                            \
  } while (0)

#define INFO_ONCE(...)           \
  do {                           \
    static bool printed = false; \
    if (!printed) {              \
      INFO(__VA_ARGS__);         \
      printed = true;            \
    }                            \
  } while (0)

#ifdef __cplusplus
}
#endif
