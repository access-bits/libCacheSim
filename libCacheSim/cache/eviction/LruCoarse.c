//
//  LruCoarse — Coarse-grained LRU with per-CPU TLB filtering
//
//  Memory accesses are grouped into sampling periods of size X.  All TLB
//  misses within the same period share a single coarse timestamp.  Pages
//  in the cache are bucketed by their most-recent coarse timestamp.
//
//  Eviction: pick the oldest bucket.  If there are ties (multiple pages
//  with the same most-recent timestamp), break ties by comparing
//  progressively older timestamps from a per-page history ring of depth
//  LRUCOARSE_HISTORY_DEPTH.  If still tied, evict the first candidate.
//
//  A *global* history table (page_id → timestamps) persists across
//  evictions so re-inserted pages retain their access history.
//
//  Runtime parameters (via --eviction-params):
//    sampling-period=N     number of accesses per coarse period (default 1)
//
//  Usage:
//    ./bin/cachesim <trace> mergedTrace lruCoarse <size> 
//        --ignore-obj-size 1 --eviction-params "sampling-period=1000"
//
//  LruCoarse.c
//  libCacheSim
//

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "dataStructure/hashtable/hashtable.h"
#include "libCacheSim/evictionAlgo.h"

/* uthash: already bundled in libCacheSim */
#include "dataStructure/ut/uthash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * Configuration
 * ===================================================================== */
#define LRUCOARSE_HISTORY_DEPTH    16
#define LRUCOARSE_NO_TIMESTAMP     0            /* sentinel = "no access" */
                                                /* current_period starts at 1 so
                                                   0 is never a valid timestamp */

#define LRUCOARSE_TLB_SETS         512   /* must be power of 2 */
#define LRUCOARSE_TLB_WAYS         4
#define LRUCOARSE_TLB_NUM_CPUS     4
#define LRUCOARSE_REPORT_INTERVAL  10000000UL  /* print stats every 10M */

/* =====================================================================
 * Data structures
 * ===================================================================== */

/* ---- TLB slot (same as before) ---- */
typedef struct {
  bool     valid;
  uint64_t page;
  uint64_t last_access_time;
} LruCoarseSlot;

/* ---- Global history entry: page_id → array of coarse timestamps ---- */
typedef struct {
  obj_id_t obj_id;                                     /* key */
  uint64_t timestamps[LRUCOARSE_HISTORY_DEPTH];        /* [0]=newest */
  UT_hash_handle hh;
} LruCoarseHistory;

/* ---- Bucket: one per coarse timestamp that has cached pages ---- */
typedef struct LruCoarseBucket {
  uint64_t timestamp;                    /* key for uthash lookup */
  cache_obj_t *head;                     /* doubly-linked list of pages */
  cache_obj_t *tail;
  int32_t n_pages;
  struct LruCoarseBucket *newer;         /* towards head (newest) */
  struct LruCoarseBucket *older;         /* towards tail (oldest) */
  UT_hash_handle hh;
} LruCoarseBucket;

/* ---- Algorithm parameters ---- */
typedef struct {
  /* Bucket chain: newest ←→ oldest */
  LruCoarseBucket *newest_bucket;
  LruCoarseBucket *oldest_bucket;
  LruCoarseBucket *bucket_map;           /* uthash: timestamp → bucket */

  /* Global history table: page_id → LruCoarseHistory */
  LruCoarseHistory *history_map;

  /* Coarse timing */
  uint64_t current_period;
  uint64_t accesses_in_period;
  uint64_t sampling_period_size;         /* X — runtime param */

  /* Per-CPU TLB simulation */
  LruCoarseSlot tlb[LRUCOARSE_TLB_NUM_CPUS][LRUCOARSE_TLB_SETS][LRUCOARSE_TLB_WAYS];
  uint64_t tlb_global_time;

  /* Stats */
  uint64_t total_accesses;
  uint64_t tlb_hits;
  uint64_t tlb_misses;
  uint64_t tlb_eviction_invalidations;
  uint64_t random_tie_breaks;
  uint64_t last_report_access;
} LruCoarse_params_t;

/* =====================================================================
 * TLB helpers (unchanged)
 * ===================================================================== */
static inline bool lrucoarse_tlb_access(LruCoarse_params_t *params,
                                        uint8_t cpu_id, uint64_t page) {
  uint32_t set_idx = (uint32_t)(page & (LRUCOARSE_TLB_SETS - 1));
  LruCoarseSlot *set = params->tlb[cpu_id][set_idx];

  for (int w = 0; w < LRUCOARSE_TLB_WAYS; w++) {
    if (set[w].valid && set[w].page == page) {
      set[w].last_access_time = params->tlb_global_time++;
      return true;
    }
  }

  int victim = -1;
  for (int w = 0; w < LRUCOARSE_TLB_WAYS; w++) {
    if (!set[w].valid) { victim = w; break; }
  }
  if (victim < 0) {
    victim = 0;
    for (int w = 1; w < LRUCOARSE_TLB_WAYS; w++) {
      if (set[w].last_access_time < set[victim].last_access_time)
        victim = w;
    }
  }
  set[victim].valid = true;
  set[victim].page = page;
  set[victim].last_access_time = params->tlb_global_time++;
  return false;
}

static inline void lrucoarse_tlb_invalidate_all(LruCoarse_params_t *params,
                                                 uint64_t page) {
  uint32_t set_idx = (uint32_t)(page & (LRUCOARSE_TLB_SETS - 1));
  for (int cpu = 0; cpu < LRUCOARSE_TLB_NUM_CPUS; cpu++) {
    LruCoarseSlot *set = params->tlb[cpu][set_idx];
    for (int w = 0; w < LRUCOARSE_TLB_WAYS; w++) {
      if (set[w].valid && set[w].page == page) {
        set[w].valid = false;
        params->tlb_eviction_invalidations++;
        break;
      }
    }
  }
}

/* =====================================================================
 * Bucket helpers
 * ===================================================================== */

/* Get or create bucket for a given timestamp */
static inline LruCoarseBucket *lrucoarse_get_or_create_bucket(
    LruCoarse_params_t *params, uint64_t ts) {
  LruCoarseBucket *b = NULL;
  HASH_FIND(hh, params->bucket_map, &ts, sizeof(ts), b);
  if (b != NULL) return b;

  b = (LruCoarseBucket *)calloc(1, sizeof(LruCoarseBucket));
  b->timestamp = ts;

  /* Insert into uthash */
  HASH_ADD(hh, params->bucket_map, timestamp, sizeof(b->timestamp), b);

  /* Insert into doubly-linked chain in sorted position.
   * Invariant: newest → … → oldest.  New bucket goes at head (newest)
   * because we only ever create a bucket for current_period which is
   * monotonically increasing. */
  b->newer = NULL;
  b->older = params->newest_bucket;
  if (params->newest_bucket != NULL) {
    params->newest_bucket->newer = b;
  }
  params->newest_bucket = b;
  if (params->oldest_bucket == NULL) {
    params->oldest_bucket = b;
  }

  return b;
}

/* Remove a page from its current bucket (using queue.prev/next for chain) */
static inline void lrucoarse_remove_from_bucket(LruCoarse_params_t *params,
                                                 cache_obj_t *obj) {
  LruCoarseBucket *b = (LruCoarseBucket *)obj->LruCoarse.bucket;
  if (b == NULL) return;

  /* Unlink from intra-bucket doubly-linked list (via queue.prev/next) */
  if (obj->queue.prev != NULL) {
    obj->queue.prev->queue.next = obj->queue.next;
  } else {
    b->head = obj->queue.next;
  }
  if (obj->queue.next != NULL) {
    obj->queue.next->queue.prev = obj->queue.prev;
  } else {
    b->tail = obj->queue.prev;
  }
  obj->queue.prev = NULL;
  obj->queue.next = NULL;
  obj->LruCoarse.bucket = NULL;
  b->n_pages--;

  /* If bucket is now empty, remove it from chain and hash */
  if (b->n_pages == 0) {
    if (b->newer != NULL) {
      b->newer->older = b->older;
    } else {
      params->newest_bucket = b->older;
    }
    if (b->older != NULL) {
      b->older->newer = b->newer;
    } else {
      params->oldest_bucket = b->newer;
    }
    HASH_DEL(params->bucket_map, b);
    free(b);
  }
}

/* Add a page to a bucket (prepend to head for O(1)) */
static inline void lrucoarse_add_to_bucket(cache_obj_t *obj,
                                            LruCoarseBucket *b) {
  obj->LruCoarse.bucket = b;
  obj->queue.prev = NULL;
  obj->queue.next = b->head;
  if (b->head != NULL) {
    b->head->queue.prev = obj;
  } else {
    b->tail = obj;
  }
  b->head = obj;
  b->n_pages++;
}

/* =====================================================================
 * History helpers
 * ===================================================================== */

/* Get or create a global history entry for a page */
static inline LruCoarseHistory *lrucoarse_get_or_create_history(
    LruCoarse_params_t *params, obj_id_t obj_id) {
  LruCoarseHistory *h = NULL;
  HASH_FIND(hh, params->history_map, &obj_id, sizeof(obj_id), h);
  if (h != NULL) return h;

  h = (LruCoarseHistory *)malloc(sizeof(LruCoarseHistory));
  h->obj_id = obj_id;
  for (int i = 0; i < LRUCOARSE_HISTORY_DEPTH; i++) {
    h->timestamps[i] = LRUCOARSE_NO_TIMESTAMP;
  }
  HASH_ADD(hh, params->history_map, obj_id, sizeof(h->obj_id), h);
  return h;
}

/* Push a new timestamp into a page's history (shift right, insert at [0]) */
static inline void lrucoarse_history_push(LruCoarseHistory *h, uint64_t ts) {
  /* Don't push if it's the same as current newest — same period */
  if (h->timestamps[0] == ts) return;

  for (int i = LRUCOARSE_HISTORY_DEPTH - 1; i > 0; i--) {
    h->timestamps[i] = h->timestamps[i - 1];
  }
  h->timestamps[0] = ts;
}

/* =====================================================================
 * Eviction: tie-breaking among candidates in oldest bucket
 * ===================================================================== */

/* Find the page to evict from the oldest bucket by comparing history.
 * O(C × H) where C = pages in bucket, H = history depth.  */
static cache_obj_t *lrucoarse_pick_eviction_victim(LruCoarse_params_t *params) {
  LruCoarseBucket *b = params->oldest_bucket;
  if (b == NULL) {
    ERROR("LruCoarse: pick_eviction_victim called with no buckets (cache empty?)\n");
    return NULL;
  }
  if (b->n_pages == 1) return b->head;

  /* Allocate candidate array to avoid corrupting cache_obj->freq field */
  cache_obj_t **candidates = (cache_obj_t **)malloc(b->n_pages * sizeof(cache_obj_t*));
  if (candidates == NULL) {
    ERROR("LruCoarse: malloc failed for %d candidates, falling back to head\n",
          b->n_pages);
    return b->head;
  }

  /* Build initial candidate list (all pages in bucket) */
  int n_candidates = 0;
  for (cache_obj_t *cur = b->head; cur != NULL; cur = cur->queue.next) {
    candidates[n_candidates++] = cur;
  }
  DEBUG_ASSERT(n_candidates == b->n_pages);

  /* For each history depth, filter candidates by minimum timestamp.
   * Since LRUCOARSE_NO_TIMESTAMP = 0 and real timestamps start at 1,
   * pages with no history naturally have the smallest value and get
   * evicted first. */
  for (int d = 1; d < LRUCOARSE_HISTORY_DEPTH; d++) {
    if (n_candidates == 0) {
      ERROR("LruCoarse: no candidates remaining at depth %d (shouldn't happen)\n", d);
      free(candidates);
      return b->head;
    }
    if (n_candidates == 1) {
      cache_obj_t *victim = candidates[0];
      free(candidates);
      return victim;
    }

    /* Find minimum timestamp at this depth among current candidates */
    uint64_t min_ts = UINT64_MAX;
    for (int i = 0; i < n_candidates; i++) {
      LruCoarseHistory *h = NULL;
      HASH_FIND(hh, params->history_map, &candidates[i]->obj_id,
                sizeof(candidates[i]->obj_id), h);
      uint64_t ts = (h != NULL) ? h->timestamps[d] : LRUCOARSE_NO_TIMESTAMP;
      if (ts < min_ts) {
        min_ts = ts;
      }
    }

    /* Filter: keep only candidates with min_ts at this depth (in-place) */
    int write_idx = 0;
    for (int i = 0; i < n_candidates; i++) {
      LruCoarseHistory *h = NULL;
      HASH_FIND(hh, params->history_map, &candidates[i]->obj_id,
                sizeof(candidates[i]->obj_id), h);
      uint64_t ts = (h != NULL) ? h->timestamps[d] : LRUCOARSE_NO_TIMESTAMP;
      if (ts == min_ts) {
        candidates[write_idx++] = candidates[i];
      }
    }
    n_candidates = write_idx;
  }

  /* Return a surviving candidate. If multiple remain after all depths,
   * break the tie randomly and count the event. */
  cache_obj_t *victim;
  if (n_candidates > 0) {
    if (n_candidates > 1) {
      params->random_tie_breaks++;
      victim = candidates[rand() % n_candidates];
    } else {
      victim = candidates[0];
    }
  } else {
    ERROR("LruCoarse: eviction tie-breaking failed after all %d depths, "
          "falling back to bucket head (bucket has %d pages)\n",
          LRUCOARSE_HISTORY_DEPTH, b->n_pages);
    victim = b->head;
  }

  free(candidates);
  return victim;
}

/* =====================================================================
 * Forward declarations
 * ===================================================================== */
static void LruCoarse_free(cache_t *cache);
static bool LruCoarse_get(cache_t *cache, const request_t *req);
static cache_obj_t *LruCoarse_find(cache_t *cache, const request_t *req,
                                    bool update_cache);
static cache_obj_t *LruCoarse_insert(cache_t *cache, const request_t *req);
static cache_obj_t *LruCoarse_to_evict(cache_t *cache, const request_t *req);
static void LruCoarse_evict(cache_t *cache, const request_t *req);
static bool LruCoarse_remove(cache_t *cache, obj_id_t obj_id);

/* =====================================================================
 * Parameter parsing
 * ===================================================================== */
static void LruCoarse_parse_params(const char *cache_specific_params,
                                    LruCoarse_params_t *params) {
  /* defaults */
  params->sampling_period_size = 1;

  if (cache_specific_params == NULL || cache_specific_params[0] == '\0')
    return;

  char *params_str = strdup(cache_specific_params);
  char *p = params_str;
  while (p != NULL && p[0] != '\0') {
    char *key = strsep(&p, "=");
    char *value = strsep(&p, ",");
    while (p != NULL && *p == ' ') p++;

    if (strcasecmp(key, "sampling-period") == 0) {
      params->sampling_period_size = (uint64_t)strtoull(value, NULL, 10);
      if (params->sampling_period_size == 0) params->sampling_period_size = 1;
    } else {
      ERROR("LruCoarse: unknown param \"%s\"\n", key);
      abort();
    }
  }
  free(params_str);
}

/* =====================================================================
 * Init / Free
 * ===================================================================== */
cache_t *LruCoarse_init(const common_cache_params_t ccache_params,
                         const char *cache_specific_params) {
  cache_t *cache =
      cache_struct_init("LruCoarse", ccache_params, cache_specific_params);
  cache->cache_init = LruCoarse_init;
  cache->cache_free = LruCoarse_free;
  cache->get = LruCoarse_get;
  cache->find = LruCoarse_find;
  cache->insert = LruCoarse_insert;
  cache->evict = LruCoarse_evict;
  cache->remove = LruCoarse_remove;
  cache->to_evict = LruCoarse_to_evict;
  cache->get_occupied_byte = cache_get_occupied_byte_default;
  cache->can_insert = cache_can_insert_default;
  cache->get_n_obj = cache_get_n_obj_default;

  if (ccache_params.consider_obj_metadata) {
    cache->obj_md_size = 8 * 2;
  } else {
    cache->obj_md_size = 0;
  }

  LruCoarse_params_t *params = calloc(1, sizeof(LruCoarse_params_t));
  LruCoarse_parse_params(cache_specific_params, params);

  params->newest_bucket = NULL;
  params->oldest_bucket = NULL;
  params->bucket_map = NULL;
  params->history_map = NULL;
  params->current_period = 1;   /* start at 1 so 0 = LRUCOARSE_NO_TIMESTAMP */
  params->accesses_in_period = 0;

  printf("LruCoarse: initialized — sampling_period=%lu, history_depth=%d, "
         "%d CPUs, %d TLB sets x %d ways\n",
         (unsigned long)params->sampling_period_size,
         LRUCOARSE_HISTORY_DEPTH,
         LRUCOARSE_TLB_NUM_CPUS, LRUCOARSE_TLB_SETS, LRUCOARSE_TLB_WAYS);

  cache->eviction_params = params;
  return cache;
}

static void LruCoarse_free(cache_t *cache) {
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;

  printf("=== LruCoarse Statistics ===\n");
  printf("Sampling period: %lu\n", (unsigned long)params->sampling_period_size);
  printf("Num CPUs:        %d\n", LRUCOARSE_TLB_NUM_CPUS);
  printf("Total accesses:  %lu\n", (unsigned long)params->total_accesses);
  printf("TLB hits:        %lu (%.2f%%)\n", (unsigned long)params->tlb_hits,
         params->total_accesses > 0
             ? 100.0 * params->tlb_hits / params->total_accesses : 0.0);
  printf("TLB misses:      %lu (%.2f%%)\n", (unsigned long)params->tlb_misses,
         params->total_accesses > 0
             ? 100.0 * params->tlb_misses / params->total_accesses : 0.0);
  printf("TLB evict invals:%lu\n",
         (unsigned long)params->tlb_eviction_invalidations);
    printf("Random tie breaks:%lu\n",
      (unsigned long)params->random_tie_breaks);

  /* Count history entries */
  unsigned long hist_count = HASH_COUNT(params->history_map);
  printf("History entries: %lu (~%lu MB)\n", hist_count,
         hist_count * sizeof(LruCoarseHistory) / (1024 * 1024));
  printf("============================\n");

  /* Free history table */
  LruCoarseHistory *h, *htmp;
  HASH_ITER(hh, params->history_map, h, htmp) {
    HASH_DEL(params->history_map, h);
    free(h);
  }

  /* Free bucket chain (pages already freed by cache framework) */
  LruCoarseBucket *b, *btmp;
  HASH_ITER(hh, params->bucket_map, b, btmp) {
    HASH_DEL(params->bucket_map, b);
    free(b);
  }

  free(params);
  cache_struct_free(cache);
}

/* =====================================================================
 * Core algorithm
 * ===================================================================== */

static bool LruCoarse_get(cache_t *cache, const request_t *req) {
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;

  cache->n_req += 1;

  /* ---- advance coarse clock ---- */
  params->total_accesses++;
  params->accesses_in_period++;
  if (params->accesses_in_period >= params->sampling_period_size) {
    params->current_period++;
    params->accesses_in_period = 0;
  }

  /* ---- TLB simulation ---- */
  uint8_t cpu = req->cpu_id;
  if (cpu >= LRUCOARSE_TLB_NUM_CPUS) {
    ERROR("LruCoarse: cpu_id %u >= LRUCOARSE_TLB_NUM_CPUS %d\n",
          cpu, LRUCOARSE_TLB_NUM_CPUS);
    abort();
  }
  bool tlb_hit = lrucoarse_tlb_access(params, cpu, (uint64_t)req->obj_id);
  if (tlb_hit) {
    params->tlb_hits++;
  } else {
    params->tlb_misses++;
  }

  /* ---- Periodic report ---- */
  if (params->total_accesses - params->last_report_access >=
      LRUCOARSE_REPORT_INTERVAL) {
    params->last_report_access = params->total_accesses;
    printf("[LruCoarse @ %luM] period=%lu accesses=%lu tlb_hits=%lu "
          "tlb_misses=%lu random_ties=%lu cache_n_obj=%ld\n",
           (unsigned long)(params->total_accesses / 1000000),
           (unsigned long)params->current_period,
           (unsigned long)params->total_accesses,
           (unsigned long)params->tlb_hits,
           (unsigned long)params->tlb_misses,
          (unsigned long)params->random_tie_breaks,
           (long)cache->n_obj);
  }

  /* ---- On TLB hit: just check cache (no promotion / no history update) ---- */
  if (tlb_hit) {
    cache_obj_t *obj = cache->find(cache, req, false);
    return (obj != NULL);
  }

  /* ---- On TLB miss: update history + promote in bucket structure ---- */
  uint64_t T = params->current_period;

  /* Update global history regardless of cache hit/miss */
  LruCoarseHistory *hist =
      lrucoarse_get_or_create_history(params, req->obj_id);
  lrucoarse_history_push(hist, T);

  cache_obj_t *obj = cache_find_base(cache, req, false);

  if (obj != NULL) {
    /* Cache hit on TLB miss — move page to bucket T */
    LruCoarseBucket *cur_bucket = (LruCoarseBucket *)obj->LruCoarse.bucket;
    if (cur_bucket == NULL || cur_bucket->timestamp != T) {
      lrucoarse_remove_from_bucket(params, obj);
      LruCoarseBucket *new_bucket =
          lrucoarse_get_or_create_bucket(params, T);
      lrucoarse_add_to_bucket(obj, new_bucket);
    }
    return true;
  }

  /* Cache miss — evict if needed, then insert */
  while (cache->get_occupied_byte(cache) + req->obj_size + cache->obj_md_size >
         cache->cache_size) {
    cache->evict(cache, req);
  }

  cache->insert(cache, req);
  return false;
}

static cache_obj_t *LruCoarse_find(cache_t *cache, const request_t *req,
                                    bool update_cache) {
  /* find is only called from LruCoarse_get with update_cache=false
   * for TLB hits (read-only lookup). We don't use the default
   * cache_find_base promotion logic — we handle it ourselves in get(). */
  return cache_find_base(cache, req, update_cache);
}

static cache_obj_t *LruCoarse_insert(cache_t *cache, const request_t *req) {
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;

  cache_obj_t *obj = cache_insert_base(cache, req);

  uint64_t T = params->current_period;
  LruCoarseBucket *b = lrucoarse_get_or_create_bucket(params, T);
  lrucoarse_add_to_bucket(obj, b);

  return obj;
}

static cache_obj_t *LruCoarse_to_evict(cache_t *cache, const request_t *req) {
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;
  return lrucoarse_pick_eviction_victim(params);
}

static void LruCoarse_evict(cache_t *cache, const request_t *req) {
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;
  cache_obj_t *victim = lrucoarse_pick_eviction_victim(params);
  DEBUG_ASSERT(victim != NULL);

  /* Invalidate evicted page from ALL per-CPU TLBs */
  lrucoarse_tlb_invalidate_all(params, (uint64_t)victim->obj_id);

  /* Remove from bucket */
  lrucoarse_remove_from_bucket(params, victim);

  /* NOTE: we do NOT delete the history entry — it persists so that if
   * this page is re-inserted later it retains its access history. */

#if defined(TRACK_DEMOTION)
  if (cache->track_demotion)
    printf("%ld demote %ld %ld\n", cache->n_req, victim->create_time,
           victim->next_access_vtime);
#endif

  cache_evict_base(cache, victim, true);
}

static bool LruCoarse_remove(cache_t *cache, obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) return false;

  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;
  lrucoarse_remove_from_bucket(params, obj);
  cache_remove_obj_base(cache, obj, true);
  return true;
}

#ifdef __cplusplus
}
#endif
