#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/*
 * filteredTrace reader -- forward chronological, zstd-compressed chunks
 *
 * Reads the output of filtered_trace_generator.cpp:
 *   Binary file: sequential zstd-compressed chunks of filtered_trace_entry_t
 *   Meta file (.meta): comment headers + CSV with
 *     chunk_index,num_events,uncompressed_size,compressed_size
 *
 * Entry format (13 bytes packed):
 *   uint64_t vaddr;
 *   uint8_t  tag;
 *   uint32_t frequency;
 *
 * Maps to request_t:
 *   obj_id         = vaddr
 *   features[0]    = tag
 *   features[1]    = frequency
 *   obj_size       = 1
 *
 * ============================================================================
 * 3-STAGE PIPELINE ARCHITECTURE
 * ============================================================================
 *
 * Stage 1: READER THREAD (sequential file I/O)
 *   - Reads compressed chunks from disk in strict order (0, 1, 2, ...)
 *   - Fills compressed_stage_t::buf[2] ping-pong buffers
 *   - Owns FILE* exclusively (no concurrent reads)
 *   - Synchronizes with decompressor via compressed.mutex + compressed.cond
 *
 * Stage 2: DECOMPRESSOR THREAD (ZSTD decompression)
 *   - Waits for ready compressed chunk via compressed.cond
 *   - Performs ZSTD_decompress (no locks held during decompression)
 *   - Stores decompressed data into decompressed_stage_t::buf[2] ping-pong
 *   - Maintains strict chunk ordering: decompresses chunk N only after N-1 done
 *   - Synchronizes with reader via compressed.mutex + compressed.cond
 *   - Synchronizes with consumer via decompressed.mutex + decompressed.cond
 *
 * Stage 3: CONSUMER THREAD (main thread, cache simulator)
 *   - Calls filteredTrace_read_one_req() to get one entry at a time
 *   - Switches to next decompressed buffer when current buffer exhausted
 *   - Maintains strict chunk ordering: consumes chunk N only after N-1 done
 *   - Synchronizes with decompressor via decompressed.mutex + decompressed.cond
 *
 * SYNCHRONIZATION MODEL:
 *   - Two independent mutex/cond pairs (one per pipeline stage)
 *   - No nested locking: decompressor carefully acquires/releases each in order
 *   - Causality enforced via chunk index matching:
 *     * Reader: next_chunk_to_read ≤ next_chunk_to_decompress ≤ next_chunk_to_consume
 *     * Decompressor only decompresses chunk matching next_chunk_to_decompress
 *     * Consumer only accepts chunk matching next_chunk_to_consume
 *
 * BUFFER STATE MACHINE (per slot):
 *   EMPTY (0)      → slot available for new work
 *   FILLING (1)    → thread actively working on this slot
 *   READY (2)      → chunk ready for next stage
 *   PROCESSING (3) → chunk being consumed by next stage
 */

#include <pthread.h>
#include <stdlib.h>
#include <zstd.h>

#include "binaryUtils.h"
#include "libCacheSim/reader.h"

#define FILTERED_TRACE_BUFFER_SIZE (24 * 1024 * 1024)
#define FILTERED_TRACE_TAG_FEATURE_IDX 0
#define FILTERED_TRACE_FREQ_FEATURE_IDX 1

/* Packed entry format from filtered_trace_generator.cpp */
typedef struct {
  uint64_t vaddr;
  uint8_t tag;
  uint32_t frequency;
} __attribute__((packed)) filtered_trace_entry_t;

/* Stage 1: Compressed buffer stage (Reader ↔ Decompressor) */
typedef struct {
  /* Synchronization for this stage */
  pthread_mutex_t mutex;
  pthread_cond_t cond;

  /* Ping-pong buffers for compressed data (reader fills, decompressor consumes) */
  char *buf[2];
  size_t capacity[2];                /* allocated size of each buffer */
  size_t size[2];                    /* bytes actually filled in this slot */
  ssize_t chunk_idx[2];              /* which chunk number is in this slot (-1 = empty) */
  int state[2];                      /* slot state machine: EMPTY|FILLING|READY|PROCESSING */

  /* Coordination indices: reader reads, decompressor decompresses */
  size_t next_chunk_to_read;
  size_t next_chunk_to_decompress;

  /* Status flags */
  bool reader_done;                  /* reader finished reading all chunks */
  bool reader_error;                 /* reader encountered an error */
} compressed_stage_t;

/* Stage 2: Decompressed buffer stage (Decompressor ↔ Consumer) */
typedef struct {
  /* Synchronization for this stage */
  pthread_mutex_t mutex;
  pthread_cond_t cond;

  /* Ping-pong buffers for decompressed data (decompressor fills, consumer reads) */
  filtered_trace_entry_t *buf[2];
  size_t capacity[2];                /* max entries in each buffer */
  size_t count[2];                   /* number of entries decompressed in this slot */
  ssize_t chunk_idx[2];              /* which chunk number is in this slot (-1 = empty) */
  int state[2];                      /* slot state machine: EMPTY|FILLING|READY|PROCESSING */

  /* Coordination indices: decompressor decompresses, consumer consumes */
  int active_buffer;                 /* which buffer is currently being read (-1 = none) */
  size_t pos;                        /* read position within active buffer */
  size_t next_chunk_to_consume;

  /* Status flags */
  bool decompress_done;              /* decompressor finished all chunks */
  bool decompress_error;             /* decompressor encountered an error */
} decompressed_stage_t;

/* Main trace reader state, wrapping both pipeline stages */
typedef struct {
  /* Pipeline stages with independent mutexes (reduces contention) */
  compressed_stage_t compressed;
  decompressed_stage_t decompressed;

  /* Metadata arrays (read-only after initialization) */
  long *chunk_positions;
  size_t *chunk_sizes;
  size_t *chunk_entries;
  size_t n_chunks;

  /* Thread management */
  pthread_t reader_thread;
  pthread_t decompress_thread;
  bool reader_thread_running;
  bool decompress_thread_running;
  bool thread_should_exit;           /* signal both threads to shut down */

  /* File I/O */
  FILE *file_handle;                 /* owned exclusively by reader thread */
  bool eof;                           /* end of file reached */

  /* Progress tracking */
  size_t last_progress_bucket;       /* for rate-limiting progress logging */
} filtered_trace_params_t;

enum {
  FILTERED_TRACE_SLOT_EMPTY = 0,      /* slot available for new work */
  FILTERED_TRACE_SLOT_FILLING = 1,    /* thread actively working on this slot */
  FILTERED_TRACE_SLOT_READY = 2,      /* chunk ready for next stage */
  FILTERED_TRACE_SLOT_PROCESSING = 3, /* chunk being consumed by next stage */
};

/* ========== HELPER FUNCTIONS ========== */

/*
 * Map tag and frequency from raw entry into request_t features array
 * Sets features[0] = tag, features[1] = frequency, updates n_features if needed
 */
static inline void filteredTrace_set_features(request_t *req,
                                              uint8_t tag,
                                              uint32_t frequency) {
  req->features[FILTERED_TRACE_TAG_FEATURE_IDX] = (int32_t)tag;
  req->features[FILTERED_TRACE_FREQ_FEATURE_IDX] = (int32_t)frequency;
  if (req->n_features <= FILTERED_TRACE_FREQ_FEATURE_IDX) {
    req->n_features = FILTERED_TRACE_FREQ_FEATURE_IDX + 1;
  }
}

/*
 * Find first EMPTY slot in state array, or -1 if both slots occupied
 * Used by reader (to find slot for next compressed chunk) and decompressor
 * (to find slot for next decompressed chunk)
 */
static inline int filteredTrace_find_empty_slot(const int state[2]) {
  if (state[0] == FILTERED_TRACE_SLOT_EMPTY) {
    return 0;
  }
  if (state[1] == FILTERED_TRACE_SLOT_EMPTY) {
    return 1;
  }
  return -1;
}

/*
 * Find compressed slot containing the exact chunk that decompressor needs next
 * Enforces strict chunk ordering: decompressor only decompresses chunk N after N-1
 * Returns slot index [0,1] if found with state==READY and chunk_idx matches target
 * Returns -1 if no such chunk available yet
 */
static inline int filteredTrace_find_ready_compressed_slot(
    const compressed_stage_t *stage) {
  ssize_t target = (ssize_t)stage->next_chunk_to_decompress;
  if (stage->state[0] == FILTERED_TRACE_SLOT_READY &&
      stage->chunk_idx[0] == target) {
    return 0;
  }
  if (stage->state[1] == FILTERED_TRACE_SLOT_READY &&
      stage->chunk_idx[1] == target) {
    return 1;
  }
  return -1;
}

/*
 * Find decompressed slot containing the exact chunk that consumer needs next
 * Enforces strict chunk ordering: consumer only reads chunk N after N-1
 * Returns slot index [0,1] if found with state==READY and chunk_idx matches target
 * Returns -1 if no such chunk available yet
 */
static inline int filteredTrace_find_ready_decompressed_slot(
    const decompressed_stage_t *stage) {
  ssize_t target = (ssize_t)stage->next_chunk_to_consume;
  if (stage->state[0] == FILTERED_TRACE_SLOT_READY &&
      stage->chunk_idx[0] == target) {
    return 0;
  }
  if (stage->state[1] == FILTERED_TRACE_SLOT_READY &&
      stage->chunk_idx[1] == target) {
    return 1;
  }
  return -1;
}

/* ========== THREAD ENTRY POINTS ========== */

/*
 * READER WORKER THREAD
 * 
 * Responsibility: Read compressed chunks from disk in sequential order
 * - Runs in background thread, owned by filteredTrace_setup()
 * - Reads from disk via fread(), filling compressed.buf[2] ping-pong buffers
 * - Strictly sequential: chunk 0, then 1, then 2, ... (no seeking)
 * - Coordinates with decompressor via compressed.mutex + compressed.cond
 * 
 * Flow:
 *   1. Lock compressed.mutex
 *   2. While not done:
 *      - Find EMPTY slot for next chunk
 *      - If no slot available, wait on compressed.cond (decompressor will signal)
 *      - Mark slot as FILLING, increment next_chunk_to_read, unlock
 *   3. Perform fread() without holding lock (I/O outside critical section)
 *   4. Lock compressed.mutex
 *      - If read succeeded: mark slot READY, increment next_chunk_to_decompress
 *      - If read failed: mark slot EMPTY, set reader_error flag
 *      - Signal decompressor via compressed.cond
 */
static void *filteredTrace_reader_worker(void *arg) {
  filtered_trace_params_t *params = (filtered_trace_params_t *)arg;
  compressed_stage_t *comp = &params->compressed;

  while (true) {
    pthread_mutex_lock(&comp->mutex);
    int slot = -1;

    /* Find available slot or wait for decompressor to free one */
    while (!params->thread_should_exit) {
      /* Check if all chunks have been read */
      if (comp->next_chunk_to_read >= params->n_chunks) {
        comp->reader_done = true;
        pthread_cond_broadcast(&comp->cond);
        pthread_mutex_unlock(&comp->mutex);
        return NULL;
      }

      /* Find EMPTY slot to fill */
      slot = filteredTrace_find_empty_slot(comp->state);
      if (slot >= 0) {
        comp->state[slot] = FILTERED_TRACE_SLOT_FILLING;
        break;
      }

      /* No empty slot; wait for decompressor to consume one */
      pthread_cond_wait(&comp->cond, &comp->mutex);
    }

    /* Exit signal received from teardown */
    if (params->thread_should_exit) {
      pthread_mutex_unlock(&comp->mutex);
      return NULL;
    }

    /* slot is guaranteed to be >= 0 here, or we would have exited above */
    size_t chunk_idx = comp->next_chunk_to_read;
    size_t chunk_size = params->chunk_sizes[chunk_idx];
    comp->next_chunk_to_read++;
    comp->chunk_idx[slot] = (ssize_t)chunk_idx;

    /* Release lock before I/O (allows decompressor to work on other buffers) */
    pthread_mutex_unlock(&comp->mutex);

    /* Perform disk I/O without holding lock */
    bool success = false;

    /* Resize buffer if needed to accommodate this chunk */
    if (chunk_size > comp->capacity[slot]) {
      size_t new_cap = chunk_size + 1024;
      char *new_buf = (char *)realloc(comp->buf[slot], new_cap);
      if (new_buf != NULL) {
        comp->buf[slot] = new_buf;
        comp->capacity[slot] = new_cap;
      }
    }

    /* Read chunk from file */
    if (chunk_size <= comp->capacity[slot]) {
      if (fread(comp->buf[slot], 1, chunk_size, params->file_handle) ==
          chunk_size) {
        success = true;
      }
    }

    /* Update state with result and signal decompressor */
    pthread_mutex_lock(&comp->mutex);
    if (!success) {
      comp->reader_error = true;
      comp->reader_done = true;
      comp->state[slot] = FILTERED_TRACE_SLOT_EMPTY;
      LOG(ERROR, STREAM_Reader,
          "filteredTrace: failed to read compressed chunk %zu, aborting\n", chunk_idx);
      pthread_cond_broadcast(&comp->cond);
      pthread_mutex_unlock(&comp->mutex);
      /* Reader errors are unrecoverable; fail hard immediately */
      abort();
    }

    /* Mark this chunk as ready for decompression */
    comp->size[slot] = chunk_size;
    comp->state[slot] = FILTERED_TRACE_SLOT_READY;
    pthread_cond_broadcast(&comp->cond);  /* Wake decompressor */
    pthread_mutex_unlock(&comp->mutex);
  }
}

/*
 * DECOMPRESSOR WORKER THREAD
 * 
 * Responsibility: Decompress chunks in order and pass to consumer
 * - Runs in background thread, owned by filteredTrace_setup()
 * - Bridges Stage 1 (compressed) and Stage 2 (decompressed)
 * - Maintains causality: decompresses chunk N only after N-1 done
 * - Performs ZSTD_decompress without holding locks (compute outside critical section)
 * - Coordinates with reader via compressed.mutex + compressed.cond
 * - Coordinates with consumer via decompressed.mutex + decompressed.cond
 * 
 * Key Design: Decompressor acquires/releases locks in order to avoid deadlock
 *   1. Lock compressed.mutex to check for ready chunks
 *   2. Unlock compressed.mutex, perform decompression
 *   3. Lock decompressed.mutex to store results
 *   4. Release both locks in sequence
 * 
 * Flow:
 *   1. Lock compressed.mutex, find chunk matching next_chunk_to_decompress
 *   2. If available: lock decompressed.mutex, find empty slot
 *   3. Unlock both, perform ZSTD_decompress without holding locks
 *   4. Lock both again, update state: mark compressed EMPTY, decompressed READY
 *   5. Signal reader and consumer via respective condition variables
 */
static void *filteredTrace_decompress_worker(void *arg) {
  filtered_trace_params_t *params = (filtered_trace_params_t *)arg;
  compressed_stage_t *comp = &params->compressed;
  decompressed_stage_t *decomp = &params->decompressed;

  while (true) {
    /* ===== STAGE 1: Find ready compressed chunk ===== */
    pthread_mutex_lock(&comp->mutex);

    if (params->thread_should_exit) {
      pthread_mutex_unlock(&comp->mutex);
      break;
    }

    /* Look for chunk matching next_chunk_to_decompress (enforces causality) */
    int comp_slot = filteredTrace_find_ready_compressed_slot(comp);
    while (!params->thread_should_exit && comp_slot < 0) {
      /* Check if we're done (all chunks read and decompressed) */
      if (comp->reader_done && comp->next_chunk_to_decompress >= params->n_chunks) {
        decomp->decompress_done = true;
        pthread_mutex_unlock(&comp->mutex);
        /* Signal consumer in case it's waiting */
        pthread_mutex_lock(&decomp->mutex);
        pthread_cond_broadcast(&decomp->cond);
        pthread_mutex_unlock(&decomp->mutex);
        return NULL;
      }
      /* No ready chunk yet; wait for reader to fill one */
      pthread_cond_wait(&comp->cond, &comp->mutex);
      comp_slot = filteredTrace_find_ready_compressed_slot(comp);
    }

    if (params->thread_should_exit) {
      pthread_mutex_unlock(&comp->mutex);
      break;
    }

    /* ===== STAGE 2: Find empty decompressed slot ===== */
    ssize_t chunk_idx = comp->chunk_idx[comp_slot];
    size_t compressed_size = comp->size[comp_slot];
    size_t expected_entries = params->chunk_entries[chunk_idx];

    /* Mark compressed chunk as being processed (reader can't reuse it yet) */
    comp->state[comp_slot] = FILTERED_TRACE_SLOT_PROCESSING;
    pthread_mutex_unlock(&comp->mutex);

    /* Now lock decompressed stage to find space */
    pthread_mutex_lock(&decomp->mutex);

    int dec_slot = filteredTrace_find_empty_slot(decomp->state);
    while (!params->thread_should_exit && dec_slot < 0) {
      /* No empty decompressed slot; wait for consumer to consume one */
      pthread_cond_wait(&decomp->cond, &decomp->mutex);
      dec_slot = filteredTrace_find_empty_slot(decomp->state);
    }

    if (params->thread_should_exit) {
      pthread_mutex_unlock(&decomp->mutex);
      break;
    }

    /* Mark decompressed slot as filling and unlock before decompression */
    decomp->state[dec_slot] = FILTERED_TRACE_SLOT_FILLING;
    decomp->chunk_idx[dec_slot] = chunk_idx;
    pthread_mutex_unlock(&decomp->mutex);

    /* ===== STAGE 3: Perform decompression (no locks held) ===== */
    bool success = false;

    /* Resize decompressed buffer if needed */
    if (expected_entries > decomp->capacity[dec_slot]) {
      filtered_trace_entry_t *new_buf = (filtered_trace_entry_t *)realloc(
          decomp->buf[dec_slot],
          expected_entries * sizeof(filtered_trace_entry_t));
      if (new_buf != NULL) {
        decomp->buf[dec_slot] = new_buf;
        decomp->capacity[dec_slot] = expected_entries;
      }
    }

    /* Decompress compressed chunk -> decompressed buffer */
    if (expected_entries <= decomp->capacity[dec_slot]) {
      size_t dec = ZSTD_decompress(
          decomp->buf[dec_slot],
          decomp->capacity[dec_slot] * sizeof(filtered_trace_entry_t),
          comp->buf[comp_slot], compressed_size);

      if (!ZSTD_isError(dec)) {
        size_t got_entries = dec / sizeof(filtered_trace_entry_t);
        if (got_entries != expected_entries) {
          LOG(WARN, STREAM_Reader,
              "filteredTrace: chunk %zd entry count mismatch: expected %zu, got %zu\n",
              chunk_idx, expected_entries, got_entries);
        }
        decomp->count[dec_slot] = got_entries;
        success = true;
      } else {
        LOG(ERROR, STREAM_Reader,
            "filteredTrace: zstd error chunk %zd: %s, aborting\n", chunk_idx,
            ZSTD_getErrorName(dec));
        /* Decompression errors are unrecoverable; fail hard immediately */
        abort();
      }
    }

    /* ===== STAGE 4: Decompression must have succeeded (failures abort) ===== */
    if (!success) {
      LOG(ERROR, STREAM_Reader,
          "filteredTrace: decompression failed for chunk %zd (buffer allocation or size mismatch), aborting\n",
          chunk_idx);
      /* Decompression failures are unrecoverable; fail hard immediately */
      abort();
    }

    /* Update state and signal both reader and consumer */
    pthread_mutex_lock(&comp->mutex);
    comp->state[comp_slot] = FILTERED_TRACE_SLOT_EMPTY;
    comp->chunk_idx[comp_slot] = -1;
    comp->size[comp_slot] = 0;
    pthread_cond_broadcast(&comp->cond);  /* Wake reader */
    pthread_mutex_unlock(&comp->mutex);

    pthread_mutex_lock(&decomp->mutex);
    decomp->state[dec_slot] = FILTERED_TRACE_SLOT_READY;
    decomp->next_chunk_to_consume++;
    pthread_cond_broadcast(&decomp->cond);  /* Wake consumer */
    pthread_mutex_unlock(&decomp->mutex);
  }

  return NULL;
}

/*
 * CONSUMER HELPER: Switch to next decompressed buffer
 * 
 * Called by consumer (cache simulator main thread) when current buffer exhausted
 * Marks current buffer EMPTY and waits for next buffer to be READY
 * Enforces strict chunk ordering: consumer switches to chunk N only after N-1 done
 * 
 * Returns: true if successfully switched to next buffer, false on EOF or error
 */
static inline bool filteredTrace_switch_buffer(filtered_trace_params_t *params) {
  decompressed_stage_t *decomp = &params->decompressed;

  pthread_mutex_lock(&decomp->mutex);

  /* If there's an active buffer, mark it empty and signal decompressor */
  if (decomp->active_buffer >= 0) {
    int old = decomp->active_buffer;
    decomp->state[old] = FILTERED_TRACE_SLOT_EMPTY;
    decomp->chunk_idx[old] = -1;
    decomp->count[old] = 0;
    decomp->active_buffer = -1;
    pthread_cond_broadcast(&decomp->cond);
  }

  /* Wait for next buffer matching next_chunk_to_consume (causality) */
  while (!params->thread_should_exit) {
    int next_buffer = filteredTrace_find_ready_decompressed_slot(decomp);
    if (next_buffer >= 0) {
      /* Found next chunk in order; mark it as processing and activate */
      decomp->state[next_buffer] = FILTERED_TRACE_SLOT_PROCESSING;
      decomp->active_buffer = next_buffer;
      decomp->pos = 0;  /* reset read position for new active buffer */
      decomp->next_chunk_to_consume++;

      pthread_mutex_unlock(&decomp->mutex);
      return true;
    }

    /* Check if we've reached EOF (decompressor done and all chunks consumed) */
    if (decomp->decompress_done &&
        decomp->next_chunk_to_consume >= params->n_chunks) {
      params->eof = true;
      pthread_mutex_unlock(&decomp->mutex);
      return false;
    }

    /* No next chunk ready yet; wait for decompressor to produce one */
    pthread_cond_wait(&decomp->cond, &decomp->mutex);
  }

  /* Exit signal: consumer thread requested shutdown */
  pthread_mutex_unlock(&decomp->mutex);
  return false;
}

/* ========== INITIALIZATION & TEARDOWN ========== */

/*
 * SETUP: Initialize reader and start background threads
 * 
 * Responsibilities:
 *   1. Parse .meta file to read chunk metadata (sizes, entry counts)
 *   2. Allocate ping-pong buffers for both stages
 *   3. Initialize synchronization primitives (2 independent mutex/cond pairs)
 *   4. Open trace file
 *   5. Create reader thread (fills compressed buffers)
 *   6. Create decompressor thread (decompresses and fills decompressed buffers)
 * 
 * Returns: 0 on success, 1 on failure
 */
static inline int filteredTrace_setup(reader_t *reader) {
  reader->trace_type = FILTERED_TRACE;
  reader->trace_format = BINARY_TRACE_FORMAT;
  reader->item_size = sizeof(filtered_trace_entry_t);
  reader->obj_id_is_num = true;
  reader->trace_start_offset = 0;

  /* Allocate main params structure */
  filtered_trace_params_t *params =
      (filtered_trace_params_t *)calloc(1, sizeof(filtered_trace_params_t));

  params->decompressed.active_buffer = -1;
  params->last_progress_bucket = (size_t)-1;

  /* ===== ALLOCATE DECOMPRESSED STAGE BUFFERS ===== */
  params->decompressed.capacity[0] = FILTERED_TRACE_BUFFER_SIZE;
  params->decompressed.capacity[1] = FILTERED_TRACE_BUFFER_SIZE;
  params->decompressed.buf[0] = (filtered_trace_entry_t *)malloc(
      sizeof(filtered_trace_entry_t) * params->decompressed.capacity[0]);
  params->decompressed.buf[1] = (filtered_trace_entry_t *)malloc(
      sizeof(filtered_trace_entry_t) * params->decompressed.capacity[1]);
  if (!params->decompressed.buf[0] || !params->decompressed.buf[1]) {
    LOG(ERROR, STREAM_Reader,
        "filteredTrace: failed to allocate entry buffers\n");
    free(params->decompressed.buf[0]);
    free(params->decompressed.buf[1]);
    free(params);
    return 1;
  }

  /* ===== ALLOCATE COMPRESSED STAGE BUFFERS ===== */
  params->compressed.capacity[0] =
      ZSTD_compressBound(params->decompressed.capacity[0] *
                         sizeof(filtered_trace_entry_t)) +
      1024;
  params->compressed.capacity[1] =
      ZSTD_compressBound(params->decompressed.capacity[1] *
                         sizeof(filtered_trace_entry_t)) +
      1024;
  params->compressed.buf[0] = (char *)malloc(params->compressed.capacity[0]);
  params->compressed.buf[1] = (char *)malloc(params->compressed.capacity[1]);
  if (!params->compressed.buf[0] || !params->compressed.buf[1]) {
    LOG(ERROR, STREAM_Reader,
        "filteredTrace: failed to allocate compressed buffers\n");
    free(params->decompressed.buf[0]);
    free(params->decompressed.buf[1]);
    free(params->compressed.buf[0]);
    free(params->compressed.buf[1]);
    free(params);
    return 1;
  }

  /* ===== INITIALIZE BUFFER STATE ARRAYS ===== */
  params->decompressed.count[0] = 0;
  params->decompressed.count[1] = 0;
  params->decompressed.chunk_idx[0] = -1;
  params->decompressed.chunk_idx[1] = -1;
  params->decompressed.state[0] = FILTERED_TRACE_SLOT_EMPTY;
  params->decompressed.state[1] = FILTERED_TRACE_SLOT_EMPTY;
  params->decompressed.pos = 0;  /* read position for active buffer */

  params->compressed.size[0] = 0;
  params->compressed.size[1] = 0;
  params->compressed.chunk_idx[0] = -1;
  params->compressed.chunk_idx[1] = -1;
  params->compressed.state[0] = FILTERED_TRACE_SLOT_EMPTY;
  params->compressed.state[1] = FILTERED_TRACE_SLOT_EMPTY;

  /* ===== INITIALIZE COORDINATION INDICES ===== */
  params->compressed.next_chunk_to_read = 0;
  params->compressed.next_chunk_to_decompress = 0;
  params->decompressed.next_chunk_to_consume = 0;

  /* ===== INITIALIZE ERROR & STATUS FLAGS ===== */
  params->eof = false;
  params->compressed.reader_done = false;
  params->compressed.reader_error = false;
  params->decompressed.decompress_done = false;
  params->decompressed.decompress_error = false;

  /* ===== INITIALIZE THREAD CONTROL ===== */
  params->thread_should_exit = false;
  params->reader_thread_running = false;
  params->decompress_thread_running = false;

  /* ===== INITIALIZE SYNCHRONIZATION PRIMITIVES (Independent per stage) ===== */
  pthread_mutex_init(&params->compressed.mutex, NULL);
  pthread_cond_init(&params->compressed.cond, NULL);
  pthread_mutex_init(&params->decompressed.mutex, NULL);
  pthread_cond_init(&params->decompressed.cond, NULL);

  /* ===== OPEN TRACE FILE ===== */
  params->file_handle = fopen(reader->trace_path, "rb");
  if (!params->file_handle) {
    LOG(ERROR, STREAM_Reader,
        "filteredTrace: failed to open %s for reader thread\n", reader->trace_path);
    free(params->decompressed.buf[0]);
    free(params->decompressed.buf[1]);
    free(params->compressed.buf[0]);
    free(params->compressed.buf[1]);
    pthread_mutex_destroy(&params->compressed.mutex);
    pthread_cond_destroy(&params->compressed.cond);
    pthread_mutex_destroy(&params->decompressed.mutex);
    pthread_cond_destroy(&params->decompressed.cond);
    free(params);
    return 1;
  }

  /* ===== READ METADATA FILE (.meta) ===== */
  char meta_path[4096];
  snprintf(meta_path, sizeof(meta_path), "%s.meta", reader->trace_path);

  FILE *meta_fp = fopen(meta_path, "r");
  if (!meta_fp) {
    LOG(ERROR, STREAM_Reader,
        "filteredTrace: failed to open .meta file: %s\n", meta_path);
    fclose(params->file_handle);
    free(params->decompressed.buf[0]);
    free(params->decompressed.buf[1]);
    free(params->compressed.buf[0]);
    free(params->compressed.buf[1]);
    pthread_mutex_destroy(&params->compressed.mutex);
    pthread_cond_destroy(&params->compressed.cond);
    pthread_mutex_destroy(&params->decompressed.mutex);
    pthread_cond_destroy(&params->decompressed.cond);
    free(params);
    return 1;
  }

  LOG(INFO, STREAM_Reader, "filteredTrace: reading metadata from %s\n", meta_path);

  char line[512];

  /* Skip header comments in .meta file */
  while (fgets(line, sizeof(line), meta_fp)) {
    if (strncmp(line, "chunk_index,", 12) == 0) {
      break;
    }
  }

  /* Allocate arrays for chunk metadata */
  size_t chunk_capacity = 10000;
  params->chunk_positions = (long *)malloc(sizeof(long) * chunk_capacity);
  params->chunk_sizes = (size_t *)malloc(sizeof(size_t) * chunk_capacity);
  params->chunk_entries = (size_t *)malloc(sizeof(size_t) * chunk_capacity);

  if (!params->chunk_positions || !params->chunk_sizes || !params->chunk_entries) {
    LOG(ERROR, STREAM_Reader,
        "filteredTrace: failed to allocate chunk info arrays\n");
    fclose(meta_fp);
    fclose(params->file_handle);
    free(params->decompressed.buf[0]);
    free(params->decompressed.buf[1]);
    free(params->compressed.buf[0]);
    free(params->compressed.buf[1]);
    free(params->chunk_positions);
    free(params->chunk_sizes);
    free(params->chunk_entries);
    pthread_mutex_destroy(&params->compressed.mutex);
    pthread_cond_destroy(&params->compressed.cond);
    pthread_mutex_destroy(&params->decompressed.mutex);
    pthread_cond_destroy(&params->decompressed.cond);
    free(params);
    return 1;
  }

  /* Parse CSV lines from .meta file */
  params->n_chunks = 0;
  long cumulative_pos = 0;

  while (fgets(line, sizeof(line), meta_fp)) {
    if (line[0] == '#') {
      continue;
    }

    /* Grow arrays if needed */
    if (params->n_chunks >= chunk_capacity) {
      chunk_capacity *= 2;
      params->chunk_positions =
          (long *)realloc(params->chunk_positions, sizeof(long) * chunk_capacity);
      params->chunk_sizes =
          (size_t *)realloc(params->chunk_sizes, sizeof(size_t) * chunk_capacity);
      params->chunk_entries =
          (size_t *)realloc(params->chunk_entries, sizeof(size_t) * chunk_capacity);
      if (!params->chunk_positions || !params->chunk_sizes ||
          !params->chunk_entries) {
        LOG(ERROR, STREAM_Reader,
            "filteredTrace: failed to grow chunk arrays\n");
        break;
      }
    }

    /* Parse CSV: chunk_index,num_events,uncompressed_size,compressed_size */
    uint64_t chunk_index = 0;
    size_t n_events = 0;
    size_t uncompressed_size = 0;
    size_t compressed_size = 0;
    if (sscanf(line, "%lu,%zu,%zu,%zu", &chunk_index, &n_events,
               &uncompressed_size, &compressed_size) == 4) {
      (void)chunk_index;
      (void)uncompressed_size;
      params->chunk_positions[params->n_chunks] = cumulative_pos;
      params->chunk_sizes[params->n_chunks] = compressed_size;
      params->chunk_entries[params->n_chunks] = n_events;
      params->n_chunks++;
      cumulative_pos += (long)compressed_size;
    }
  }

  fclose(meta_fp);

  if (params->n_chunks == 0) {
    LOG(ERROR, STREAM_Reader,
        "filteredTrace: no chunk information in .meta file\n");
    fclose(params->file_handle);
    free(params->decompressed.buf[0]);
    free(params->decompressed.buf[1]);
    free(params->compressed.buf[0]);
    free(params->compressed.buf[1]);
    free(params->chunk_positions);
    free(params->chunk_sizes);
    free(params->chunk_entries);
    pthread_mutex_destroy(&params->compressed.mutex);
    pthread_cond_destroy(&params->compressed.cond);
    pthread_mutex_destroy(&params->decompressed.mutex);
    pthread_cond_destroy(&params->decompressed.cond);
    free(params);
    return 1;
  }

  /* Calculate total entries for logging */
  uint64_t total_entries = 0;
  for (size_t i = 0; i < params->n_chunks; i++) {
    total_entries += params->chunk_entries[i];
  }
  reader->n_total_req = (int64_t)total_entries;

  LOG(INFO, STREAM_Reader,
      "filteredTrace: %zu chunks, %lu total entries\n", params->n_chunks,
      (unsigned long)total_entries);

  reader->reader_params = params;

  /* ===== CREATE READER THREAD ===== */
  if (pthread_create(&params->reader_thread, NULL,
                     filteredTrace_reader_worker, params) != 0) {
    LOG(ERROR, STREAM_Reader,
        "filteredTrace: failed to create reader thread\n");
    fclose(params->file_handle);
    free(params->decompressed.buf[0]);
    free(params->decompressed.buf[1]);
    free(params->compressed.buf[0]);
    free(params->compressed.buf[1]);
    free(params->chunk_positions);
    free(params->chunk_sizes);
    free(params->chunk_entries);
    pthread_mutex_destroy(&params->compressed.mutex);
    pthread_cond_destroy(&params->compressed.cond);
    pthread_mutex_destroy(&params->decompressed.mutex);
    pthread_cond_destroy(&params->decompressed.cond);
    free(params);
    return 1;
  }
  params->reader_thread_running = true;

  /* ===== CREATE DECOMPRESSOR THREAD ===== */
  if (pthread_create(&params->decompress_thread, NULL,
                     filteredTrace_decompress_worker, params) != 0) {
    LOG(ERROR, STREAM_Reader,
        "filteredTrace: failed to create decompression thread\n");
    /* Signal reader thread to exit */
    pthread_mutex_lock(&params->compressed.mutex);
    params->thread_should_exit = true;
    pthread_cond_broadcast(&params->compressed.cond);
    pthread_mutex_unlock(&params->compressed.mutex);
    pthread_mutex_lock(&params->decompressed.mutex);
    pthread_cond_broadcast(&params->decompressed.cond);
    pthread_mutex_unlock(&params->decompressed.mutex);
    /* Wait for reader to finish */
    pthread_join(params->reader_thread, NULL);
    params->reader_thread_running = false;
    fclose(params->file_handle);
    free(params->decompressed.buf[0]);
    free(params->decompressed.buf[1]);
    free(params->compressed.buf[0]);
    free(params->compressed.buf[1]);
    free(params->chunk_positions);
    free(params->chunk_sizes);
    free(params->chunk_entries);
    pthread_mutex_destroy(&params->compressed.mutex);
    pthread_cond_destroy(&params->compressed.cond);
    pthread_mutex_destroy(&params->decompressed.mutex);
    pthread_cond_destroy(&params->decompressed.cond);
    free(params);
    return 1;
  }
  params->decompress_thread_running = true;

  LOG(INFO, STREAM_Reader,
      "filteredTrace: setup complete, 3-stage reader/decompress/consume pipeline enabled\n");

  return 0;
}

/*
 * TEARDOWN: Shut down background threads and free resources
 * 
 * Called when cache simulator is done reading or on error
 * Responsibilities:
 *   1. Signal both threads to exit (via thread_should_exit flag)
 *   2. Wait for threads to finish (pthread_join)
 *   3. Close file handle
 *   4. Free all allocated buffers
 *   5. Destroy synchronization primitives
 */
static inline void filteredTrace_teardown(reader_t *reader) {
  if (reader->reader_params == NULL) {
    return;
  }

  filtered_trace_params_t *params =
      (filtered_trace_params_t *)reader->reader_params;

  /* Signal threads to exit */
  pthread_mutex_lock(&params->compressed.mutex);
  params->thread_should_exit = true;
  pthread_cond_broadcast(&params->compressed.cond);
  pthread_mutex_unlock(&params->compressed.mutex);

  pthread_mutex_lock(&params->decompressed.mutex);
  pthread_cond_broadcast(&params->decompressed.cond);
  pthread_mutex_unlock(&params->decompressed.mutex);

  /* Wait for threads to finish */
  if (params->reader_thread_running) {
    pthread_join(params->reader_thread, NULL);
  }
  if (params->decompress_thread_running) {
    pthread_join(params->decompress_thread, NULL);
  }

  /* Clean up file handle */
  if (params->file_handle) {
    fclose(params->file_handle);
  }

  /* Free buffers */
  free(params->decompressed.buf[0]);
  free(params->decompressed.buf[1]);
  free(params->compressed.buf[0]);
  free(params->compressed.buf[1]);
  free(params->chunk_positions);
  free(params->chunk_sizes);
  free(params->chunk_entries);

  /* Destroy synchronization primitives */
  pthread_mutex_destroy(&params->compressed.mutex);
  pthread_cond_destroy(&params->compressed.cond);
  pthread_mutex_destroy(&params->decompressed.mutex);
  pthread_cond_destroy(&params->decompressed.cond);

  free(params);
  reader->reader_params = NULL;
}

/*
 * HOT PATH: Consumer reads one entry at a time
 * 
 * Called by cache simulator for each memory access
 * Responsibility: Yield one filtered_trace_entry_t as a request_t
 * - Returns one entry from active decompressed buffer
 * - Switches to next buffer when current buffer exhausted
 * - Blocks if next buffer not ready yet (decompressor is catching up)
 * - Maps vaddr -> obj_id, tag -> features[0], frequency -> features[1]
 * 
 * Returns: 0 on success, 1 on EOF or error
 */
static inline int filteredTrace_read_one_req(reader_t *reader, request_t *req) {
  filtered_trace_params_t *params =
      (filtered_trace_params_t *)reader->reader_params;
  decompressed_stage_t *decomp = &params->decompressed;

  int active = decomp->active_buffer;

  /* If no active buffer or current buffer exhausted, switch to next */
  if (active < 0 || decomp->pos >= decomp->count[active]) {
    if (!filteredTrace_switch_buffer(params)) {
      req->valid = false;
      return 1;
    }
    active = decomp->active_buffer;
  }

  /* Get next entry from active buffer */
  filtered_trace_entry_t *entry = &decomp->buf[active][decomp->pos];
  decomp->pos++;

  /* Map entry fields to request_t */
  req->clock_time = (int64_t)reader->n_read_req;
  req->obj_id = (obj_id_t)entry->vaddr;
  req->obj_size = 1;
  req->next_access_vtime = -2;
  filteredTrace_set_features(req, entry->tag, entry->frequency);
  req->valid = true;

  return 0;
}

#ifdef __cplusplus
}
#endif
