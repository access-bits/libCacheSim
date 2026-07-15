/*
 * progress_reporter.h — Unified progress reporting for trace readers
 *
 * Provides throttled progress output to stderr with automatic clearing
 * before log statements (via pre-log callback).
 *
 * Usage:
 *   progress_reporter_init();                     // at start of simulation
 *   progress_reporter_report(reader, start_ns);  // in main loop (throttled)
 *   progress_reporter_shutdown();                // at end (cleanup)
 *
 * The progress bar is automatically cleared before any LOG() statement
 * via the registered pre-log callback.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct reader;

/**
 * Get current time in nanoseconds since arbitrary epoch
 */
int64_t progress_reporter_get_time_ns(void);

/**
 * Initialize progress reporting system
 * Registers pre-log callback to clear progress bar before logging
 */
void progress_reporter_init(void);

/**
 * Shutdown progress reporting system
 * Clears any active progress bar from stderr
 */
void progress_reporter_shutdown(void);

/**
 * Report progress for the given reader
 * Throttled internally (updates ~2x/sec, minimum 0.5s between updates)
 *
 * Prints to stderr: [progress_bar] percentage | current/total | rate/s | elapsed | ETA
 *
 * @param reader        trace reader with n_read_req and n_total_req
 * @param start_time_ns start time in nanoseconds (from get_time_ns())
 */
void progress_reporter_report(struct reader *reader, int64_t start_time_ns);

#ifdef __cplusplus
}
#endif
