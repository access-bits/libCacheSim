/*
 * progress_reporter.c — Unified progress reporting for trace readers
 *
 * Provides:
 *  - Throttled progress output to stderr
 *  - Automatic clearing before LOG statements (via pre-log callback)
 *  - Rate calculation with smoothing
 *  - Human-readable formatting (K/M/G suffixes, hh:mm:ss)
 */

#include "progress_reporter.h"
#include "libCacheSim/reader.h"
#include "libCacheSim/log.h"

#include <stdio.h>
#include <time.h>
#include <string.h>

/* ================================================================
 * Utilities
 * ================================================================ */

/**
 * Get current time in nanoseconds since arbitrary epoch
 */
int64_t progress_reporter_get_time_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Internal use only */
static int64_t get_time_ns(void) {
  return progress_reporter_get_time_ns();
}

/**
 * Format large number with K/M/G suffixes for display
 */
static const char *format_num_suffix(uint64_t n, char *buf, size_t bufsize) {
  if (n >= 1000000000) {
    snprintf(buf, bufsize, "%luG", n / 1000000000);
  } else if (n >= 1000000) {
    snprintf(buf, bufsize, "%luM", n / 1000000);
  } else if (n >= 1000) {
    snprintf(buf, bufsize, "%luK", n / 1000);
  } else {
    snprintf(buf, bufsize, "%lu", n);
  }
  return buf;
}

/**
 * Format time duration in human-readable format (hh:mm:ss or mm:ss)
 */
static const char *format_time_duration(double seconds, char *buf, size_t bufsize) {
  int total_sec = (int)seconds;
  int hours = total_sec / 3600;
  int mins = (total_sec % 3600) / 60;
  int secs = total_sec % 60;
  
  if (hours > 0) {
    snprintf(buf, bufsize, "%dh%02dm%02ds", hours, mins, secs);
  } else {
    snprintf(buf, bufsize, "%dm%02ds", mins, secs);
  }
  return buf;
}

/* ================================================================
 * Progress tracking state
 * ================================================================ */

static bool progress_bar_displayed = false;

/* ---- Internal progress tracking state ---- */
static struct {
  uint64_t last_update_count;
  int64_t  last_update_time_ns;
  double   smoothed_rate;
} g_progress_state = {0, 0, 0.0};

/**
 * Clear progress bar from stderr
 * Called before every log statement (via pre-log callback)
 */
static void clear_progress_bar(void) {
  if (!progress_bar_displayed) {
    return;
  }
  /* Return to start of line and clear to end */
  fprintf(stderr, "\r\033[K");
  fflush(stderr);
  progress_bar_displayed = false;
}

/**
 * Pre-log callback — clears progress bar before logger writes
 * Registered via log_set_pre_log_callback
 */
static void progress_pre_log_callback(void) {
  clear_progress_bar();
}

/* ================================================================
 * Public API
 * ================================================================ */

void progress_reporter_init(void) {
  progress_bar_displayed = false;
  g_progress_state.last_update_count   = 0;
  g_progress_state.last_update_time_ns = 0;
  g_progress_state.smoothed_rate       = 0.0;
  log_set_pre_log_callback(progress_pre_log_callback);
}

void progress_reporter_shutdown(void) {
  clear_progress_bar();
}

void progress_reporter_report(struct reader *reader, int64_t start_time_ns) {
  if (reader == NULL) {
    return;
  }

  int64_t now_ns = get_time_ns();
  double elapsed_since_update_sec =
      (now_ns - g_progress_state.last_update_time_ns) / 1e9;

  /* Throttle updates to ~2 per second (0.5 sec minimum between updates) */
  if (elapsed_since_update_sec < 0.5 && g_progress_state.last_update_count > 0) {
    return;
  }
  
  uint64_t current = reader->n_read_req;
  uint64_t total = reader->n_total_req;
  
  if (total == 0 || current == 0) {
    return;
  }
  
  double total_elapsed = (now_ns - start_time_ns) / 1e9;
  double percent = (100.0 * current) / total;
  
  /* Calculate rate (requests/sec) with EMA smoothing: 0.25*current + 0.75*previous */
  uint64_t delta = current - g_progress_state.last_update_count;
  double instant_rate = (elapsed_since_update_sec > 0) ? (delta / elapsed_since_update_sec) : 0.0;
  double rate;
  if (g_progress_state.smoothed_rate == 0.0) {
    rate = instant_rate;  /* cold start: use raw rate */
  } else {
    rate = 0.1 * instant_rate + 0.9 * g_progress_state.smoothed_rate;
  }
  
  /* Calculate ETA */
  double eta_sec = (rate > 0 && total > current) ? ((total - current) / rate) : 0.0;
  
  /* Format progress bar */
  int bar_width = 30;
  int filled = (int)(bar_width * percent / 100.0);
  
  char bar[35];
  snprintf(bar, sizeof(bar), "[");
  for (int i = 0; i < filled && i < bar_width; i++) {
    strcat(bar, "=");
  }
  if (filled < bar_width) {
    strcat(bar, ">");
  }
  for (int i = filled + 1; i < bar_width; i++) {
    strcat(bar, " ");
  }
  strcat(bar, "]");
  
  /* Format numbers with suffixes */
  char curr_str[16], total_str[16], rate_str[16];
  format_num_suffix(current, curr_str, sizeof(curr_str));
  format_num_suffix(total, total_str, sizeof(total_str));
  format_num_suffix((uint64_t)rate, rate_str, sizeof(rate_str));
  
  /* Format times */
  char elapsed_str[32], eta_str[32];
  format_time_duration(total_elapsed, elapsed_str, sizeof(elapsed_str));
  format_time_duration(eta_sec, eta_str, sizeof(eta_str));
  
  /* Print progress line (overwrite with \r and clear to EOL with \033[K) */
  fprintf(stderr, "\r\033[K[%s] %5.1f%% | %s/%s | %s/s | %s elapsed",
          bar, percent, curr_str, total_str, rate_str, elapsed_str);
  
  if (eta_sec > 0 && eta_sec < 86400) {  /* Only show ETA if < 24 hours */
    fprintf(stderr, " | ETA: %s", eta_str);
  }
  
  fflush(stderr);
  progress_bar_displayed = true;
  
  /* Update tracking */
  g_progress_state.last_update_count   = current;
  g_progress_state.last_update_time_ns = now_ns;
  g_progress_state.smoothed_rate       = rate;
}
