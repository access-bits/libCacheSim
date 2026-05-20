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

#include "libCacheSim/simulator.h"

#include <math.h>
#include <semaphore.h>

#include "../cache/cacheUtils.h"
#include "../utils/include/myprint.h"
#include "../utils/include/mystr.h"
#include "libCacheSim/evictionAlgo.h"
#include "libCacheSim/plugin.h"

typedef struct simulator_multithreading_params {
  reader_t *reader;
  reader_t **readers;
  ssize_t n_caches;
  cache_t **caches;
  uint64_t n_warmup_req; /* num of requests used for warming up cache */
  reader_t *warmup_reader;
  int warmup_sec; /* num of seconds of requests used for warming up cache */
  cache_stat_t *result;
  GMutex mtx; /* prevent simultaneous write to progress */
  gint *progress;
  gpointer other_data;
  bool free_cache_when_finish;
  bool use_random_seed;
} sim_mt_params_t;

static void _simulate(gpointer data, gpointer user_data) {
  sim_mt_params_t *params = (sim_mt_params_t *)user_data;
  int idx = GPOINTER_TO_UINT(data) - 1;
  if (params->use_random_seed) {
    set_rand_seed(rand());
  } else {
    set_rand_seed(1);
  }

  cache_stat_t *result = params->result;
  /* If an array of readers is provided, use the one corresponding to this
   * cache; otherwise, use the single reader */
  reader_t *source_reader =
      params->readers ? params->readers[idx] : params->reader;
  reader_t *cloned_reader = clone_reader(source_reader);
  request_t *req = new_request();
  cache_t *local_cache = params->caches[idx];
  strncpy(result[idx].cache_name, local_cache->cache_name,
          CACHE_NAME_ARRAY_LEN);

  /* warm up using warmup_reader */
  if (params->warmup_reader) {
    reader_t *warmup_cloned_reader = clone_reader(params->warmup_reader);
    read_one_req(warmup_cloned_reader, req);
    while (req->valid) {
      local_cache->get(local_cache, req);
      result[idx].n_warmup_req += 1;
      read_one_req(warmup_cloned_reader, req);
    }
    close_reader(warmup_cloned_reader);
    INFO("cache %s (size %" PRIu64
         ") finishes warm up using warmup reader "
         "with %" PRIu64 " requests\n",
         local_cache->cache_name, local_cache->cache_size,
         result[idx].n_warmup_req);
  }

  read_one_req(cloned_reader, req);
  int64_t start_ts = (int64_t)req->clock_time;

  /* using warmup_frac or warmup_sec of requests from reader to warm up */
  if (params->n_warmup_req > 0 || params->warmup_sec > 0) {
    uint64_t n_warmup = 0;
    while (req->valid && (n_warmup < params->n_warmup_req ||
                          req->clock_time - start_ts < params->warmup_sec)) {
      req->clock_time -= start_ts;
      local_cache->get(local_cache, req);
      n_warmup += 1;
      read_one_req(cloned_reader, req);
    }
    result[idx].n_warmup_req += n_warmup;
    INFO("cache %s (size %" PRIu64
         ") finishes warm up using "
         "with %" PRIu64 " requests, %.2lf hour trace time\n",
         local_cache->cache_name, local_cache->cache_size, n_warmup,
         (double)(req->clock_time - start_ts) / 3600.0);
  }

  while (req->valid) {
    result[idx].n_req++;
    result[idx].n_req_byte += req->obj_size;
    result[idx].n_req_cost += req->obj_cost;

    req->clock_time -= start_ts;
    if (local_cache->get(local_cache, req) == false) {
      result[idx].n_miss++;
      result[idx].n_miss_byte += req->obj_size;
      result[idx].n_miss_cost += req->obj_cost;
    }
    read_one_req(cloned_reader, req);
  }

/* disabled due to ARC and LeCaR use ghost entries in the hash table */
#if defined(SUPPORT_TTL) && defined(ENABLE_SCAN)
  /* get expiration information */
  if (local_cache->hashtable->n_obj != 0) {
    cache_stat_t temp_stat;
    memset(&temp_stat, 0, sizeof(cache_stat_t));
    temp_stat.curr_rtime = req->clock_time;
    get_cache_state(local_cache, &temp_stat);

    if (local_cache->occupied_size != temp_stat.occupied_size) {
      WARN(
          "occupied_size not match, %ld vs %ld, maybe the "
          "cache uses a ghost list, in which case, the expired "
          "object count may not be accurate",
          local_cache->occupied_size, temp_stat.occupied_size);
    }
    result[idx].expired_obj_cnt = temp_stat.expired_obj_cnt;
    result[idx].expired_bytes = temp_stat.expired_bytes;
  }
#endif

  result[idx].curr_rtime = req->clock_time;
  result[idx].n_obj = local_cache->n_obj;
  result[idx].occupied_byte = local_cache->occupied_byte;

  // report progress
  g_mutex_lock(&(params->mtx));
  (*(params->progress))++;
  g_mutex_unlock(&(params->mtx));

  // clean up
  if (params->free_cache_when_finish) {
    local_cache->cache_free(local_cache);
  }
  free_request(req);
  close_reader(cloned_reader);
}

cache_stat_t *simulate_at_multi_sizes_with_step_size(
    reader_t *const reader, const cache_t *cache, uint64_t step_size,
    reader_t *warmup_reader, double warmup_frac, int warmup_sec,
    int num_of_threads, bool use_random_seed) {
  int num_of_sizes = (int)ceil((double)cache->cache_size / (double)step_size);
  get_num_of_req(reader);
  uint64_t *cache_sizes = my_malloc_n(uint64_t, num_of_sizes);
  for (int i = 0; i < num_of_sizes; i++) {
    cache_sizes[i] = step_size * (i + 1);
  }

  cache_stat_t *res = simulate_at_multi_sizes(
      reader, cache, num_of_sizes, cache_sizes, warmup_reader, warmup_frac,
      warmup_sec, num_of_threads, use_random_seed);
  my_free(sizeof(uint64_t) * num_of_sizes, cache_sizes);
  return res;
}

/**
 * @brief get miss ratio curve for different cache sizes
 *
 * @param reader
 * @param cache
 * @param num_of_sizes
 * @param cache_sizes
 * @param warmup_reader if not NULL, read from warmup_reader to warm up cache
 * @param warmup_frac use warmup_frac of requests from reader to warm up cache
 * @param warmup_sec uses warmup_sec seconds of requests to warm up cache
 * @param num_of_threads
 *
 * note that warmup_reader, warmup_frac and warmup_sec are mutually exclusive
 *
 */
cache_stat_t *simulate_at_multi_sizes(
    reader_t *reader, const cache_t *cache, int num_of_sizes,
    const uint64_t *cache_sizes, reader_t *warmup_reader, double warmup_frac,
    int warmup_sec, int num_of_threads, bool use_random_seed) {
  int progress = 0;

  cache_stat_t *result = my_malloc_n(cache_stat_t, num_of_sizes);
  memset(result, 0, sizeof(cache_stat_t) * num_of_sizes);

  // build parameters and send to thread pool
  sim_mt_params_t *params = my_malloc(sim_mt_params_t);
  params->reader = reader;
  params->readers = NULL;
  params->warmup_reader = warmup_reader;
  params->warmup_sec = warmup_sec;
  params->n_caches = num_of_sizes;
  params->n_warmup_req =
      (uint64_t)((double)get_num_of_req(reader) * warmup_frac);
  params->result = result;
  params->free_cache_when_finish = true;
  params->progress = &progress;
  params->use_random_seed = use_random_seed;
  g_mutex_init(&(params->mtx));

  // build the thread pool
  GThreadPool *gthread_pool = g_thread_pool_new(
      (GFunc)_simulate, (gpointer)params, num_of_threads, TRUE, NULL);
  ASSERT_NOT_NULL(gthread_pool, "cannot create thread pool in simulator\n");

  // start computation
  params->caches = my_malloc_n(cache_t *, num_of_sizes);
  for (int i = 1; i < num_of_sizes + 1; i++) {
    params->caches[i - 1] =
        create_cache_with_new_size(cache, cache_sizes[i - 1]);
    result[i - 1].cache_size = cache_sizes[i - 1];
    ASSERT_TRUE(g_thread_pool_push(gthread_pool, GSIZE_TO_POINTER(i), NULL),
                "cannot push data into thread_pool in get_miss_ratio\n");
  }

  char start_cache_size[64], end_cache_size[64];
  convert_size_to_str(cache_sizes[0], start_cache_size, 64);
  convert_size_to_str(cache_sizes[num_of_sizes - 1], end_cache_size, 64);

  INFO(
      "%s starts computation %s, num_warmup_req %lld, start cache size %s, "
      "end cache size %s, %d sizes, %d threads, please wait\n",
      __func__, cache->cache_name, (long long)(params->n_warmup_req),
      start_cache_size, end_cache_size, num_of_sizes, num_of_threads);

  // wait for all simulations to finish
  while (progress < num_of_sizes - 1) {
    print_progress((double)progress / (double)(num_of_sizes - 1) * 100);
  }

  // clean up
  g_thread_pool_free(gthread_pool, FALSE, TRUE);
  g_mutex_clear(&(params->mtx));
  my_free(sizeof(cache_t *) * num_of_sizes, params->caches);
  my_free(sizeof(sim_mt_params_t), params);

  // user is responsible for free-ing the result
  return result;
}

/**
 * @brief run multiple simulations in parallel
 *
 * @param reader
 * @param caches
 * @param num_of_caches
 * @param warmup_reader
 * @param warmup_frac
 * @param warmup_sec
 * @param num_of_threads
 * @return cache_stat_t*
 */
cache_stat_t *simulate_with_multi_caches(
    reader_t *reader, cache_t *caches[], int num_of_caches,
    reader_t *warmup_reader, double warmup_frac, int warmup_sec,
    int num_of_threads, bool free_cache_when_finish, bool use_random_seed) {
  assert(num_of_caches > 0);
  int i, progress = 0;

  cache_stat_t *result = my_malloc_n(cache_stat_t, num_of_caches);
  memset(result, 0, sizeof(cache_stat_t) * num_of_caches);

  // build parameters and send to thread pool
  sim_mt_params_t *params = my_malloc(sim_mt_params_t);
  params->reader = reader;
  params->readers = NULL;
  params->caches = caches;
  params->warmup_reader = warmup_reader;
  params->warmup_sec = warmup_sec;
  params->use_random_seed = use_random_seed;
  if (warmup_frac > 1e-6) {
    params->n_warmup_req =
        (uint64_t)((double)get_num_of_req(reader) * warmup_frac);
  } else {
    params->n_warmup_req = 0;
  }
  params->result = result;
  params->free_cache_when_finish = free_cache_when_finish;
  params->progress = &progress;
  g_mutex_init(&(params->mtx));

  // build the thread pool
  GThreadPool *gthread_pool = g_thread_pool_new(
      (GFunc)_simulate, (gpointer)params, num_of_threads, TRUE, NULL);
  ASSERT_NOT_NULL(gthread_pool, "cannot create thread pool in simulator\n");

  // start computation
  for (i = 1; i < num_of_caches + 1; i++) {
    result[i - 1].cache_size = caches[i - 1]->cache_size;

    ASSERT_TRUE(g_thread_pool_push(gthread_pool, GSIZE_TO_POINTER(i), NULL),
                "cannot push data into thread_pool in get_miss_ratio\n");
  }

  char start_cache_size[64], end_cache_size[64];
  convert_size_to_str(result[0].cache_size, start_cache_size, 64);
  convert_size_to_str(result[num_of_caches - 1].cache_size, end_cache_size, 64);

  INFO(
      "%s starts computation, num_warmup_req %lld, start cache %s size %s, "
      "end cache %s size %s, %d caches, %d threads, please wait\n",
      __func__, (long long)(params->n_warmup_req), caches[0]->cache_name,
      start_cache_size, caches[num_of_caches - 1]->cache_name, end_cache_size,
      num_of_caches, num_of_threads);

  // wait for all simulations to finish
  while (progress < num_of_caches - 1) {
    print_progress((double)progress / (double)(num_of_caches - 1) * 100);
  }

  // clean up
  g_thread_pool_free(gthread_pool, FALSE, TRUE);
  g_mutex_clear(&(params->mtx));
  my_free(sizeof(sim_mt_params_t), params);

  // user is responsible for free-ing the result
  return result;
}

cache_stat_t *simulate_with_multi_caches_scaling(
    reader_t **readers, cache_t *caches[], int num_of_caches,
    reader_t *warmup_reader, double warmup_frac, int warmup_sec,
    int num_of_threads, bool free_cache_when_finish) {
  int progress = 0;

  cache_stat_t *result = my_malloc_n(cache_stat_t, num_of_caches);
  memset(result, 0, sizeof(cache_stat_t) * num_of_caches);

  sim_mt_params_t *params = my_malloc(sim_mt_params_t);
  params->readers = readers;  // use multi-readers for scaling
  params->reader = NULL;      // not used in scaling mode
  params->caches = caches;
  params->warmup_reader = warmup_reader;
  params->warmup_sec = warmup_sec;
  params->use_random_seed = false;  // or set as desired
  if (warmup_frac > 1e-6) {
    params->n_warmup_req =
        (uint64_t)((double)get_num_of_req(readers[0]) * warmup_frac);
  } else {
    params->n_warmup_req = 0;
  }
  params->result = result;
  params->free_cache_when_finish = free_cache_when_finish;
  params->progress = &progress;
  g_mutex_init(&(params->mtx));

  GThreadPool *gthread_pool = g_thread_pool_new(
      (GFunc)_simulate, (gpointer)params, num_of_threads, TRUE, NULL);
  ASSERT_NOT_NULL(gthread_pool, "cannot create thread pool in simulator\n");

  for (int i = 1; i < num_of_caches + 1; i++) {
    result[i - 1].cache_size = caches[i - 1]->cache_size;
    ASSERT_TRUE(g_thread_pool_push(gthread_pool, GSIZE_TO_POINTER(i), NULL),
                "cannot push data into thread_pool in get_miss_ratio\n");
  }

  char start_cache_size[64], end_cache_size[64];
  convert_size_to_str(result[0].cache_size, start_cache_size, 64);
  convert_size_to_str(result[num_of_caches - 1].cache_size, end_cache_size, 64);

  INFO(
      "simulate_with_multi_caches_scaling starts computation, num_warmup_req "
      "%lld, start cache size %s, end cache size "
      "%s, %d caches, %d threads, please wait\n",
      (long long)params->n_warmup_req, start_cache_size, end_cache_size,
      num_of_caches, num_of_threads);

  while (progress < num_of_caches - 1) {
    print_progress((double)progress / (double)(num_of_caches - 1) * 100);
  }

  g_thread_pool_free(gthread_pool, FALSE, TRUE);
  g_mutex_clear(&(params->mtx));
  my_free(sizeof(sim_mt_params_t), params);
  for (int i = 0; i < num_of_caches; i++) {
    result[i].sampler_ratio = readers[i]->sampler->sampling_ratio;
  }
  return result;
}


/* ================================================================
 * Bounded ring-buffer queue — used by simulate_with_single_reader
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
                                 int64_t *start_ts, uint64_t n_warmup_from_wr,
                                 uint64_t n_warmup_req, int warmup_sec) {
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

    /* Phase 1 — warmup_reader requests: always warmup, raw timestamps */
    if (*consumed <= n_warmup_from_wr) {
      cache->get(cache, req);
      result->n_warmup_req++;
      continue;
    }

    /* Record time base on the first main-reader request */
    if (*start_ts == INT64_MIN) {
      *start_ts = req->clock_time;
    }
    req->clock_time -= *start_ts;

    /* Phase 2 — count/time-based warmup from main reader */
    uint64_t main_idx = *consumed - n_warmup_from_wr; /* 1-based */
    if (main_idx <= n_warmup_req ||
        (warmup_sec > 0 && req->clock_time < (int64_t)warmup_sec)) {
      cache->get(cache, req);
      result->n_warmup_req++;
      continue;
    }

    /* Phase 3 — measured simulation */
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
  uint64_t         n_warmup_from_wr; /* items from warmup_reader (always warmup) */
  uint64_t         n_warmup_req;     /* additional count-based warmup from main reader */
  int              warmup_sec;       /* time-based warmup from main reader */
  bool             free_cache_when_finish;
  bool             use_random_seed;
  GMutex          *progress_mtx;
  gint            *progress;
} sim_queue_worker_params_t;

static void _simulate_from_queue(gpointer data, gpointer user_data) {
  (void)user_data;
  sim_queue_worker_params_t *p = (sim_queue_worker_params_t *)data;

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
        p->n_warmup_from_wr, p->n_warmup_req, p->warmup_sec);
    
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
 * @brief Simulate multiple caches reading the trace exactly once.
 *
 * Uses batch-and-barrier pattern: reader accumulates requests in local buffer
 * (up to queue_depth), waits for all workers to drain their queues, then
 * bulk-pushes entire batch to all queues. Workers process batches in parallel.
 * Memory bounded at O(num_of_caches * queue_depth * sizeof(request_t)).
 *
 * warmup_reader, warmup_frac and warmup_sec are mutually exclusive.
 *
 * @param reader                  trace reader (read once on calling thread)
 * @param caches                  array of initialised cache_t* to simulate
 * @param num_of_caches           length of caches[]
 * @param warmup_reader           optional separate warmup trace (may be NULL)
 * @param warmup_frac             fraction of main-trace requests used as warmup
 * @param warmup_sec              seconds of trace time used as warmup
 * @param num_of_threads          worker thread pool size
 * @param queue_depth             per-simulator queue capacity (<=0 → default 1024)
 * @param free_cache_when_finish  if true each cache is freed when its worker finishes
 * @param use_random_seed         if true each worker seeds RNG with rand()
 * @return heap-allocated array of cache_stat_t; caller must free
 */
cache_stat_t *simulate_with_single_reader(
    reader_t *reader, cache_t *caches[], int num_of_caches,
    reader_t *warmup_reader, double warmup_frac, int warmup_sec,
    int num_of_threads, int queue_depth, bool free_cache_when_finish,
    bool use_random_seed) {
  assert(num_of_caches > 0);
  if (queue_depth <= 0) queue_depth = BQUEUE_DEFAULT_DEPTH;

  cache_stat_t *result = my_malloc_n(cache_stat_t, num_of_caches);
  memset(result, 0, sizeof(cache_stat_t) * num_of_caches);

  /* Allocate per-simulator bounded queues */
  bounded_queue_t **queues = my_malloc_n(bounded_queue_t *, num_of_caches);
  for (int i = 0; i < num_of_caches; i++) {
    queues[i]            = bqueue_create(queue_depth);
    result[i].cache_size = caches[i]->cache_size;
  }

  /* Count main-reader requests for warmup_frac (does not advance reader) */
  uint64_t n_warmup_req = 0;
  if (warmup_frac > 1e-6) {
    n_warmup_req = (uint64_t)((double)get_num_of_req(reader) * warmup_frac);
  }

  /* Count warmup_reader items so workers know the warmup boundary */
  uint64_t n_warmup_from_wr = 0;
  if (warmup_reader != NULL) {
    n_warmup_from_wr = (uint64_t)get_num_of_req(warmup_reader);
  }

  /* Progress tracking */
  int    progress = 0;
  GMutex progress_mtx;
  g_mutex_init(&progress_mtx);

  /* Spawn worker threads */
  GThreadPool *pool = g_thread_pool_new(
      (GFunc)_simulate_from_queue, NULL, num_of_threads, TRUE, NULL);
  ASSERT_NOT_NULL(pool,
                  "cannot create thread pool in simulate_with_single_reader\n");

  for (int i = 0; i < num_of_caches; i++) {
    sim_queue_worker_params_t *p = malloc(sizeof(sim_queue_worker_params_t));
    p->queue                  = queues[i];
    p->cache                  = caches[i];
    p->result                 = &result[i];
    p->n_warmup_from_wr       = n_warmup_from_wr;
    p->n_warmup_req           = n_warmup_req;
    p->warmup_sec             = warmup_sec;
    p->free_cache_when_finish = free_cache_when_finish;
    p->use_random_seed        = use_random_seed;
    p->progress_mtx           = &progress_mtx;
    p->progress               = &progress;
    ASSERT_TRUE(g_thread_pool_push(pool, p, NULL),
                "cannot push worker in simulate_with_single_reader\n");
  }

  /* Allocate batch buffer for accumulating requests */
  request_t *batch = my_malloc_n(request_t, queue_depth);

  /* Process warmup_reader first (if provided) */
  if (warmup_reader != NULL) {
    reader_t *wr = clone_reader(warmup_reader);
    request_t req;
    
    while (true) {
      /* Fill batch buffer */
      int batch_size = 0;
      while (batch_size < queue_depth) {
        read_one_req(wr, &req);
        if (!req.valid) break;
        batch[batch_size++] = req;
      }
      
      if (batch_size == 0) break;  /* warmup reader exhausted */
      
      /* Wait for all workers to drain (barrier) */
      for (int i = 0; i < num_of_caches; i++) {
        sem_wait(&queues[i]->empty_sem);
      }
      
      /* Push batch to all queues */
      for (int i = 0; i < num_of_caches; i++) {
        bqueue_push_batch(queues[i], batch, batch_size);
      }
      
      if (batch_size < queue_depth) break;  /* partial batch = EOF */
    }
    
    close_reader(wr);
  }

  /* Main reader loop — batch-and-barrier pattern */
  while (true) {
    /* Phase 1: Fill batch buffer (zero locks) */
    int batch_size = 0;
    request_t req;
    while (batch_size < queue_depth) {
      read_one_req(reader, &req);
      if (!req.valid) break;
      batch[batch_size++] = req;
    }
    
    if (batch_size == 0) break;  /* trace exhausted */
    
    /* Phase 2: Wait for all queues to drain (barrier synchronization) */
    for (int i = 0; i < num_of_caches; i++) {
      sem_wait(&queues[i]->empty_sem);
    }
    
    /* Phase 3: Push batch to each queue sequentially, signal each worker */
    for (int i = 0; i < num_of_caches; i++) {
      bqueue_push_batch(queues[i], batch, batch_size);
    }
    
    if (batch_size < queue_depth) break;  /* partial batch = EOF */
  }

  /* Signal EOF: wait for final drain, then close all queues */
  for (int i = 0; i < num_of_caches; i++) {
    sem_wait(&queues[i]->empty_sem);
    bqueue_close(queues[i]);
  }

  /* Block until all workers finish */
  g_thread_pool_free(pool, FALSE, TRUE);
  g_mutex_clear(&progress_mtx);

  /* Destroy queues and batch buffer */
  for (int i = 0; i < num_of_caches; i++) {
    bqueue_destroy(queues[i]);
  }
  my_free(sizeof(bounded_queue_t *) * num_of_caches, queues);
  my_free(sizeof(request_t) * queue_depth, batch);

  return result;
}

/**
 * @brief MRC sweep using simulate_with_single_reader.
 *
 * Equivalent to simulate_at_multi_sizes but reads the trace only once.
 */
cache_stat_t *simulate_at_multi_sizes_single_reader(
    reader_t *reader, const cache_t *cache, int num_of_sizes,
    const uint64_t *cache_sizes, reader_t *warmup_reader, double warmup_frac,
    int warmup_sec, int num_of_threads, int queue_depth, bool use_random_seed) {
  cache_t **sized_caches = my_malloc_n(cache_t *, num_of_sizes);
  for (int i = 0; i < num_of_sizes; i++) {
    sized_caches[i] = create_cache_with_new_size(cache, cache_sizes[i]);
  }

  char start_str[64], end_str[64];
  convert_size_to_str(cache_sizes[0], start_str, 64);
  convert_size_to_str(cache_sizes[num_of_sizes - 1], end_str, 64);
  INFO(
      "%s single-reader MRC, start %s end %s, %d sizes, %d threads, "
      "queue depth %d\n",
      cache->cache_name, start_str, end_str, num_of_sizes, num_of_threads,
      queue_depth > 0 ? queue_depth : BQUEUE_DEFAULT_DEPTH);

  /* simulate_with_single_reader allocates and returns the result array;
   * caches are freed by workers (free_cache_when_finish = true) */
  cache_stat_t *result = simulate_with_single_reader(
      reader, sized_caches, num_of_sizes, warmup_reader, warmup_frac,
      warmup_sec, num_of_threads, queue_depth, true, use_random_seed);

  my_free(sizeof(cache_t *) * num_of_sizes, sized_caches);
  return result;
}

#ifdef __cplusplus
}
#endif
