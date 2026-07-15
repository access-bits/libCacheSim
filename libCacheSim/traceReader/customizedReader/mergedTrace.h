#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/*
 * mergedTrace reader — forward chronological, zstd-compressed batches
 *
 * Reads the output of merge_traces.c:
 *   Binary file: sequential zstd-compressed batches of merged_trace_entry_t
 *   Meta file (.meta): comment headers + CSV with batch_index,entry_count,byte_size
 *
 * Entry format (9 bytes packed):
 *   uint64_t vaddr;   // virtual address or page number
 *   uint8_t  cpu;     // CPU that issued this access
 *
 * Maps to request_t:
 *   obj_id  = vaddr (or vaddr >> 12 if address_mode is "virtual_addresses")
 *   features[0] (cpu_id) = cpu
 *   obj_size = 1
 *
 * Architecture:
 *   - Ping-pong double buffers with background decompression thread
 *   - Read forward: batch 0, 1, 2, ... and within each batch entry 0, 1, 2, ...
 *   - While main thread consumes active buffer, worker decompresses next batch
 *     into inactive buffer
 */

#include "binaryUtils.h"
#include "libCacheSim/reader.h"
#include <zstd.h>
#include <pthread.h>

#define MERGED_TRACE_BUFFER_SIZE (24 * 1024 * 1024) /* 24M entries per buffer */
#define MERGED_TRACE_CPU_FEATURE_IDX 0

static inline void mergedTrace_set_cpu_feature(request_t *req, uint8_t cpu_id) {
  req->features[MERGED_TRACE_CPU_FEATURE_IDX] = (int32_t)cpu_id;
  if (req->n_features <= MERGED_TRACE_CPU_FEATURE_IDX) {
    req->n_features = MERGED_TRACE_CPU_FEATURE_IDX + 1;
  }
}

typedef struct {
  uint64_t vaddr;
  uint8_t cpu;
} __attribute__((packed)) merged_trace_entry_t;

typedef struct {
  /* Ping-pong decompression buffers */
  merged_trace_entry_t *entry_buffer[2];
  char *compressed_buf;
  size_t buffer_capacity;
  size_t compressed_buf_capacity;

  /* Active buffer state */
  int active_buffer;       /* 0 or 1 */
  size_t buffer_size[2];   /* Number of entries in each buffer */
  size_t buffer_pos[2];    /* Current read position (forward) */

  /* Batch information from .meta file */
  long *batch_positions;   /* File offset of each batch */
  size_t *batch_sizes;     /* Compressed byte size of each batch */
  size_t *batch_entries;   /* Number of entries in each batch */
  size_t n_batches;
  size_t next_batch;       /* Next batch index to decompress (forward) */

  /* Background decompression thread */
  pthread_t decompress_thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool thread_running;
  bool thread_should_exit;

  /* Decompression request */
  int decompress_buffer_idx;    /* Which buffer to decompress into (-1 = none) */
  ssize_t decompress_batch_idx; /* Which batch to decompress */
  bool decompress_ready;
  bool decompress_error;

  FILE *file_handle;
  bool eof;
  bool convert_to_pages; /* true if address_mode is "virtual_addresses" */
} merged_trace_params_t;

/* ------------------------------------------------------------------ */
/*  Background decompression thread                                    */
/* ------------------------------------------------------------------ */

static void *mergedTrace_decompress_worker(void *arg) {
  merged_trace_params_t *params = (merged_trace_params_t *)arg;

  while (true) {
    pthread_mutex_lock(&params->mutex);

    /* Wait for a decompression request or exit signal */
    while (!params->thread_should_exit && params->decompress_buffer_idx < 0) {
      pthread_cond_wait(&params->cond, &params->mutex);
    }

    if (params->thread_should_exit) {
      pthread_mutex_unlock(&params->mutex);
      break;
    }

    /* Take the job */
    int buffer_idx = params->decompress_buffer_idx;
    ssize_t batch_idx = params->decompress_batch_idx;
    params->decompress_buffer_idx = -1;

    pthread_mutex_unlock(&params->mutex);

    /* Decompress outside the lock */
    bool success = false;

    if (batch_idx >= 0 && batch_idx < (ssize_t)params->n_batches) {
      long batch_pos = params->batch_positions[batch_idx];
      size_t compressed_size = params->batch_sizes[batch_idx];
      size_t expected_entries = params->batch_entries[batch_idx];

      if (fseek(params->file_handle, batch_pos, SEEK_SET) == 0) {
        /* Grow compressed buffer if needed */
        if (compressed_size > params->compressed_buf_capacity) {
          params->compressed_buf_capacity = compressed_size + 1024;
          char *new_buf = (char *)realloc(params->compressed_buf,
                                          params->compressed_buf_capacity);
          if (!new_buf) {
            LOG(ERROR, STREAM_Reader,
                "mergedTrace: failed to realloc compressed buffer to %zu\n",
                params->compressed_buf_capacity);
            goto done;
          }
          params->compressed_buf = new_buf;
        }

        if (fread(params->compressed_buf, 1, compressed_size,
                  params->file_handle) == compressed_size) {
          /* Grow entry buffer if needed */
          if (expected_entries > params->buffer_capacity) {
            merged_trace_entry_t *new_buf =
                (merged_trace_entry_t *)realloc(
                    params->entry_buffer[buffer_idx],
                    expected_entries * sizeof(merged_trace_entry_t));
            if (!new_buf) {
              LOG(ERROR, STREAM_Reader,
                  "mergedTrace: failed to realloc entry buffer\n");
              goto done;
            }
            params->entry_buffer[buffer_idx] = new_buf;
            /* Only update capacity for the buffer we just grew;
             * the other buffer keeps its own capacity. */
          }

          size_t dec = ZSTD_decompress(
              params->entry_buffer[buffer_idx],
              (expected_entries > params->buffer_capacity
                   ? expected_entries
                   : params->buffer_capacity) *
                  sizeof(merged_trace_entry_t),
              params->compressed_buf, compressed_size);

          if (!ZSTD_isError(dec)) {
            size_t got_entries = dec / sizeof(merged_trace_entry_t);
            if (got_entries != expected_entries) {
              LOG(WARN, STREAM_Reader,
                  "mergedTrace: batch %zd entry count mismatch: "
                  "expected %zu, got %zu\n",
                  batch_idx, expected_entries, got_entries);
            }

            pthread_mutex_lock(&params->mutex);
            params->buffer_size[buffer_idx] = got_entries;
            params->buffer_pos[buffer_idx] = 0;
            success = true;
            pthread_mutex_unlock(&params->mutex);
          } else {
            LOG(ERROR, STREAM_Reader,
                "mergedTrace: zstd error batch %zd: %s\n", batch_idx,
                ZSTD_getErrorName(dec));
          }
        }
      }
    }

  done:
    pthread_mutex_lock(&params->mutex);
    params->decompress_ready = success;
    params->decompress_error = !success;
    pthread_cond_signal(&params->cond);
    pthread_mutex_unlock(&params->mutex);
  }

  return NULL;
}

/* ------------------------------------------------------------------ */
/*  Helper: request decompression                                      */
/* ------------------------------------------------------------------ */

static inline void mergedTrace_request_decompress(merged_trace_params_t *params,
                                                  int buffer_idx,
                                                  ssize_t batch_idx) {
  pthread_mutex_lock(&params->mutex);
  params->decompress_buffer_idx = buffer_idx;
  params->decompress_batch_idx = batch_idx;
  params->decompress_ready = false;
  params->decompress_error = false;
  pthread_cond_signal(&params->cond);
  pthread_mutex_unlock(&params->mutex);
}

/* Wait for decompression, switch to newly decompressed buffer,
 * and kick off prefetch of the next batch. */
static inline bool mergedTrace_switch_buffer(merged_trace_params_t *params) {
  int next_buffer = 1 - params->active_buffer;

  pthread_mutex_lock(&params->mutex);
  while (!params->decompress_ready && !params->decompress_error) {
    pthread_cond_wait(&params->cond, &params->mutex);
  }
  if (params->decompress_error) {
    pthread_mutex_unlock(&params->mutex);
    return false;
  }
  pthread_mutex_unlock(&params->mutex);

  params->active_buffer = next_buffer;

  /* Log progress */
  if (params->n_batches > 0) {
    size_t pct = params->next_batch * 100 / params->n_batches;
    static size_t last_pct = (size_t)-1;
    if (pct / 5 != last_pct / 5) {
      LOG(INFO, STREAM_Reader,
          "mergedTrace: %zu/%zu batches read (%zu%%)\n",
          params->next_batch, params->n_batches, pct);
      last_pct = pct;
    }
  }

  /* Prefetch next batch into the now-inactive buffer */
  if (params->next_batch < params->n_batches) {
    int inactive = 1 - params->active_buffer;
    mergedTrace_request_decompress(params, inactive,
                                   (ssize_t)params->next_batch);
    params->next_batch++;
  } else {
    params->eof = true;
  }

  return true;
}

/* ------------------------------------------------------------------ */
/*  Setup                                                              */
/* ------------------------------------------------------------------ */

static inline int mergedTrace_setup(reader_t *reader) {
  reader->trace_type = MERGED_TRACE;
  reader->trace_format = BINARY_TRACE_FORMAT;
  reader->item_size = sizeof(merged_trace_entry_t);
  reader->obj_id_is_num = true;
  reader->trace_start_offset = 0;

  merged_trace_params_t *params =
      (merged_trace_params_t *)calloc(1, sizeof(merged_trace_params_t));

  params->buffer_capacity = MERGED_TRACE_BUFFER_SIZE;

  params->entry_buffer[0] = (merged_trace_entry_t *)malloc(
      sizeof(merged_trace_entry_t) * params->buffer_capacity);
  params->entry_buffer[1] = (merged_trace_entry_t *)malloc(
      sizeof(merged_trace_entry_t) * params->buffer_capacity);
  if (!params->entry_buffer[0] || !params->entry_buffer[1]) {
    LOG(ERROR, STREAM_Reader, "mergedTrace: failed to allocate entry buffers\n");
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params);
    return 1;
  }

  params->compressed_buf_capacity =
      ZSTD_compressBound(params->buffer_capacity *
                         sizeof(merged_trace_entry_t)) +
      1024;
  params->compressed_buf = (char *)malloc(params->compressed_buf_capacity);
  if (!params->compressed_buf) {
    LOG(ERROR, STREAM_Reader,
        "mergedTrace: failed to allocate compressed buffer\n");
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params);
    return 1;
  }

  /* Initial state */
  params->active_buffer = 0;
  params->buffer_size[0] = 0;
  params->buffer_size[1] = 0;
  params->buffer_pos[0] = 0;
  params->buffer_pos[1] = 0;
  params->eof = false;
  params->convert_to_pages = false;

  params->decompress_buffer_idx = -1;
  params->decompress_batch_idx = -1;
  params->decompress_ready = false;
  params->decompress_error = false;
  params->thread_should_exit = false;
  params->thread_running = false;
  pthread_mutex_init(&params->mutex, NULL);
  pthread_cond_init(&params->cond, NULL);

  /* Open second file handle for background thread */
  params->file_handle = fopen(reader->trace_path, "rb");
  if (!params->file_handle) {
    LOG(ERROR, STREAM_Reader,
        "mergedTrace: failed to open %s for bg thread\n",
        reader->trace_path);
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params->compressed_buf);
    pthread_mutex_destroy(&params->mutex);
    pthread_cond_destroy(&params->cond);
    free(params);
    return 1;
  }

  /* Parse .meta file */
  char meta_path[4096];
  snprintf(meta_path, sizeof(meta_path), "%s.meta", reader->trace_path);

  FILE *meta_fp = fopen(meta_path, "r");
  if (!meta_fp) {
    LOG(ERROR, STREAM_Reader,
        "mergedTrace: failed to open .meta file: %s\n", meta_path);
    fclose(params->file_handle);
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params->compressed_buf);
    pthread_mutex_destroy(&params->mutex);
    pthread_cond_destroy(&params->cond);
    free(params);
    return 1;
  }

  LOG(INFO, STREAM_Reader,
      "mergedTrace: reading metadata from %s\n", meta_path);

  char line[512];

  /* Parse comment headers */
  while (fgets(line, sizeof(line), meta_fp)) {
    if (strncmp(line, "# address_mode:", 15) == 0) {
      if (strstr(line + 15, "virtual_addresses") != NULL) {
        params->convert_to_pages = true;
        LOG(INFO, STREAM_Reader,
            "mergedTrace: address_mode=virtual_addresses, "
            "will convert to pages (>> 12)\n");
      } else {
        LOG(INFO, STREAM_Reader, "mergedTrace: address_mode=pages\n");
      }
    } else if (strncmp(line, "batch_index,", 12) == 0) {
      /* CSV header — batch data follows */
      break;
    }
    /* Skip other comment lines */
  }

  /* Read batch entries */
  size_t batch_capacity = 10000;
  params->batch_positions =
      (long *)malloc(sizeof(long) * batch_capacity);
  params->batch_sizes =
      (size_t *)malloc(sizeof(size_t) * batch_capacity);
  params->batch_entries =
      (size_t *)malloc(sizeof(size_t) * batch_capacity);

  if (!params->batch_positions || !params->batch_sizes ||
      !params->batch_entries) {
    LOG(ERROR, STREAM_Reader,
        "mergedTrace: failed to allocate batch info arrays\n");
    fclose(meta_fp);
    fclose(params->file_handle);
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params->compressed_buf);
    free(params->batch_positions);
    free(params->batch_sizes);
    free(params->batch_entries);
    pthread_mutex_destroy(&params->mutex);
    pthread_cond_destroy(&params->cond);
    free(params);
    return 1;
  }

  params->n_batches = 0;
  long cumulative_pos = 0;

  while (fgets(line, sizeof(line), meta_fp)) {
    if (params->n_batches >= batch_capacity) {
      batch_capacity *= 2;
      params->batch_positions = (long *)realloc(
          params->batch_positions, sizeof(long) * batch_capacity);
      params->batch_sizes = (size_t *)realloc(
          params->batch_sizes, sizeof(size_t) * batch_capacity);
      params->batch_entries = (size_t *)realloc(
          params->batch_entries, sizeof(size_t) * batch_capacity);
      if (!params->batch_positions || !params->batch_sizes ||
          !params->batch_entries) {
        LOG(ERROR, STREAM_Reader, "mergedTrace: failed to grow batch arrays\n");
        break;
      }
    }

    uint64_t batch_index;
    size_t entry_count, byte_size;
    if (sscanf(line, "%lu,%zu,%zu", &batch_index, &entry_count, &byte_size) ==
        3) {
      params->batch_positions[params->n_batches] = cumulative_pos;
      params->batch_sizes[params->n_batches] = byte_size;
      params->batch_entries[params->n_batches] = entry_count;
      params->n_batches++;
      cumulative_pos += byte_size;
    }
  }

  fclose(meta_fp);

  if (params->n_batches == 0) {
    LOG(ERROR, STREAM_Reader,
        "mergedTrace: no batch information in .meta file\n");
    fclose(params->file_handle);
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params->compressed_buf);
    free(params->batch_positions);
    free(params->batch_sizes);
    free(params->batch_entries);
    pthread_mutex_destroy(&params->mutex);
    pthread_cond_destroy(&params->cond);
    free(params);
    return 1;
  }

  /* Total entries for n_total_req */
  uint64_t total_entries = 0;
  for (size_t i = 0; i < params->n_batches; i++)
    total_entries += params->batch_entries[i];
  reader->n_total_req = (int64_t)total_entries;

  LOG(INFO, STREAM_Reader,
      "mergedTrace: %zu batches, %lu total entries\n", params->n_batches,
      (unsigned long)total_entries);

  /* Start from batch 0 (forward) */
  params->next_batch = 0;

  reader->reader_params = params;

  /* Start background decompression thread */
  if (pthread_create(&params->decompress_thread, NULL,
                     mergedTrace_decompress_worker, params) != 0) {
    LOG(ERROR, STREAM_Reader,
        "mergedTrace: failed to create decompression thread\n");
    fclose(params->file_handle);
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params->compressed_buf);
    free(params->batch_positions);
    free(params->batch_sizes);
    free(params->batch_entries);
    pthread_mutex_destroy(&params->mutex);
    pthread_cond_destroy(&params->cond);
    free(params);
    return 1;
  }
  params->thread_running = true;

  LOG(INFO, STREAM_Reader,
      "mergedTrace: setup complete, background decompression enabled\n");

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Teardown                                                           */
/* ------------------------------------------------------------------ */

static inline void mergedTrace_teardown(reader_t *reader) {
  if (reader->reader_params == NULL) return;

  merged_trace_params_t *params =
      (merged_trace_params_t *)reader->reader_params;

  if (params->thread_running) {
    pthread_mutex_lock(&params->mutex);
    params->thread_should_exit = true;
    pthread_cond_signal(&params->cond);
    pthread_mutex_unlock(&params->mutex);
    pthread_join(params->decompress_thread, NULL);
  }

  if (params->file_handle) fclose(params->file_handle);

  free(params->entry_buffer[0]);
  free(params->entry_buffer[1]);
  free(params->compressed_buf);
  free(params->batch_positions);
  free(params->batch_sizes);
  free(params->batch_entries);

  pthread_mutex_destroy(&params->mutex);
  pthread_cond_destroy(&params->cond);

  free(params);
  reader->reader_params = NULL;
}

/* ------------------------------------------------------------------ */
/*  Read one request                                                   */
/* ------------------------------------------------------------------ */

static inline int mergedTrace_read_one_req(reader_t *reader, request_t *req) {
  merged_trace_params_t *params =
      (merged_trace_params_t *)reader->reader_params;

  int active = params->active_buffer;

  /* Check if current buffer is exhausted */
  if (params->buffer_pos[active] >= params->buffer_size[active]) {
    /* First load (both buffers empty) */
    if (params->buffer_size[0] == 0 && params->buffer_size[1] == 0) {
      if (params->next_batch >= params->n_batches) {
        params->eof = true;
        req->valid = false;
        return 1;
      }

      /* Synchronously decompress first batch */
      mergedTrace_request_decompress(params, active,
                                     (ssize_t)params->next_batch);
      params->next_batch++;

      pthread_mutex_lock(&params->mutex);
      while (!params->decompress_ready && !params->decompress_error) {
        pthread_cond_wait(&params->cond, &params->mutex);
      }
      if (params->decompress_error) {
        pthread_mutex_unlock(&params->mutex);
        params->eof = true;
        req->valid = false;
        return 1;
      }
      pthread_mutex_unlock(&params->mutex);

      /* Kick off prefetch of next batch into inactive buffer */
      if (params->next_batch < params->n_batches) {
        int inactive = 1 - active;
        mergedTrace_request_decompress(params, inactive,
                                       (ssize_t)params->next_batch);
        params->next_batch++;
      } else {
        params->eof = true;
      }
    } else if (params->eof) {
      req->valid = false;
      return 1;
    } else {
      /* Switch to the already-decompressing buffer */
      if (!mergedTrace_switch_buffer(params)) {
        req->valid = false;
        return 1;
      }
      active = params->active_buffer;
    }
  }

  /* Read entry from active buffer (forward) */
  merged_trace_entry_t *entry =
      &params->entry_buffer[active][params->buffer_pos[active]];
  params->buffer_pos[active]++;

  /* Map to request_t */
  uint64_t addr = entry->vaddr;
  if (params->convert_to_pages) {
    addr = addr >> 12;
  }

  req->clock_time = (int64_t)reader->n_read_req;
  req->obj_id = (obj_id_t)addr;
  req->obj_size = 1;
  req->next_access_vtime = -2;
  mergedTrace_set_cpu_feature(req, entry->cpu);
  req->valid = true;

  return 0;
}

#ifdef __cplusplus
}
#endif
