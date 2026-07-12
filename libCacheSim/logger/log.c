/*
 * log.c — multi-stream structured file logger for libCacheSim
 *
 * Design:
 *   - All writes serialised by a single pthread mutex
 *   - Lazy per-stream file opening (no empty log files)
 *   - Thread-local current stream for per-config fan-out routing:
 *       LOG(WARN, STREAM_Eviction, ...) on a worker thread writes to
 *       both Eviction.log AND the worker's config stream (e.g. config_0_lru_100mib.log)
 *   - Pre-init fallback to stderr (safe for tests that never call log_init)
 *   - Writes to: stdout + all.log + explicit stream file [+ config stream file]
 *
 * Format: [YYYY-MM-DD HH:MM:SS.mmm] [LEVEL  ] [StreamName   ] message
 */

#define _GNU_SOURCE
#include "libCacheSim/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define MAX_STREAMS      256
#define STREAM_NAME_LEN   64
#define LOG_LINE_MAX    2048

/* -----------------------------------------------------------------------
 * Stream registry entry
 * ----------------------------------------------------------------------- */

typedef struct {
    stream_id_t id;
    char        name[STREAM_NAME_LEN];
    FILE       *fp;              /* NULL until first write */
    bool        open_attempted;  /* avoid retrying failed opens */
} stream_entry_t;

/* -----------------------------------------------------------------------
 * Global logger state
 * ----------------------------------------------------------------------- */

typedef struct {
    log_level_t     min_level;
    bool            filter_streams;            /* false = ALL */
    stream_id_t     enabled_ids[MAX_STREAMS];
    int             n_enabled;

    char            log_dir[4096];
    FILE           *all_fp;

    stream_entry_t  streams[MAX_STREAMS];
    int             n_streams;

    void          (*pre_log_cb)(void);

    bool            initialized;
    pthread_mutex_t mtx;
} logger_t;

static logger_t g_log = {
    .min_level   = LOG_INFO,
    .initialized = false,
    .mtx         = PTHREAD_MUTEX_INITIALIZER,
};

/* Thread-local config stream.  0 = not set (no fan-out). */
static _Thread_local stream_id_t tl_config_stream = 0;

/* -----------------------------------------------------------------------
 * Built-in stream definitions — one per subsystem.
 * DEFINE_LOG_STREAM registers the name via __attribute__((constructor)).
 * ----------------------------------------------------------------------- */

DEFINE_LOG_STREAM(Main)
DEFINE_LOG_STREAM(Reader)
DEFINE_LOG_STREAM(Cache)
DEFINE_LOG_STREAM(Eviction)
DEFINE_LOG_STREAM(Profiler)
DEFINE_LOG_STREAM(Utils)

/* -----------------------------------------------------------------------
 * log_parse_level / log_level_str
 * ----------------------------------------------------------------------- */

log_level_t log_parse_level(const char *s) {
    if (!s || !*s) return LOG_INFO;
    char upper[16] = {0};
    for (int i = 0; i < 15 && s[i]; i++)
        upper[i] = (char)((s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i]);
    if (strcmp(upper, "DEBUG")   == 0) return LOG_DEBUG;
    if (strcmp(upper, "INFO")    == 0) return LOG_INFO;
    if (strcmp(upper, "WARN")    == 0 || strcmp(upper, "WARNING") == 0) return LOG_WARN;
    if (strcmp(upper, "ERROR")   == 0) return LOG_ERROR;
    fprintf(stderr, "log: unknown level '%s', defaulting to INFO.\n", s);
    return LOG_INFO;
}

const char *log_level_str(log_level_t l) {
    switch (l) {
        case LOG_DEBUG: return "DEBUG  ";
        case LOG_INFO:  return "INFO   ";
        case LOG_WARN:  return "WARNING";
        case LOG_ERROR: return "ERROR  ";
    }
    return "UNKNOWN";
}

/* -----------------------------------------------------------------------
 * log_register_stream
 * ----------------------------------------------------------------------- */

stream_id_t log_register_stream(const char *name) {
    stream_id_t id = log_stream_hash(name);

    pthread_mutex_lock(&g_log.mtx);

    for (int i = 0; i < g_log.n_streams; i++) {
        if (g_log.streams[i].id == id) {
            pthread_mutex_unlock(&g_log.mtx);
            return id;
        }
    }

    if (g_log.n_streams < MAX_STREAMS) {
        stream_entry_t *e = &g_log.streams[g_log.n_streams++];
        e->id             = id;
        e->fp             = NULL;
        e->open_attempted = false;
        strncpy(e->name, name, STREAM_NAME_LEN - 1);
        e->name[STREAM_NAME_LEN - 1] = '\0';
    } else {
        fprintf(stderr, "log: MAX_STREAMS (%d) reached, cannot register '%s'\n",
                MAX_STREAMS, name);
    }

    pthread_mutex_unlock(&g_log.mtx);
    return id;
}

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

static const char *find_stream_name_locked(stream_id_t id) {
    for (int i = 0; i < g_log.n_streams; i++) {
        if (g_log.streams[i].id == id) return g_log.streams[i].name;
    }
    return "<unknown>";
}

/* Recursive mkdir — creates all intermediate directories. */
static int make_dir(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (size_t i = 1; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "log: mkdir '%s': %s\n", tmp, strerror(errno));
                return -1;
            }
            tmp[i] = saved;
        }
    }
    return 0;
}

/* Parse comma-separated stream filter into enabled_ids[]. */
static void parse_stream_filter(const char *s) {
    g_log.filter_streams = false;
    g_log.n_enabled      = 0;

    if (!s || !*s || strcmp(s, "ALL") == 0) return;

    g_log.filter_streams = true;
    char buf[1024];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = strtok(buf, ",");
    while (token) {
        while (*token == ' ' || *token == '\t') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t')) *end-- = '\0';

        stream_id_t id = log_stream_hash(token);
        bool found = false;
        for (int i = 0; i < g_log.n_streams; i++) {
            if (g_log.streams[i].id == id) { found = true; break; }
        }
        if (found) {
            if (g_log.n_enabled < MAX_STREAMS)
                g_log.enabled_ids[g_log.n_enabled++] = id;
        } else {
            fprintf(stderr, "log: unknown stream '%s' in filter, ignoring.\n", token);
        }
        token = strtok(NULL, ",");
    }
}

/* Lazy-open per-stream file.  Must be called under mtx. */
static FILE *get_stream_file_locked(stream_id_t id) {
    for (int i = 0; i < g_log.n_streams; i++) {
        stream_entry_t *e = &g_log.streams[i];
        if (e->id != id) continue;
        if (e->fp)             return e->fp;
        if (e->open_attempted) return NULL;

        e->open_attempted = true;
        char path[4096 + STREAM_NAME_LEN + 8];
        snprintf(path, sizeof(path), "%s/%s.log", g_log.log_dir, e->name);
        e->fp = fopen(path, "a");
        if (!e->fp)
            fprintf(stderr, "log: cannot open stream log '%s': %s\n",
                    path, strerror(errno));
        return e->fp;
    }
    return NULL;
}

/* Build the formatted log line. */
static void format_line(log_level_t level, stream_id_t stream,
                         const char *msg, char *out, size_t out_sz) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    char time_str[24];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);
    int ms = (int)(ts.tv_nsec / 1000000);

    const char *raw_name = find_stream_name_locked(stream);
    char sname[13];
    snprintf(sname, sizeof(sname), "%-12s", raw_name);

    snprintf(out, out_sz, "[%s.%03d] [%s] [%s] %s\n",
             time_str, ms, log_level_str(level), sname, msg);
}

/* -----------------------------------------------------------------------
 * log_init
 * ----------------------------------------------------------------------- */

void log_init(const log_config_t *cfg) {
    pthread_mutex_lock(&g_log.mtx);

    /* Close any previously opened files (supports re-init). */
    if (g_log.all_fp) { fclose(g_log.all_fp); g_log.all_fp = NULL; }
    for (int i = 0; i < g_log.n_streams; i++) {
        if (g_log.streams[i].fp) {
            fclose(g_log.streams[i].fp);
            g_log.streams[i].fp             = NULL;
            g_log.streams[i].open_attempted = false;
        }
    }

    g_log.min_level = log_parse_level(cfg->level);

    if (cfg->dir && cfg->dir[0]) {
        strncpy(g_log.log_dir, cfg->dir, sizeof(g_log.log_dir) - 1);
        g_log.log_dir[sizeof(g_log.log_dir) - 1] = '\0';
    } else {
        g_log.log_dir[0] = '\0';
    }

    parse_stream_filter(cfg->streams);

    if (!g_log.log_dir[0]) {
        /* No log dir — pre-init stderr fallback for all writes. */
        g_log.initialized = true;
        pthread_mutex_unlock(&g_log.mtx);
        return;
    }

    pthread_mutex_unlock(&g_log.mtx);

    if (make_dir(g_log.log_dir) != 0) {
        fprintf(stderr, "log: cannot create log directory '%s'\n", g_log.log_dir);
    }

    pthread_mutex_lock(&g_log.mtx);

    char all_path[4096 + 8];
    snprintf(all_path, sizeof(all_path), "%s/all.log", g_log.log_dir);
    g_log.all_fp = fopen(all_path, "a");
    if (!g_log.all_fp)
        fprintf(stderr, "log: cannot open '%s': %s\n", all_path, strerror(errno));

    g_log.initialized = true;
    pthread_mutex_unlock(&g_log.mtx);
}

/* -----------------------------------------------------------------------
 * log_shutdown
 * ----------------------------------------------------------------------- */

void log_shutdown(void) {
    pthread_mutex_lock(&g_log.mtx);

    if (g_log.all_fp) {
        fflush(g_log.all_fp);
        fclose(g_log.all_fp);
        g_log.all_fp = NULL;
    }
    for (int i = 0; i < g_log.n_streams; i++) {
        if (g_log.streams[i].fp) {
            fflush(g_log.streams[i].fp);
            fclose(g_log.streams[i].fp);
            g_log.streams[i].fp = NULL;
        }
    }
    g_log.initialized = false;

    pthread_mutex_unlock(&g_log.mtx);
}

/* -----------------------------------------------------------------------
 * Thread-local config stream routing
 * ----------------------------------------------------------------------- */

void log_set_thread_stream(stream_id_t stream) {
    tl_config_stream = stream;
}

stream_id_t log_get_thread_stream(void) {
    return tl_config_stream;
}

/* -----------------------------------------------------------------------
 * log_enabled — fast pre-check (called by LOG macro)
 * ----------------------------------------------------------------------- */

bool log_enabled(log_level_t level, stream_id_t stream) {
    if (level < g_log.min_level) return false;
    if (!g_log.filter_streams)   return true;
    for (int i = 0; i < g_log.n_enabled; i++) {
        if (g_log.enabled_ids[i] == stream) return true;
    }
    /* Also pass if the calling thread's config stream is enabled. */
    for (int i = 0; i < g_log.n_enabled; i++) {
        if (g_log.enabled_ids[i] == tl_config_stream) return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Internal: emit one formatted line to all destinations.
 * Must be called under mtx.
 * ----------------------------------------------------------------------- */

static void emit_line_locked(const char *line, stream_id_t explicit_stream) {
    /* stdout */
    fputs(line, stdout);
    fflush(stdout);

    /* all.log */
    if (g_log.all_fp) {
        fputs(line, g_log.all_fp);
        fflush(g_log.all_fp);
    }

    if (!g_log.log_dir[0]) return;

    /* explicit stream file */
    FILE *sf = get_stream_file_locked(explicit_stream);
    if (sf) { fputs(line, sf); fflush(sf); }

    /* fan-out: also write to thread-local config stream if different */
    if (tl_config_stream != 0 && tl_config_stream != explicit_stream) {
        FILE *cf = get_stream_file_locked(tl_config_stream);
        if (cf) { fputs(line, cf); fflush(cf); }
    }
}

/* -----------------------------------------------------------------------
 * log_writef — central write function (called by LOG macro)
 * ----------------------------------------------------------------------- */

void log_writef(log_level_t level, stream_id_t stream,
                const char *fmt, ...) {
    char msg[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Strip trailing newline — format_line adds one. */
    size_t mlen = strlen(msg);
    while (mlen > 0 && (msg[mlen - 1] == '\n' || msg[mlen - 1] == '\r'))
        msg[--mlen] = '\0';

    if (!g_log.initialized) {
        fprintf(stderr, "[PRE-INIT] [%s] %s\n", log_level_str(level), msg);
        return;
    }

    char line[LOG_LINE_MAX + 64];

    pthread_mutex_lock(&g_log.mtx);

    format_line(level, stream, msg, line, sizeof(line));

    if (g_log.pre_log_cb) g_log.pre_log_cb();

    emit_line_locked(line, stream);

    pthread_mutex_unlock(&g_log.mtx);
}

/* -----------------------------------------------------------------------
 * log_set_pre_log_callback
 * ----------------------------------------------------------------------- */

void log_set_pre_log_callback(void (*cb)(void)) {
    pthread_mutex_lock(&g_log.mtx);
    g_log.pre_log_cb = cb;
    pthread_mutex_unlock(&g_log.mtx);
}
