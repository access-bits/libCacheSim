#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Oracle LRU TLB compressed binary trace format - REVERSE chronological order
 * WITH TLB simulation fields passed to the algorithm.
 *
 * This is identical to oracleLruTlbCompressedReverse.h except that it also
 * passes tlb_miss and cpu fields from the trace entry to req->tlb_miss and
 * req->cpu_id, enabling the cache algorithm to perform TLB simulation.
 *
 * Entry format (after decompression, within each batch in reverse order):
 *   struct {
 *     uint64_t vaddr;
 *     uint8_t tlb_miss;
 *     uint8_t cpu;
 *     int64_t page_next_access_time;
 *   };
 */

#include "../binaryUtils.h"
#include "libCacheSim/reader.h"
#include <zstd.h>
#include <pthread.h>

#define ORACLE_LRU_TLB_REVERSE_TLB_SIM_BUFFER_SIZE (24 * 1024 * 1024)  /* 24M entries per batch */

typedef struct {
  uint64_t vaddr;
  uint8_t tlb_miss;
  uint8_t cpu;
  int64_t page_next_access_time;
} __attribute__((packed)) oracle_lru_tlb_reverse_tlb_sim_entry_t;

typedef struct {
  /* Ping-pong decompression buffers (double buffering) */
  oracle_lru_tlb_reverse_tlb_sim_entry_t *entry_buffer[2];
  char *compressed_buf;
  size_t buffer_capacity;
  size_t compressed_buf_capacity;
  
  /* Active buffer state */
  int active_buffer;
  size_t buffer_size[2];
  ssize_t buffer_pos[2];
  
  /* Batch information from .meta file */
  long *batch_positions;
  size_t *batch_sizes;
  size_t *batch_entries;
  size_t n_batches;
  ssize_t current_batch;
  
  /* Background decompression thread */
  pthread_t decompress_thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool thread_running;
  bool thread_should_exit;
  
  /* Decompression request */
  int decompress_buffer_idx;
  ssize_t decompress_batch_idx;
  bool decompress_ready;
  bool decompress_error;
  
  /* File handle for thread */
  FILE *file_handle;
  
  /* File position tracking */
  bool eof;
} oracle_lru_tlb_reverse_tlb_sim_params_t;

/* Background decompression thread worker */
static void* oracleLruTlbCompressedReverseTlbSim_decompress_worker(void *arg) {
  oracle_lru_tlb_reverse_tlb_sim_params_t *params = (oracle_lru_tlb_reverse_tlb_sim_params_t *)arg;
  
  while (true) {
    pthread_mutex_lock(&params->mutex);
    
    while (!params->thread_should_exit && params->decompress_buffer_idx < 0) {
      pthread_cond_wait(&params->cond, &params->mutex);
    }
    
    if (params->thread_should_exit) {
      pthread_mutex_unlock(&params->mutex);
      break;
    }
    
    int buffer_idx = params->decompress_buffer_idx;
    ssize_t batch_idx = params->decompress_batch_idx;
    params->decompress_buffer_idx = -1;
    
    pthread_mutex_unlock(&params->mutex);
    
    bool success = false;
    
    if (batch_idx >= 0 && batch_idx < (ssize_t)params->n_batches) {
      long batch_pos = params->batch_positions[batch_idx];
      size_t compressed_size = params->batch_sizes[batch_idx];
      size_t expected_entries = params->batch_entries[batch_idx];
      
      if (fseek(params->file_handle, batch_pos, SEEK_SET) == 0) {
        if (compressed_size > params->compressed_buf_capacity) {
          params->compressed_buf_capacity = compressed_size + 1024;
          char *new_buf = (char *)realloc(params->compressed_buf, 
                                          params->compressed_buf_capacity);
          if (!new_buf) {
            ERROR("Failed to reallocate compressed buffer to %zu bytes\n", 
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
        
        if (fread(params->compressed_buf, 1, compressed_size, params->file_handle) 
            == compressed_size) {
          size_t decompressed_size = ZSTD_decompress(
            params->entry_buffer[buffer_idx],
            params->buffer_capacity * sizeof(oracle_lru_tlb_reverse_tlb_sim_entry_t),
            params->compressed_buf,
            compressed_size);
          
          if (!ZSTD_isError(decompressed_size)) {
            pthread_mutex_lock(&params->mutex);
            params->buffer_size[buffer_idx] = decompressed_size / sizeof(oracle_lru_tlb_reverse_tlb_sim_entry_t);
            params->buffer_pos[buffer_idx] = (ssize_t)params->buffer_size[buffer_idx] - 1;
            
            if (params->buffer_size[buffer_idx] != expected_entries) {
              ERROR("Entry count mismatch in batch %ld: expected %zu, got %zu\n",
                    batch_idx, expected_entries, params->buffer_size[buffer_idx]);
            }
            success = true;
            pthread_mutex_unlock(&params->mutex);
          }
          else {
            ERROR("ZSTD decompression error for batch %ld: %s\n", 
                  batch_idx, ZSTD_getErrorName(decompressed_size));
          }
        }
      }
    }
    
    pthread_mutex_lock(&params->mutex);
    params->decompress_ready = success;
    params->decompress_error = !success;
    pthread_cond_signal(&params->cond);
    pthread_mutex_unlock(&params->mutex);
  }
  
  return NULL;
}

static inline void request_decompress_lru_tlb_sim(oracle_lru_tlb_reverse_tlb_sim_params_t *params, 
                                                    int buffer_idx, ssize_t batch_idx) {
  pthread_mutex_lock(&params->mutex);
  params->decompress_buffer_idx = buffer_idx;
  params->decompress_batch_idx = batch_idx;
  params->decompress_ready = false;
  params->decompress_error = false;
  pthread_cond_signal(&params->cond);
  pthread_mutex_unlock(&params->mutex);
}

static inline bool switch_buffer_lru_tlb_sim(oracle_lru_tlb_reverse_tlb_sim_params_t *params) {
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

  size_t batches_done = params->n_batches - 1 - (size_t)params->current_batch;
  if (params->n_batches > 0) {
    size_t pct = batches_done * 100 / params->n_batches;
    static size_t last_pct = -1UL;
    if (pct / 5 != last_pct / 5) {
      INFO("oracleLruTlbCompressedReverseTlbSim: %zu/%zu batches read (%zu%%)\n",
           batches_done, params->n_batches, pct);
      last_pct = pct;
    }
  }

  params->current_batch--;
  if (params->current_batch >= 0) {
    int inactive_buffer = 1 - params->active_buffer;
    request_decompress_lru_tlb_sim(params, inactive_buffer, params->current_batch);
  } else {
    params->eof = true;
  }
  
  return true;
}

static inline int oracleLruTlbCompressedReverseTlbSim_setup(reader_t *reader) {
  reader->trace_type = ORACLE_LRU_TLB_COMPRESSED_REVERSE_TLB_SIM_TRACE;
  reader->trace_format = BINARY_TRACE_FORMAT;
  reader->item_size = sizeof(oracle_lru_tlb_reverse_tlb_sim_entry_t);
  reader->obj_id_is_num = true;
  reader->trace_start_offset = 0;

  oracle_lru_tlb_reverse_tlb_sim_params_t *params = 
    (oracle_lru_tlb_reverse_tlb_sim_params_t *)malloc(sizeof(oracle_lru_tlb_reverse_tlb_sim_params_t));
  
  params->buffer_capacity = ORACLE_LRU_TLB_REVERSE_TLB_SIM_BUFFER_SIZE;
  
  params->entry_buffer[0] = (oracle_lru_tlb_reverse_tlb_sim_entry_t *)malloc(
    sizeof(oracle_lru_tlb_reverse_tlb_sim_entry_t) * params->buffer_capacity);
  params->entry_buffer[1] = (oracle_lru_tlb_reverse_tlb_sim_entry_t *)malloc(
    sizeof(oracle_lru_tlb_reverse_tlb_sim_entry_t) * params->buffer_capacity);
  
  if (!params->entry_buffer[0] || !params->entry_buffer[1]) {
    ERROR("Failed to allocate entry buffers\n");
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params);
    return 1;
  }
  
  params->compressed_buf_capacity = ZSTD_compressBound(
    params->buffer_capacity * sizeof(oracle_lru_tlb_reverse_tlb_sim_entry_t)) + 1024;
  params->compressed_buf = (char *)malloc(params->compressed_buf_capacity);
  
  if (!params->compressed_buf) {
    ERROR("Failed to allocate compressed buffer\n");
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params);
    return 1;
  }
  
  params->active_buffer = 0;
  params->buffer_size[0] = 0;
  params->buffer_size[1] = 0;
  params->buffer_pos[0] = -1;
  params->buffer_pos[1] = -1;
  params->eof = false;
  
  params->decompress_buffer_idx = -1;
  params->decompress_batch_idx = -1;
  params->decompress_ready = false;
  params->decompress_error = false;
  params->thread_should_exit = false;
  params->thread_running = false;
  pthread_mutex_init(&params->mutex, NULL);
  pthread_cond_init(&params->cond, NULL);
  
  params->file_handle = fopen(reader->trace_path, "rb");
  if (!params->file_handle) {
    ERROR("Failed to open file handle for background thread: %s\n",
          reader->trace_path);
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params->compressed_buf);
    pthread_mutex_destroy(&params->mutex);
    pthread_cond_destroy(&params->cond);
    free(params);
    return 1;
  }
  
  char meta_path[4096];
  snprintf(meta_path, sizeof(meta_path), "%s.meta", reader->trace_path);
  
  FILE *meta_fp = fopen(meta_path, "r");
  if (!meta_fp) {
    ERROR("Failed to open .meta file: %s\n", meta_path);
    ERROR("The .meta file is required for reverse oracle LRU TLB TlbSim traces\n");
    fclose(params->file_handle);
    free(params->entry_buffer[0]);
    free(params->entry_buffer[1]);
    free(params->compressed_buf);
    pthread_mutex_destroy(&params->mutex);
    pthread_cond_destroy(&params->cond);
    free(params);
    return 1;
  }
  
  INFO("Reading batch information from %s...\n", meta_path);
  
  char line[512];
  bool is_compressed = false;
  bool is_reverse = false;
  
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
      break;
    }
  }
  
  if (!is_compressed) {
    ERROR("Trace must be compressed (check .meta file)\n");
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
    ERROR("Trace must be in reverse order (check .meta file)\n");
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
  
  size_t batch_capacity = 10000;
  params->batch_positions = (long *)malloc(sizeof(long) * batch_capacity);
  params->batch_sizes = (size_t *)malloc(sizeof(size_t) * batch_capacity);
  params->batch_entries = (size_t *)malloc(sizeof(size_t) * batch_capacity);
  
  if (!params->batch_positions || !params->batch_sizes || !params->batch_entries) {
    ERROR("Failed to allocate batch info arrays\n");
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
  
  long cumulative_position = 0;
  
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
        ERROR("Failed to grow batch info arrays\n");
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
    ERROR("No batch information found in .meta file\n");
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
  
  uint64_t total_entries = 0;
  for (size_t i = 0; i < params->n_batches; i++) total_entries += params->batch_entries[i];
  reader->n_total_req = (int64_t)total_entries;

  INFO("Loaded %zu batches, %lu total entries\n", params->n_batches, (unsigned long)total_entries);

  params->current_batch = params->n_batches - 1;
  
  reader->reader_params = params;
  
  if (pthread_create(&params->decompress_thread, NULL, 
                     oracleLruTlbCompressedReverseTlbSim_decompress_worker, params) != 0) {
    ERROR("Failed to create background decompression thread\n");
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
  
  INFO("Setup LRU TLB TlbSim reverse oracle reader: %zu batches, entry_size=%zu, background decompression enabled\n", 
       params->n_batches, sizeof(oracle_lru_tlb_reverse_tlb_sim_entry_t));
  
  return 0;
}

static inline void oracleLruTlbCompressedReverseTlbSim_teardown(reader_t *reader) {
  if (reader->reader_params != NULL) {
    oracle_lru_tlb_reverse_tlb_sim_params_t *params = 
      (oracle_lru_tlb_reverse_tlb_sim_params_t *)reader->reader_params;
    
    if (params->thread_running) {
      pthread_mutex_lock(&params->mutex);
      params->thread_should_exit = true;
      pthread_cond_signal(&params->cond);
      pthread_mutex_unlock(&params->mutex);
      
      pthread_join(params->decompress_thread, NULL);
    }
    
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

static inline int oracleLruTlbCompressedReverseTlbSim_read_one_req(reader_t *reader,
                                                                     request_t *req) {
  oracle_lru_tlb_reverse_tlb_sim_params_t *params = 
    (oracle_lru_tlb_reverse_tlb_sim_params_t *)reader->reader_params;
  
  int active = params->active_buffer;
  
  if (params->buffer_pos[active] < 0) {
    if (params->eof) {
      req->valid = FALSE;
      return 1;
    }
    
    if (params->buffer_size[0] == 0 && params->buffer_size[1] == 0) {
      if (params->current_batch < 0 || params->current_batch >= (ssize_t)params->n_batches) {
        params->eof = true;
        req->valid = FALSE;
        return 1;
      }
      
      request_decompress_lru_tlb_sim(params, active, params->current_batch);
      
      pthread_mutex_lock(&params->mutex);
      while (!params->decompress_ready && !params->decompress_error) {
        pthread_cond_wait(&params->cond, &params->mutex);
      }
      
      if (params->decompress_error) {
        pthread_mutex_unlock(&params->mutex);
        params->eof = true;
        req->valid = FALSE;
        return 1;
      }
      pthread_mutex_unlock(&params->mutex);
      
      params->current_batch--;
      if (params->current_batch >= 0) {
        int next_buffer = 1 - active;
        request_decompress_lru_tlb_sim(params, next_buffer, params->current_batch);
      } else {
        params->eof = true;
      }
    } else {
      if (!switch_buffer_lru_tlb_sim(params)) {
        req->valid = FALSE;
        return 1;
      }
      active = params->active_buffer;
    }
  }
  
  oracle_lru_tlb_reverse_tlb_sim_entry_t *entry = &params->entry_buffer[active][params->buffer_pos[active]];
  params->buffer_pos[active]--;

  req->clock_time = (int64_t)reader->n_read_req;
  req->obj_id = entry->vaddr;
  req->obj_size = 1;
  req->next_access_vtime = entry->page_next_access_time;
  req->tlb_miss = entry->tlb_miss;
  req->cpu_id = entry->cpu;
  
  if (req->next_access_vtime == -1 || req->next_access_vtime == INT64_MAX) {
    req->next_access_vtime = MAX_REUSE_DISTANCE;
  }

  req->valid = TRUE;
  
  return 0;
}

#ifdef __cplusplus
}
#endif
