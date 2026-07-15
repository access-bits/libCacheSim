#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/*
 * oracleGeneral compressed binary trace format - REVERSE chronological order
 *
 * This reader handles oracle files written in REVERSE chronological order.
 * The file contains time T-1, T-2, ..., 0 (last to first).
 * The reader reads BACKWARD to present time 0, 1, 2, ... to the simulator.
 *
 * NO HEADER in binary file - all metadata is in accompanying .meta file
 *
 * Binary file: Pure compressed batches (no headers, no size prefixes)
 * Metadata file (.meta): Contains batch information (batch_index, entry_count, byte_size)
 *
 * Entry format (after decompression, within each batch in reverse order):
 *   struct {
 *     uint32_t clock_time;
 *     uint64_t obj_id;
 *     uint32_t obj_size;
 *     int64_t next_access_vtime;
 *   };
 *
 * Reading strategy:
 *   1. Parse .meta file to get batch positions and sizes
 *   2. Read batches from last to first using positions from .meta
 *   3. Within each batch, read entries from last to first (backward)
 *   Result: Time flows forward (0, 1, 2, ...) for the simulator
 */

#include "../binaryUtils.h"
#include "libCacheSim/reader.h"
#include <zstd.h>
#include <pthread.h>

#define ORACLE_REVERSE_BUFFER_SIZE (24 * 1024 * 1024)  /* 24M entries per batch */

typedef struct {
  uint32_t clock_time;
  uint64_t obj_id;
  uint32_t obj_size;
  int64_t next_access_vtime;
} __attribute__((packed)) oracle_reverse_entry_t;

typedef struct {
  /* Ping-pong decompression buffers (double buffering) */
  oracle_reverse_entry_t *entry_buffer[2];  /* Two buffers for ping-pong */
  char *compressed_buf;                     /* Single compressed buffer (worker uses sequentially) */
  size_t buffer_capacity;
  size_t compressed_buf_capacity;
  
  /* Active buffer state */
  int active_buffer;       /* 0 or 1 - which buffer is currently being consumed */
  size_t buffer_size[2];   /* Number of entries in each buffer */
  ssize_t buffer_pos[2];   /* Current position in each buffer (backward iteration, -1 = exhausted) */
  
  /* Batch information from .meta file */
  long *batch_positions;   /* File positions of each batch */
  size_t *batch_sizes;     /* Compressed byte size of each batch */
  size_t *batch_entries;   /* Number of entries in each batch */
  size_t n_batches;        /* Total number of batches */
  ssize_t current_batch;   /* Current batch index (counting backward) */
  
  /* Background decompression thread */
  pthread_t decompress_thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool thread_running;
  bool thread_should_exit;
  
  /* Decompression request */
  int decompress_buffer_idx;   /* Which buffer to decompress into (-1 = none) */
  ssize_t decompress_batch_idx; /* Which batch to decompress */
  bool decompress_ready;        /* Is decompressed buffer ready? */
  bool decompress_error;        /* Did decompression fail? */
  
  /* File handle for thread */
  FILE *file_handle;
  
  /* File position tracking */
  bool eof;
} oracle_reverse_params_t;

/* Background decompression thread worker */
static void* oracleGeneralTraceReverse_decompress_worker(void *arg) {
  oracle_reverse_params_t *params = (oracle_reverse_params_t *)arg;
  
  while (true) {
    pthread_mutex_lock(&params->mutex);
    
    /* Wait for decompression request or exit signal */
    while (!params->thread_should_exit && params->decompress_buffer_idx < 0) {
      pthread_cond_wait(&params->cond, &params->mutex);
    }
    
    /* Check if we should exit */
    if (params->thread_should_exit) {
      pthread_mutex_unlock(&params->mutex);
      break;
    }
    
    /* Get decompression job */
    int buffer_idx = params->decompress_buffer_idx;
    ssize_t batch_idx = params->decompress_batch_idx;
    params->decompress_buffer_idx = -1;  /* Mark job as taken */
    
    pthread_mutex_unlock(&params->mutex);
    
    /* Perform decompression (without holding lock) */
    bool success = false;
    
    if (batch_idx >= 0 && batch_idx < (ssize_t)params->n_batches) {
      long batch_pos = params->batch_positions[batch_idx];
      size_t compressed_size = params->batch_sizes[batch_idx];
      size_t expected_entries = params->batch_entries[batch_idx];
      
      /* Seek and read (thread-safe as each thread uses separate file handle) */
      if (fseek(params->file_handle, batch_pos, SEEK_SET) == 0) {
        /* Ensure buffer is large enough (no lock needed - only worker thread accesses) */
        if (compressed_size > params->compressed_buf_capacity) {
          params->compressed_buf_capacity = compressed_size + 1024;
          char *new_buf = (char *)realloc(params->compressed_buf, 
                                          params->compressed_buf_capacity);
          if (!new_buf) {
            LOG(ERROR, STREAM_Reader,
                "oracleGeneralCompressedReverse: failed to realloc compressed buffer to %zu bytes\n",
                params->compressed_buf_capacity);
            pthread_mutex_lock(&params->mutex);
            params->decompress_ready = false;
            params->decompress_error = true;
            pthread_cond_signal(&params->cond);
            pthread_mutex_unlock(&params->mutex);
            return NULL;
          }
          params->compressed_buf = new_buf;
        }
        
        /* Read compressed data */
        if (fread(params->compressed_buf, 1, compressed_size, params->file_handle) 
            == compressed_size) {
          /* Decompress */
          size_t decompressed_size = ZSTD_decompress(
            params->entry_buffer[buffer_idx],
            params->buffer_capacity * sizeof(oracle_reverse_entry_t),
            params->compressed_buf,
            compressed_size);
          
          if (!ZSTD_isError(decompressed_size)) {
            pthread_mutex_lock(&params->mutex);
            params->buffer_size[buffer_idx] = decompressed_size / sizeof(oracle_reverse_entry_t);
            params->buffer_pos[buffer_idx] = (ssize_t)params->buffer_size[buffer_idx] - 1;
            
            /* Verify entry count */
            if (params->buffer_size[buffer_idx] != expected_entries) {
              LOG(ERROR, STREAM_Reader,
                  "oracleGeneralCompressedReverse: entry count mismatch in batch %ld: "
                  "expected %zu, got %zu\n",
                  batch_idx, expected_entries, params->buffer_size[buffer_idx]);
            }
            success = true;
            pthread_mutex_unlock(&params->mutex);
          } else {
            LOG(ERROR, STREAM_Reader,
                "oracleGeneralCompressedReverse: ZSTD decompression error for batch %ld: %s\n",
                batch_idx, ZSTD_getErrorName(decompressed_size));
          }
        }
      }
    }
    
    /* Signal completion */
    pthread_mutex_lock(&params->mutex);
    params->decompress_ready = success;
    params->decompress_error = !success;
    pthread_cond_signal(&params->cond);  /* Wake up waiting thread */
    pthread_mutex_unlock(&params->mutex);
  }
  
  return NULL;
}

/* Request decompression of a batch into specified buffer */
static inline void oracleGeneralTraceReverse_request_decompress(
    oracle_reverse_params_t *params, int buffer_idx, ssize_t batch_idx) {
  pthread_mutex_lock(&params->mutex);
  params->decompress_buffer_idx = buffer_idx;
  params->decompress_batch_idx = batch_idx;
  params->decompress_ready = false;
  params->decompress_error = false;
  pthread_cond_signal(&params->cond);  /* Wake up worker thread */
  pthread_mutex_unlock(&params->mutex);
}

/* Wait for decompression to complete and switch to the newly decompressed buffer */
static inline bool oracleGeneralTraceReverse_switch_buffer(
    oracle_reverse_params_t *params) {
  int next_buffer = 1 - params->active_buffer;
  
  /* Wait for decompression to complete */
  pthread_mutex_lock(&params->mutex);
  while (!params->decompress_ready && !params->decompress_error) {
    pthread_cond_wait(&params->cond, &params->mutex);
  }
  
  if (params->decompress_error) {
    pthread_mutex_unlock(&params->mutex);
    return false;  /* Decompression failed */
  }
  
  pthread_mutex_unlock(&params->mutex);
  
  /* Switch active buffer (buffer_pos already set by worker thread) */
  params->active_buffer = next_buffer;

  /* Log progress every 5% of batches */
  size_t batches_done = params->n_batches - 1 - (size_t)params->current_batch;
  if (params->n_batches > 0) {
    size_t pct = batches_done * 100 / params->n_batches;
    static size_t last_pct = -1UL;
    if (pct / 5 != last_pct / 5) {
      LOG(INFO, STREAM_Reader,
          "oracleGeneralCompressedReverse: %zu/%zu batches read (%zu%%)\n",
          batches_done, params->n_batches, pct);
      last_pct = pct;
    }
  }

  /* Prepare for next batch */
  params->current_batch--;
  if (params->current_batch >= 0) {
    /* Request decompression of next batch into the now-inactive buffer */
    int inactive_buffer = 1 - params->active_buffer;
    oracleGeneralTraceReverse_request_decompress(
        params, inactive_buffer, params->current_batch);
  } else {
    params->eof = true;
  }
  
  return true;
}

static inline int oracleGeneralTraceReverse_setup(reader_t *reader) {
  /* No header in binary file - read .meta file for batch information */
  
  reader->trace_type = ORACLE_GENERAL_REVERSE_TRACE;
  reader->trace_format = BINARY_TRACE_FORMAT;
  reader->item_size = sizeof(oracle_reverse_entry_t);
  reader->obj_id_is_num = true;
  reader->trace_start_offset = 0;  /* No header */

  /* Allocate reader-specific parameters */
  oracle_reverse_params_t *params = 
    (oracle_reverse_params_t *)malloc(sizeof(oracle_reverse_params_t));
  
  params->buffer_capacity = ORACLE_REVERSE_BUFFER_SIZE;
  
  /* Allocate ping-pong buffers */
  params->entry_buffer[0] = (oracle_reverse_entry_t *)malloc(
    sizeof(oracle_reverse_entry_t) * params->buffer_capacity);
  params->entry_buffer[1] = (oracle_reverse_entry_t *)malloc(
    sizeof(oracle_reverse_entry_t) * params->buffer_capacity);
  
  if (!params->entry_buffer[0] || !params->entry_buffer[1]) {
    LOG(ERROR, STREAM_Reader,
        "oracleGeneralCompressedReverse: failed to allocate entry buffers\n");
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params);
    return 1;
  }
  
  params->compressed_buf_capacity = ZSTD_compressBound(
    params->buffer_capacity * sizeof(oracle_reverse_entry_t)) + 1024;
  params->compressed_buf = (char *)malloc(params->compressed_buf_capacity);
  
  if (!params->compressed_buf) {
    LOG(ERROR, STREAM_Reader,
        "oracleGeneralCompressedReverse: failed to allocate compressed buffer\n");
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params);
    return 1;
  }
  
  /* Initialize ping-pong state */
  params->active_buffer = 0;
  params->buffer_size[0] = 0;
  params->buffer_size[1] = 0;
  params->buffer_pos[0] = -1;
  params->buffer_pos[1] = -1;
  params->eof = false;
  
  /* Initialize thread state */
  params->decompress_buffer_idx = -1;
  params->decompress_batch_idx = -1;
  params->decompress_ready = false;
  params->decompress_error = false;
  params->thread_should_exit = false;
  params->thread_running = false;
  pthread_mutex_init(&params->mutex, NULL);
  pthread_cond_init(&params->cond, NULL);
  
  /* Open a second file handle for the background decompression thread. */
  params->file_handle = fopen(reader->trace_path, "rb");
  if (!params->file_handle) {
    LOG(ERROR, STREAM_Reader,
        "oracleGeneralCompressedReverse: failed to open file handle for "
        "background thread: %s\n", reader->trace_path);
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params->compressed_buf);
    pthread_mutex_destroy(&params->mutex);
    pthread_cond_destroy(&params->cond);
    free(params);
    return 1;
  }
  
  /* Build .meta file path */
  char meta_path[4096];
  snprintf(meta_path, sizeof(meta_path), "%s.meta", reader->trace_path);
  
  FILE *meta_fp = fopen(meta_path, "r");
  if (!meta_fp) {
    LOG(ERROR, STREAM_Reader,
        "oracleGeneralCompressedReverse: failed to open .meta file: %s\n",
        meta_path);
    LOG(ERROR, STREAM_Reader,
        "oracleGeneralCompressedReverse: the .meta file is required\n");
    fclose(params->file_handle);
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params->compressed_buf);
    pthread_mutex_destroy(&params->mutex);
    pthread_cond_destroy(&params->cond);
    free(params);
    return 1;
  }
  
  /* Parse .meta file */
  LOG(INFO, STREAM_Reader,
      "oracleGeneralCompressedReverse: reading batch information from %s\n",
      meta_path);
  
  char line[512];
  bool is_compressed = false;
  bool is_reverse = false;
  
  /* Parse header comments */
  while (fgets(line, sizeof(line), meta_fp)) {
    if (strncmp(line, "# compressed:", 13) == 0) {
      int compressed_flag;
      sscanf(line + 13, "%d", &compressed_flag);
      is_compressed = (compressed_flag != 0);
    } else if (strncmp(line, "# reverse_order:", 16) == 0) {
      int reverse_flag;
      sscanf(line + 16, "%d", &reverse_flag);
      is_reverse = (reverse_flag != 0);
    } else if (strncmp(line, "batch_index,", 12) == 0) {
      /* CSV header - batch data starts next */
      break;
    }
  }
  
  if (!is_compressed) {
    LOG(ERROR, STREAM_Reader,
        "oracleGeneralCompressedReverse: trace must be compressed "
        "(check .meta file)\n");
    fclose(meta_fp);
    fclose(params->file_handle);
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params->compressed_buf);
    pthread_mutex_destroy(&params->mutex);
    pthread_cond_destroy(&params->cond);
    free(params);
    return 1;
  }
  
  if (!is_reverse) {
    LOG(ERROR, STREAM_Reader,
        "oracleGeneralCompressedReverse: trace must be in reverse order "
        "(check .meta file)\n");
    fclose(meta_fp);
    fclose(params->file_handle);
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params->compressed_buf);
    pthread_mutex_destroy(&params->mutex);
    pthread_cond_destroy(&params->cond);
    free(params);
    return 1;
  }
  
  /* Read batch information */
  size_t batch_capacity = 10000;
  params->batch_positions = (long *)malloc(sizeof(long) * batch_capacity);
  params->batch_sizes = (size_t *)malloc(sizeof(size_t) * batch_capacity);
  params->batch_entries = (size_t *)malloc(sizeof(size_t) * batch_capacity);
  
  if (!params->batch_positions || !params->batch_sizes || !params->batch_entries) {
    LOG(ERROR, STREAM_Reader,
        "oracleGeneralCompressedReverse: failed to allocate batch info arrays\n");
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
  
  long cumulative_position = 0;  /* No header in binary file */
  
  while (fgets(line, sizeof(line), meta_fp)) {
    if (params->n_batches >= batch_capacity) {
      batch_capacity *= 2;
      long *new_positions = (long *)realloc(params->batch_positions,
                                            sizeof(long) * batch_capacity);
      size_t *new_sizes = (size_t *)realloc(params->batch_sizes,
                                            sizeof(size_t) * batch_capacity);
      size_t *new_entries = (size_t *)realloc(params->batch_entries,
                                              sizeof(size_t) * batch_capacity);
      if (!new_positions || !new_sizes || !new_entries) {
        LOG(ERROR, STREAM_Reader,
            "oracleGeneralCompressedReverse: failed to grow batch info arrays\n");
        if (new_positions) params->batch_positions = new_positions;
        if (new_sizes) params->batch_sizes = new_sizes;
        if (new_entries) params->batch_entries = new_entries;
        break;
      }
      params->batch_positions = new_positions;
      params->batch_sizes = new_sizes;
      params->batch_entries = new_entries;
    }
    
    uint64_t batch_index;
    size_t entry_count, byte_size;
    
    if (sscanf(line, "%lu,%zu,%zu", &batch_index, &entry_count, &byte_size) == 3) {
      params->batch_positions[params->n_batches] = cumulative_position;
      params->batch_sizes[params->n_batches] = byte_size;
      params->batch_entries[params->n_batches] = entry_count;
      params->n_batches++;
      cumulative_position += byte_size;
    }
  }
  
  fclose(meta_fp);
  
  if (params->n_batches == 0) {
    LOG(ERROR, STREAM_Reader,
        "oracleGeneralCompressedReverse: no batch information found in .meta file\n");
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
  
  /* Sum entry counts across all batches */
  uint64_t total_entries = 0;
  for (size_t i = 0; i < params->n_batches; i++) total_entries += params->batch_entries[i];
  reader->n_total_req = (int64_t)total_entries;

  LOG(INFO, STREAM_Reader,
      "oracleGeneralCompressedReverse: loaded %zu batches, %lu total entries\n",
      params->n_batches, (unsigned long)total_entries);

  /* Start from the LAST batch (which contains earliest time entries) */
  params->current_batch = params->n_batches - 1;
  
  reader->reader_params = params;
  
  /* Start background decompression thread */
  if (pthread_create(&params->decompress_thread, NULL, 
                     oracleGeneralTraceReverse_decompress_worker, params) != 0) {
    LOG(ERROR, STREAM_Reader,
        "oracleGeneralCompressedReverse: failed to create background decompression thread\n");
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
      "oracleGeneralCompressedReverse: setup complete, %zu batches, "
      "entry_size=%zu, background decompression enabled\n",
      params->n_batches, sizeof(oracle_reverse_entry_t));
  
  return 0;
}

static inline void oracleGeneralTraceReverse_teardown(reader_t *reader) {
  if (reader->reader_params != NULL) {
    oracle_reverse_params_t *params = 
      (oracle_reverse_params_t *)reader->reader_params;
    
    /* Stop background thread */
    if (params->thread_running) {
      pthread_mutex_lock(&params->mutex);
      params->thread_should_exit = true;
      pthread_cond_signal(&params->cond);
      pthread_mutex_unlock(&params->mutex);
      
      pthread_join(params->decompress_thread, NULL);
    }
    
    /* Cleanup resources */
    if (params->file_handle) {
      fclose(params->file_handle);
    }
    
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
}

static inline int oracleGeneralTraceReverse_read_one_req(reader_t *reader,
                                                              request_t *req) {
  oracle_reverse_params_t *params = 
    (oracle_reverse_params_t *)reader->reader_params;
  
  int active = params->active_buffer;
  
  /* Check if we need to load/switch to next batch */
  if (params->buffer_pos[active] < 0) {
    /* Current buffer exhausted */
    if (params->eof) {
      req->valid = false;
      return 1;
    }
    
    /* Check if this is the first load */
    if (params->buffer_size[0] == 0 && params->buffer_size[1] == 0) {
      /* First load: synchronously decompress first batch into active buffer */
      if (params->current_batch < 0 ||
          params->current_batch >= (ssize_t)params->n_batches) {
        params->eof = true;
        req->valid = false;
        return 1;
      }
      
      /* Request decompression and wait for it */
      oracleGeneralTraceReverse_request_decompress(
          params, active, params->current_batch);
      
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
      
      /* buffer_pos already set by worker thread to end of buffer */
      
      /* Start decompressing next batch in background */
      params->current_batch--;
      if (params->current_batch >= 0) {
        int next_buffer = 1 - active;
        oracleGeneralTraceReverse_request_decompress(
            params, next_buffer, params->current_batch);
      } else {
        params->eof = true;
      }
    } else {
      /* Subsequent loads: switch to already-decompressed buffer */
      if (!oracleGeneralTraceReverse_switch_buffer(params)) {
        req->valid = false;
        return 1;
      }
      active = params->active_buffer;  /* Update after switch */
    }
  }
  
  /* Get current entry from active buffer (reading backward) */
  oracle_reverse_entry_t *entry =
      &params->entry_buffer[active][params->buffer_pos[active]];
  params->buffer_pos[active]--;

  /* Fill request structure */
  req->clock_time = entry->clock_time;
  req->obj_id = entry->obj_id;
  req->obj_size = entry->obj_size;
  req->next_access_vtime = entry->next_access_vtime;
  
  if (req->next_access_vtime == -1 || req->next_access_vtime == INT64_MAX) {
    req->next_access_vtime = MAX_REUSE_DISTANCE;
  }

  req->valid = true;
  
  return 0;
}

#ifdef __cplusplus
}
#endif
