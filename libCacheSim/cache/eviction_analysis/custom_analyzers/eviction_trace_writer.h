/*
 * eviction_trace_writer.h — Eviction analyzer that logs evicted objects to CSV
 *
 * Buffers (clock_time, obj_id) for every eviction in memory (default 8M
 * entries) and writes the whole buffer out as an uncompressed CSV file
 * when the cache is freed (or whenever the buffer fills up).
 */

#pragma once

#include "libCacheSim/eviction_analyzer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create an eviction trace writer analyzer
 *
 * @param name      analyzer name (also used as default output filename prefix)
 * @param params    optional output file path, used as-is; defaults to
 *                  "<name>_evictions.csv" in the process's working directory
 * @return          eviction_analyzer_t pointer, or NULL on error
 */
eviction_analyzer_t *eviction_trace_writer_create(const char *name,
                                                   const char *params);

#ifdef __cplusplus
}
#endif
