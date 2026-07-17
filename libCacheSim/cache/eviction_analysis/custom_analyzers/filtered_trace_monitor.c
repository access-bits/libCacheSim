/*
 * filtered_trace_monitor.c — Fast eviction analyzer for filtered trace monitoring
 *
 * Tracks objects with tag=0 (tagged "in filter") and reports how many
 * evicted objects were in the filter buffer.
 */

#include "filtered_trace_monitor.h"
#include "libCacheSim/cache.h"
#include "libCacheSim/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILTERED_TRACE_MONITOR_MAX_BUFFER 128

typedef struct {
  uint64_t obj_ids[FILTERED_TRACE_MONITOR_MAX_BUFFER]; /* Fixed buffer of object IDs */
  int size;                    /* Current count in buffer */
  int max_size;                /* Filter size limit */
  uint64_t evicted_count;      /* Count of evictions found in buffer */
  uint64_t tag0_count;         /* Diagnostic: objects added (tag=0) */
  uint64_t tag1_count;         /* Diagnostic: objects removed (tag=1) */
  uint64_t tag2_count;         /* Diagnostic: single-touch objects (tag=2) */
} filtered_trace_monitor_t;

#define FILTERED_TRACE_TAG_FEATURE_IDX 1

/* ================================================================
 * Private helpers (fast path)
 * ================================================================ */

/**
 * Check if obj_id is in buffer (linear search, O(n) with n ≤ 128)
 */
static bool obj_in_buffer(filtered_trace_monitor_t *ftm, uint64_t obj_id) {
  for (int i = 0; i < ftm->size; i++) {
    if (ftm->obj_ids[i] == obj_id) {
      return true;
    }
  }
  return false;
}

/**
 * Add obj_id to buffer (append if not full, warn if would overflow)
 */
static void buffer_add(filtered_trace_monitor_t *ftm, uint64_t obj_id) {
  /* Check if already present */
  if (obj_in_buffer(ftm, obj_id)) {
    LOG(WARN, STREAM_Cache, 
        "FilteredTraceMonitor: duplicate tag=0 for object %llu (already in buffer)",
        (unsigned long long)obj_id);
    return;
  }

  /* Add if space available */
  if (ftm->size < ftm->max_size) {
    ftm->obj_ids[ftm->size++] = obj_id;
  } else {
    /* Buffer would overflow — warn */
    LOG(WARN, STREAM_Cache, 
        "FilteredTraceMonitor: buffer full (%d/%d), dropping object %llu",
        ftm->size, ftm->max_size, (unsigned long long)obj_id);
  }
}

/**
 * Remove obj_id from buffer (swap-to-remove, O(n))
 */
static void buffer_remove(filtered_trace_monitor_t *ftm, uint64_t obj_id) {
  for (int i = 0; i < ftm->size; i++) {
    if (ftm->obj_ids[i] == obj_id) {
      /* Swap last element into this position */
      ftm->obj_ids[i] = ftm->obj_ids[--ftm->size];
      return;
    }
  }
  
  /* Object not found in buffer - warn about trace inconsistency */
  LOG(WARN, STREAM_Cache, 
      "FilteredTraceMonitor: tag=1 for object %llu not in buffer (missing tag=0?)",
      (unsigned long long)obj_id);
}

/* ================================================================
 * Analyzer interface
 * ================================================================ */

static void ftm_start(eviction_analyzer_t *self) {
  filtered_trace_monitor_t *ftm = (filtered_trace_monitor_t *)self->data;
  ftm->size = 0;
  ftm->evicted_count = 0;
  ftm->tag0_count = 0;
  ftm->tag1_count = 0;
  ftm->tag2_count = 0;
  LOG(INFO, STREAM_Cache, "FilteredTraceMonitor started (filter_size=%d)",
      ftm->max_size);
}

static void ftm_process(eviction_analyzer_t *self, cache_obj_t *evicted_obj,
                        request_t *req) {
  filtered_trace_monitor_t *ftm = (filtered_trace_monitor_t *)self->data;

  /* Check if this request tagged an object */
  if (req && req->n_features > FILTERED_TRACE_TAG_FEATURE_IDX) {
    uint8_t tag = (uint8_t)req->features[FILTERED_TRACE_TAG_FEATURE_IDX];

    if (tag == 0) {
      /* Tag 0: object enters trace filter */
      buffer_add(ftm, req->obj_id);
      ftm->tag0_count++;
    } else if (tag == 1) {
      /* Tag 1: object leaves trace filter (normal multi-touch exit) */
      buffer_remove(ftm, req->obj_id);
      ftm->tag1_count++;
    } else if (tag == 2) {
      /* Tag 2: single-touch object (entered and left in same window) */
      /* Don't add to buffer since it's already gone */
      ftm->tag2_count++;
    }
  }

  /* Check if evicted cache object is currently in trace filter */
  if (evicted_obj != NULL) {
    if (obj_in_buffer(ftm, evicted_obj->obj_id)) {
      LOG(INFO, STREAM_Cache,
          "FilteredTraceMonitor: object %llu evicted while in filter (buffer_size=%d/%d)",
          (unsigned long long)evicted_obj->obj_id, ftm->size, ftm->max_size);
      ftm->evicted_count++;
    }
  }
}

static void ftm_finalize(eviction_analyzer_t *self, const char *output_dir) {
  filtered_trace_monitor_t *ftm = (filtered_trace_monitor_t *)self->data;

  LOG(INFO, STREAM_Cache,
      "FilteredTraceMonitor Summary: evicted_from_filter=%llu, "
      "tag0_entries=%llu, tag1_entries=%llu, tag2_entries=%llu, "
      "buffer_final_size=%d",
      (unsigned long long)ftm->evicted_count,
      (unsigned long long)ftm->tag0_count,
      (unsigned long long)ftm->tag1_count,
      (unsigned long long)ftm->tag2_count,
      ftm->size);
}

static void ftm_free(eviction_analyzer_t *self) {
  free(self->data);
  free(self);
}

/* ================================================================
 * Factory
 * ================================================================ */

eviction_analyzer_t *filtered_trace_monitor_create(const char *name,
                                                    const char *params) {
  /* Parse filter_size from params string (e.g., "32" or "filter_size=32") */
  int filter_size = 32;  /* Default: match trace generator filter size */
  
  if (params && strlen(params) > 0) {
    /* Try to parse as direct integer first */
    if (sscanf(params, "%d", &filter_size) != 1) {
      /* Try key=value format */
      if (sscanf(params, "filter_size=%d", &filter_size) != 1) {
        LOG(ERROR, STREAM_Cache,
            "FilteredTraceMonitor: invalid params '%s'", params);
        return NULL;
      }
    }
  }

  if (filter_size <= 0) {
    LOG(WARN, STREAM_Cache,
        "FilteredTraceMonitor: invalid filter_size=%d, using default 32",
        filter_size);
    filter_size = 32;
  }

  if (filter_size > FILTERED_TRACE_MONITOR_MAX_BUFFER) {
    LOG(WARN, STREAM_Cache,
        "FilteredTraceMonitor: filter_size=%d exceeds max %d, clamping",
        filter_size, FILTERED_TRACE_MONITOR_MAX_BUFFER);
    filter_size = FILTERED_TRACE_MONITOR_MAX_BUFFER;
  }

  filtered_trace_monitor_t *ftm = malloc(sizeof(filtered_trace_monitor_t));
  if (!ftm) {
    return NULL;
  }

  ftm->size = 0;
  ftm->max_size = filter_size;
  ftm->evicted_count = 0;

  eviction_analyzer_t *analyzer = malloc(sizeof(eviction_analyzer_t));
  if (!analyzer) {
    free(ftm);
    return NULL;
  }

  strncpy(analyzer->name, name, sizeof(analyzer->name) - 1);
  analyzer->name[sizeof(analyzer->name) - 1] = '\0';
  analyzer->data = ftm;
  analyzer->start = ftm_start;
  analyzer->process = ftm_process;
  analyzer->finalize = ftm_finalize;
  analyzer->free = ftm_free;

  return analyzer;
}
