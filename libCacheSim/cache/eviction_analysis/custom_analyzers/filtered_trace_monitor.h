/*
 * filtered_trace_monitor.h — Eviction analyzer for filtered trace monitoring
 *
 * Tracks objects with tag=0 in a fixed-size buffer and counts evictions
 * of objects present in the buffer. Fast path for critical simulation loop.
 */

#pragma once

#include "libCacheSim/eviction_analyzer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create a filtered trace monitor analyzer
 *
 * @param name      analyzer name (e.g., "filteredTraceMonitor")
 * @param params    parameter string: filter_size (default: "64")
 *                  formats: "64" or "filter_size=64"
 * @return          eviction_analyzer_t pointer, or NULL on error
 */
eviction_analyzer_t *filtered_trace_monitor_create(const char *name,
                                                    const char *params);

#ifdef __cplusplus
}
#endif
