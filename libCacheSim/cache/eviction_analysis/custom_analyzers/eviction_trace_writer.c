/*
 * eviction_trace_writer.c — Eviction analyzer that logs evicted objects to CSV
 *
 * Fast path: every eviction just appends (clock_time, obj_id) to a
 * pre-allocated in-memory buffer. The buffer is flushed to disk as a single
 * uncompressed CSV file in one bulk write, either when it fills up or when
 * the cache is freed.
 */

#include "eviction_trace_writer.h"
#include "libCacheSim/cache.h"
#include "libCacheSim/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVICTION_TRACE_WRITER_CAPACITY (2 * 1024 * 1024) /* 2M evictions */

typedef struct {
  int64_t clock_times[EVICTION_TRACE_WRITER_CAPACITY]; /* buffered clock_time per eviction */
  uint64_t obj_ids[EVICTION_TRACE_WRITER_CAPACITY];    /* buffered obj_id per eviction */
  size_t count;            /* number of evictions currently buffered */
  uint64_t flush_count;    /* number of times the buffer has been flushed */
  uint64_t total_evicted;  /* total evictions seen across all flushes */
  char filename[256];      /* output CSV path, used as-is (see params) */
  char analyzer_name[128];
} eviction_trace_writer_t;

/* Write the whole buffer out as CSV in one bulk write, then reset it. */
static void etw_flush(eviction_trace_writer_t *etw) {
  if (etw->count == 0) {
    return;
  }

  bool append = etw->flush_count > 0;
  FILE *fp = fopen(etw->filename, append ? "a" : "w");
  if (!fp) {
    LOG(ERROR, STREAM_Cache, "EvictionTraceWriter: failed to open '%s' for writing",
        etw->filename);
    return;
  }

  if (!append) {
    fputs("clock_time,obj_id\n", fp);
  }
  for (size_t i = 0; i < etw->count; i++) {
    fprintf(fp, "%lld,%llu\n", (long long)etw->clock_times[i],
            (unsigned long long)etw->obj_ids[i]);
  }

  fclose(fp);

  LOG(INFO, STREAM_Cache,
      "EvictionTraceWriter '%s' flushed %zu evictions to '%s' (flush #%llu)",
      etw->analyzer_name, etw->count, etw->filename,
      (unsigned long long)etw->flush_count + 1);

  etw->total_evicted += etw->count;
  etw->count = 0;
  etw->flush_count++;
}

static void etw_start(eviction_analyzer_t *self) {
  eviction_trace_writer_t *etw = (eviction_trace_writer_t *)self->data;
  etw->count = 0;
  etw->flush_count = 0;
  etw->total_evicted = 0;
  LOG(INFO, STREAM_Cache,
      "EvictionTraceWriter '%s' started (capacity=%d evictions, file=%s)",
      etw->analyzer_name, EVICTION_TRACE_WRITER_CAPACITY, etw->filename);
}

static void etw_process(eviction_analyzer_t *self, obj_id_t evicted_id,
                         request_t *req) {
  eviction_trace_writer_t *etw = (eviction_trace_writer_t *)self->data;

  if (evicted_id == OBJ_ID_NONE) {
    return;
  }

  if (etw->count >= EVICTION_TRACE_WRITER_CAPACITY) {
    etw_flush(etw);
  }

  etw->clock_times[etw->count] = req ? req->clock_time : -1;
  etw->obj_ids[etw->count] = evicted_id;
  etw->count++;
}

static void etw_finalize(eviction_analyzer_t *self, const char *output_dir) {
  eviction_trace_writer_t *etw = (eviction_trace_writer_t *)self->data;
  (void)output_dir; /* filename is used as-is, caller controls full path via params */
  etw_flush(etw);

  LOG(INFO, STREAM_Cache,
      "EvictionTraceWriter '%s' finished: total_evicted=%llu, flush_count=%llu",
      etw->analyzer_name, (unsigned long long)etw->total_evicted,
      (unsigned long long)etw->flush_count);
}

static void etw_free(eviction_analyzer_t *self) {
  free(self->data);
  free(self);
}

/* params, if given, is just the output filename */
static void parse_params(const char *params, char *filename,
                          size_t filename_size) {
  if (params && strlen(params) > 0) {
    strncpy(filename, params, filename_size - 1);
    filename[filename_size - 1] = '\0';
  }
}

eviction_analyzer_t *eviction_trace_writer_create(const char *name,
                                                   const char *params) {
  char filename[256];
  snprintf(filename, sizeof(filename), "%s_evictions.csv",
           (name && name[0]) ? name : "eviction_trace");
  parse_params(params, filename, sizeof(filename));

  eviction_trace_writer_t *etw = malloc(sizeof(eviction_trace_writer_t));
  if (!etw) {
    LOG(ERROR, STREAM_Cache,
        "EvictionTraceWriter: failed to allocate %d-entry buffer",
        EVICTION_TRACE_WRITER_CAPACITY);
    return NULL;
  }

  etw->count = 0;
  etw->flush_count = 0;
  etw->total_evicted = 0;
  strncpy(etw->filename, filename, sizeof(etw->filename) - 1);
  etw->filename[sizeof(etw->filename) - 1] = '\0';
  strncpy(etw->analyzer_name, name ? name : "eviction_trace_writer",
          sizeof(etw->analyzer_name) - 1);
  etw->analyzer_name[sizeof(etw->analyzer_name) - 1] = '\0';

  eviction_analyzer_t *analyzer = malloc(sizeof(eviction_analyzer_t));
  if (!analyzer) {
    free(etw);
    return NULL;
  }

  strncpy(analyzer->name, name, sizeof(analyzer->name) - 1);
  analyzer->name[sizeof(analyzer->name) - 1] = '\0';
  analyzer->data = etw;
  analyzer->start = etw_start;
  analyzer->process = etw_process;
  analyzer->finalize = etw_finalize;
  analyzer->free = etw_free;

  return analyzer;
}
