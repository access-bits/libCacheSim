//
//  profiler.c
//  libCacheSim
//
//  Created by Juncheng on 11/20/19.
//  Copyright © 2016-2019 Juncheng. All rights reserved.
//

#ifdef __cplusplus
extern "C" {
#endif

#include "libCacheSim/sim_config.h"
#include "libCacheSim/simulator.h"
#include "libCacheSim/log.h"

#include <math.h>
#include <semaphore.h>

#include "../cache/cacheUtils.h"
#include "../utils/include/myprint.h"
#include "../utils/include/mystr.h"
#include "libCacheSim/evictionAlgo.h"
#include "libCacheSim/plugin.h"

/* ================================================================
 * Bounded ring-buffer queue — used by batch-and-barrier pattern
 *
 * Batch-and-barrier pattern: Reader accumulates requests in local buffer,
 * waits for all queues to drain, then bulk-pushes entire batch to all queues.
 * Memory bounded at O(num_caches * queue_depth * sizeof(request_t)).
 * ================================================================ */

#define BQUEUE_DEFAULT_DEPTH 1024

typedef struct {
  request_t *buf;        /* ring buffer; stores request_t by value */
  int        capacity;
  int        head, tail, count;
  GMutex     mutex;      /* protects head/tail/count/closed */
  sem_t      empty_sem;  /* worker posts when queue drains to 0 */
  sem_t      ready_sem;  /* reader posts when new batch is ready */
  bool       closed;     /* set by reader at EOF; worker exits */
} bounded_queue_t;

static bounded_queue_t *bqueue_create(int capacity) {
  bounded_queue_t *q = malloc(sizeof(bounded_queue_t));
  q->buf      = malloc(sizeof(request_t) * (size_t)capacity);
  q->capacity = capacity;
  q->head = q->tail = q->count = 0;
  q->closed   = false;
  g_mutex_init(&q->mutex);
  sem_init(&q->empty_sem, 0, 1);  /* starts at 1; queue begins empty */
  sem_init(&q->ready_sem, 0, 0);  /* starts at 0; reader posts when ready */
  return q;
}

static void bqueue_destroy(bounded_queue_t *q) {
  sem_destroy(&q->empty_sem);
  sem_destroy(&q->ready_sem);
  g_mutex_clear(&q->mutex);
  free(q->buf);
  free(q);
}

/**
 * Reader: Bulk-push a batch of requests to queue.
 * Normally batch_size == queue_depth (barrier ensures queue is empty).
 * Only the final batch may be < queue_depth (EOF reached).
 */
static void bqueue_push_batch(bounded_queue_t *q, const request_t *batch,
                               int batch_size) {
  g_mutex_lock(&q->mutex);
  
  /* Bulk copy batch */
  memcpy(q->buf, batch, (size_t)batch_size * sizeof(request_t));
  q->head  = 0;
  q->tail  = batch_size % q->capacity;
  q->count = batch_size;
  
  g_mutex_unlock(&q->mutex);
  
  /* Signal worker that batch is ready */
  sem_post(&q->ready_sem);
}

/**
 * Worker: Process all requests from queue while holding lock.
 * No contention - reader is blocked at barrier waiting for empty_sem.
 * Returns number of requests processed (queue_depth normally, 0 on EOF).
 */
static int bqueue_process_batch(bounded_queue_t *q, cache_t *cache,
                                 cache_stat_t *result, uint64_t *consumed,
                                 int64_t *start_ts, uint64_t n_warmup_req,
                                 int warmup_sec) {
  g_mutex_lock(&q->mutex);
  
  if (q->count == 0 && q->closed) {
    g_mutex_unlock(&q->mutex);
    return 0;  /* EOF */
  }
  
  int batch_size = q->count;
  
  /* Process all requests directly from queue buffer (while holding lock) */
  for (int i = 0; i < batch_size; i++) {
    request_t *req = &q->buf[q->head];
    q->head = (q->head + 1) % q->capacity;
    (*consumed)++;

    /* Record time base on the first request */
    if (*start_ts == INT64_MIN) {
      *start_ts = req->clock_time;
    }
    req->clock_time -= *start_ts;

    /* Warmup phase — count/time-based warmup */
    if (*consumed <= n_warmup_req ||
        (warmup_sec > 0 && req->clock_time < (int64_t)warmup_sec)) {
      cache->get(cache, req);
      result->n_warmup_req++;
      continue;
    }

    /* Measured simulation */
    result->n_req++;
    result->n_req_byte += req->obj_size;
    result->n_req_cost += req->obj_cost;
    if (!cache->get(cache, req)) {
      result->n_miss++;
      result->n_miss_byte += req->obj_size;
      result->n_miss_cost += req->obj_cost;
    }
  }
  
  q->count = 0;
  
  g_mutex_unlock(&q->mutex);
  
  /* Signal reader that queue is empty (barrier synchronization) */
  sem_post(&q->empty_sem);
  
  return batch_size;
}

/**
 * Reader: Signal EOF and wake worker for final cleanup.
 */
static void bqueue_close(bounded_queue_t *q) {
  g_mutex_lock(&q->mutex);
  q->closed = true;
  g_mutex_unlock(&q->mutex);
  
  /* Wake worker so it can see closed flag and exit */
  sem_post(&q->ready_sem);
}

/* Per-worker parameters for _simulate_from_queue */
typedef struct {
  bounded_queue_t *queue;
  cache_t         *cache;
  cache_stat_t    *result;
  uint64_t         n_warmup_req;     /* count-based warmup from main reader */
  int              warmup_sec;       /* time-based warmup from main reader */
  bool             free_cache_when_finish;
  bool             use_random_seed;
  GMutex          *progress_mtx;
  gint            *progress;
  stream_id_t      config_stream;    /* log routing for this worker (0 = none) */
} sim_queue_worker_params_t;

static void _simulate_from_queue(gpointer data, gpointer user_data) {
  (void)user_data;
  sim_queue_worker_params_t *p = (sim_queue_worker_params_t *)data;

  /* Route worker thread log output to its config's stream */
  if (p->config_stream != 0) {
    log_set_thread_stream(p->config_stream);
  }

  if (p->use_random_seed) {
    set_rand_seed(rand());
  } else {
    set_rand_seed(1);
  }

  cache_t      *cache  = p->cache;
  cache_stat_t *result = p->result;
  strncpy(result->cache_name, cache->cache_name, CACHE_NAME_ARRAY_LEN - 1);
  result->cache_name[CACHE_NAME_ARRAY_LEN - 1] = '\0';

  uint64_t consumed = 0;
  int64_t  start_ts = INT64_MIN;

  /* Batch-and-barrier loop: wait for batch → process from queue buffer */
  while (true) {
    /* Wait for reader to signal batch ready */
    sem_wait(&p->queue->ready_sem);
    
    /* Process all requests directly from queue buffer (single lock held) */
    int processed = bqueue_process_batch(
        p->queue, cache, result, &consumed, &start_ts,
        p->n_warmup_req, p->warmup_sec);
    
    if (processed == 0) {
      break;  /* EOF reached */
    }
  }

  result->n_obj         = cache->n_obj;
  result->occupied_byte = cache->occupied_byte;

  g_mutex_lock(p->progress_mtx);
  (*p->progress)++;
  g_mutex_unlock(p->progress_mtx);

  if (p->free_cache_when_finish) {
    cache->cache_free(cache);
  }
  
  free(p);
}



/**
 * @brief Simulate a list of configurations, each with its own policy, size, and
 *        warmup settings, reading the trace exactly once.
 *
 * Thread pool size equals n_configs — every configuration runs concurrently.
 * No num_threads parameter; num_threads is not a user-visible setting.
 *
 * Caches must be pre-created by the caller (admission/prefetch wrappers already
 * attached).  The function sets free_cache_when_finish = true and frees each
 * cache after its worker finishes.
 *
 * @param reader      trace reader (read once on the calling thread)
 * @param global_cfg  global settings (queue_depth, etc.)
 * @param configs     per-config settings (warmup_sec, warmup_frac, etc.)
 * @param caches      pre-created cache_t* array, length n_configs
 * @param n_configs   number of configurations / caches
 * @return heap-allocated array of cache_stat_t (caller must free)
 */
cache_stat_t *simulate_with_config_list(
    reader_t *reader, sim_global_config_t *global_cfg,
    sim_config_t *configs, cache_t **caches, int n_configs,
    const stream_id_t *config_streams) {
  assert(n_configs > 0);

  int queue_depth =
      (global_cfg->queue_depth > 0) ? global_cfg->queue_depth
                                     : BQUEUE_DEFAULT_DEPTH;

  cache_stat_t *result = my_malloc_n(cache_stat_t, n_configs);
  memset(result, 0, sizeof(cache_stat_t) * n_configs);

  /* Allocate per-simulator bounded queues */
  bounded_queue_t **queues = my_malloc_n(bounded_queue_t *, n_configs);
  for (int i = 0; i < n_configs; i++) {
    queues[i]            = bqueue_create(queue_depth);
    result[i].cache_size = caches[i]->cache_size;
  }

  /* Compute total requests once — needed for warmup_frac calculations.
   * get_num_of_req caches the count and resets the reader position. */
  uint64_t total_req = 0;
  for (int i = 0; i < n_configs; i++) {
    if (configs[i].warmup_frac > 1e-6) {
      total_req = (uint64_t)get_num_of_req(reader);
      break;
    }
  }

  /* Progress tracking */
  int    progress = 0;
  GMutex progress_mtx;
  g_mutex_init(&progress_mtx);

  /* Thread pool size = n_configs (all configs run concurrently) */
  GThreadPool *pool = g_thread_pool_new(
      (GFunc)_simulate_from_queue, NULL, n_configs, TRUE, NULL);
  ASSERT_NOT_NULL(pool,
                  "cannot create thread pool in simulate_with_config_list\n");

  for (int i = 0; i < n_configs; i++) {
    /* Compute per-config n_warmup_req from warmup_frac or warmup_sec */
    uint64_t n_warmup_req = 0;
    if (configs[i].warmup_frac > 1e-6) {
      n_warmup_req = (uint64_t)((double)total_req * configs[i].warmup_frac);
    }

    sim_queue_worker_params_t *p = malloc(sizeof(sim_queue_worker_params_t));
    p->queue                  = queues[i];
    p->cache                  = caches[i];
    p->result                 = &result[i];
    p->n_warmup_req           = n_warmup_req;
    p->warmup_sec             = configs[i].warmup_sec;
    p->free_cache_when_finish = true;
    p->use_random_seed        = true;
    p->progress_mtx           = &progress_mtx;
    p->progress               = &progress;
    p->config_stream          = config_streams ? config_streams[i] : 0;
    ASSERT_TRUE(g_thread_pool_push(pool, p, NULL),
                "cannot push worker in simulate_with_config_list\n");
  }

  /* Allocate batch buffer */
  request_t *batch = my_malloc_n(request_t, queue_depth);

  /* Main reader loop — batch-and-barrier pattern */
  while (true) {
    /* Phase 1: Fill batch (zero locks) */
    int       batch_size = 0;
    request_t req;
    while (batch_size < queue_depth) {
      read_one_req(reader, &req);
      if (!req.valid) break;
      batch[batch_size++] = req;
    }

    if (batch_size == 0) break;  /* trace exhausted */

    /* Phase 2: Barrier — wait for all queues to drain */
    for (int i = 0; i < n_configs; i++) {
      sem_wait(&queues[i]->empty_sem);
    }

    /* Phase 3: Push batch to all queues, signal each worker */
    for (int i = 0; i < n_configs; i++) {
      bqueue_push_batch(queues[i], batch, batch_size);
    }

    if (batch_size < queue_depth) break;  /* partial batch = EOF */
  }

  /* Signal EOF: wait for final drain, then close all queues */
  for (int i = 0; i < n_configs; i++) {
    sem_wait(&queues[i]->empty_sem);
    bqueue_close(queues[i]);
  }

  /* Block until all workers finish */
  g_thread_pool_free(pool, FALSE, TRUE);
  g_mutex_clear(&progress_mtx);

  /* Destroy queues and batch buffer */
  for (int i = 0; i < n_configs; i++) {
    bqueue_destroy(queues[i]);
  }
  my_free(sizeof(bounded_queue_t *) * n_configs, queues);
  my_free(sizeof(request_t) * queue_depth, batch);

  return result;
}

#ifdef __cplusplus
}
#endif
