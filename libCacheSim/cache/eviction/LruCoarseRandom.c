//
//  LruCoarseRandom — Coarse-grained LRU with per-CPU TLB filtering + random eviction
//
//  Same as LruCoarse but simplified eviction:
//    - When oldest bucket has multiple pages, pick one at random
//    - No history-based tie-breaking (O(1) eviction instead of O(C×H))
//
//  Memory accesses are grouped into sampling periods of size X.  All TLB
//  misses within the same period share a single coarse timestamp.  Pages
//  in the cache are bucketed by their most-recent coarse timestamp.
//
//  Runtime parameters (via --eviction-params):
//    sampling-period=N     number of accesses per coarse period (default 1)
//
//  Usage:
//    ./bin/cachesim <trace> mergedTrace lruCoarseRandom <size> 
//        --ignore-obj-size 1 --eviction-params "sampling-period=1000"
//
//  LruCoarseRandom.c
//  libCacheSim
//

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "dataStructure/hashtable/hashtable.h"
#include "libCacheSim/evictionAlgo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * Configuration
 * ===================================================================== */
#define LRUCOARSERANDOM_TLB_SETS         512   /* must be power of 2 */
#define LRUCOARSERANDOM_TLB_WAYS         4
#define LRUCOARSERANDOM_TLB_NUM_CPUS     4
#define LRUCOARSERANDOM_REPORT_INTERVAL  10000000UL  /* print stats every 10M */

/* =====================================================================
 * Data structures
 * ===================================================================== */

/* ---- TLB slot ---- */
typedef struct {
  bool     valid;
  uint64_t page;
  uint64_t last_access_time;
} LruCoarseRandomSlot;

/* ---- Bucket: one per coarse timestamp that has cached pages ---- */
typedef struct LruCoarseRandomBucket {
  uint64_t timestamp;
  cache_obj_t **pages;                   /* dynamic array of page pointers */
  int32_t n_pages;                       /* logical size */
  int32_t capacity;                      /* allocated slots */
  struct LruCoarseRandomBucket *newer;         /* towards head (newest) */
  struct LruCoarseRandomBucket *older;         /* towards tail (oldest) */
} LruCoarseRandomBucket;

/* ---- Algorithm parameters ---- */
typedef struct {
  /* Bucket chain: newest ←→ oldest */
  LruCoarseRandomBucket *newest_bucket;
  LruCoarseRandomBucket *oldest_bucket;

  /* Coarse timing */
  uint64_t current_period;
  uint64_t accesses_in_period;
  uint64_t sampling_period_size;         /* X — runtime param */

  /* Per-CPU TLB simulation */
  LruCoarseRandomSlot tlb[LRUCOARSERANDOM_TLB_NUM_CPUS][LRUCOARSERANDOM_TLB_SETS][LRUCOARSERANDOM_TLB_WAYS];
  uint64_t tlb_global_time;

  /* Stats */
  uint64_t total_accesses;
  uint64_t tlb_hits;
  uint64_t tlb_misses;
  uint64_t tlb_eviction_invalidations;
  uint64_t random_selections;  /* count of random picks from bucket */
  uint64_t last_report_access;

  /* Random seed */
  unsigned int random_seed;
  bool random_seed_set;
} LruCoarseRandom_params_t;

/* =====================================================================
 * TLB helpers
 * ===================================================================== */
static inline bool lrucoarserandom_tlb_access(LruCoarseRandom_params_t *params,
                                        uint8_t cpu_id, uint64_t page) {
  uint32_t set_idx = (uint32_t)(page & (LRUCOARSERANDOM_TLB_SETS - 1));
  LruCoarseRandomSlot *set = params->tlb[cpu_id][set_idx];

  for (int w = 0; w < LRUCOARSERANDOM_TLB_WAYS; w++) {
    if (set[w].valid && set[w].page == page) {
      set[w].last_access_time = params->tlb_global_time++;
      return true;
    }
  }

  int victim = -1;
  for (int w = 0; w < LRUCOARSERANDOM_TLB_WAYS; w++) {
    if (!set[w].valid) { victim = w; break; }
  }
  if (victim < 0) {
    victim = 0;
    for (int w = 1; w < LRUCOARSERANDOM_TLB_WAYS; w++) {
      if (set[w].last_access_time < set[victim].last_access_time)
        victim = w;
    }
  }
  set[victim].valid = true;
  set[victim].page = page;
  set[victim].last_access_time = params->tlb_global_time++;
  return false;
}

static inline void lrucoarserandom_tlb_invalidate_all(LruCoarseRandom_params_t *params,
                                                 uint64_t page) {
  uint32_t set_idx = (uint32_t)(page & (LRUCOARSERANDOM_TLB_SETS - 1));
  for (int cpu = 0; cpu < LRUCOARSERANDOM_TLB_NUM_CPUS; cpu++) {
    LruCoarseRandomSlot *set = params->tlb[cpu][set_idx];
    for (int w = 0; w < LRUCOARSERANDOM_TLB_WAYS; w++) {
      if (set[w].valid && set[w].page == page) {
        set[w].valid = false;
        params->tlb_eviction_invalidations++;
        break;
      }
    }
  }
}

#define LRUCOARSERANDOM_BUCKET_INIT_CAP  64  /* initial capacity for bucket pages array */

/* =====================================================================
 * Bucket helpers
 * ===================================================================== */

/* Get or create bucket for the current (newest) timestamp.
 * Invariant: ts is always current_period, which is monotonically increasing,
 * so the matching bucket (if any) is always newest_bucket. */
static inline LruCoarseRandomBucket *lrucoarserandom_get_or_create_bucket(
    LruCoarseRandom_params_t *params, uint64_t ts) {
  assert(ts == params->current_period);
  if (params->newest_bucket != NULL && params->newest_bucket->timestamp == ts)
    return params->newest_bucket;

  LruCoarseRandomBucket *b = (LruCoarseRandomBucket *)calloc(1, sizeof(LruCoarseRandomBucket));
  b->timestamp = ts;
  b->capacity = LRUCOARSERANDOM_BUCKET_INIT_CAP;
  b->pages = (cache_obj_t **)malloc(sizeof(cache_obj_t *) * b->capacity);

  /* New bucket goes at head (newest). */
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

/* Remove a page from its current bucket (swap-with-last for O(1)) */
static inline void lrucoarserandom_remove_from_bucket(LruCoarseRandom_params_t *params,
                                                 cache_obj_t *obj) {
  LruCoarseRandomBucket *b = (LruCoarseRandomBucket *)obj->LruCoarse.bucket;
  if (b == NULL) return;

  int32_t idx = obj->LruCoarse.bucket_idx;
  int32_t last = b->n_pages - 1;

  /* Swap with last element if not already last */
  if (idx != last) {
    cache_obj_t *moved = b->pages[last];
    b->pages[idx] = moved;
    moved->LruCoarse.bucket_idx = idx;
  }

  obj->LruCoarse.bucket = NULL;
  obj->LruCoarse.bucket_idx = -1;
  b->n_pages--;

  /* If bucket is now empty, remove it from chain and free */
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
    free(b->pages);
    free(b);
  }
}

/* Add a page to a bucket (append to array for O(1) amortized) */
static inline void lrucoarserandom_add_to_bucket(cache_obj_t *obj,
                                            LruCoarseRandomBucket *b) {
  /* Grow array if full */
  if (b->n_pages == b->capacity) {
    b->capacity *= 2;
    b->pages = (cache_obj_t **)realloc(b->pages, sizeof(cache_obj_t *) * b->capacity);
  }

  obj->LruCoarse.bucket = b;
  obj->LruCoarse.bucket_idx = b->n_pages;
  b->pages[b->n_pages] = obj;
  b->n_pages++;
}

/* =====================================================================
 * Eviction: random selection from oldest bucket
 * ===================================================================== */

/* Pick a random page from the oldest bucket. O(1) via direct array access. */
static cache_obj_t *lrucoarserandom_pick_eviction_victim(LruCoarseRandom_params_t *params) {
  LruCoarseRandomBucket *b = params->oldest_bucket;
  if (b == NULL) {
    ERROR("LruCoarseRandom: pick_eviction_victim called with no buckets (cache empty?)\n");
    return NULL;
  }
  if (b->n_pages == 1) return b->pages[0];

  int target_idx = rand() % b->n_pages;
  params->random_selections++;

  return b->pages[target_idx];
}

/* =====================================================================
 * Forward declarations
 * ===================================================================== */
static void LruCoarseRandom_free(cache_t *cache);
static bool LruCoarseRandom_get(cache_t *cache, const request_t *req);
static cache_obj_t *LruCoarseRandom_find(cache_t *cache, const request_t *req,
                                    bool update_cache);
static cache_obj_t *LruCoarseRandom_insert(cache_t *cache, const request_t *req);
static cache_obj_t *LruCoarseRandom_to_evict(cache_t *cache, const request_t *req);
static void LruCoarseRandom_evict(cache_t *cache, const request_t *req);
static bool LruCoarseRandom_remove(cache_t *cache, obj_id_t obj_id);

/* =====================================================================
 * Parameter parsing
 * ===================================================================== */
static void LruCoarseRandom_parse_params(const char *cache_specific_params,
                                    LruCoarseRandom_params_t *params) {
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
    } else if (strcasecmp(key, "random-seed") == 0) {
      params->random_seed = (unsigned int)strtoul(value, NULL, 10);
      params->random_seed_set = true;
    } else {
      ERROR("LruCoarseRandom: unknown param \"%s\"\n", key);
      abort();
    }
  }
  free(params_str);
}

/* =====================================================================
 * Init / Free
 * ===================================================================== */
cache_t *LruCoarseRandom_init(const common_cache_params_t ccache_params,
                         const char *cache_specific_params) {
  cache_t *cache =
      cache_struct_init("LruCoarseRandom", ccache_params, cache_specific_params);
  cache->cache_init = LruCoarseRandom_init;
  cache->cache_free = LruCoarseRandom_free;
  cache->get = LruCoarseRandom_get;
  cache->find = LruCoarseRandom_find;
  cache->insert = LruCoarseRandom_insert;
  cache->evict = LruCoarseRandom_evict;
  cache->remove = LruCoarseRandom_remove;
  cache->to_evict = LruCoarseRandom_to_evict;
  cache->get_occupied_byte = cache_get_occupied_byte_default;
  cache->can_insert = cache_can_insert_default;
  cache->get_n_obj = cache_get_n_obj_default;

  if (ccache_params.consider_obj_metadata) {
    cache->obj_md_size = 8 * 2;
  } else {
    cache->obj_md_size = 0;
  }

  LruCoarseRandom_params_t *params = calloc(1, sizeof(LruCoarseRandom_params_t));
  LruCoarseRandom_parse_params(cache_specific_params, params);

  params->newest_bucket = NULL;
  params->oldest_bucket = NULL;
  params->current_period = 1;   /* start at 1 so 0 = LRUCOARSERANDOM_NO_TIMESTAMP */
  params->accesses_in_period = 0;

  if (params->random_seed_set) {
    srand(params->random_seed);
  } else {
    unsigned int seed = (unsigned int)(time(NULL) ^ getpid());
    srand(seed);
    params->random_seed = seed;
  }

  printf("LruCoarseRandom: initialized — sampling_period=%lu, random_seed=%u, "
         "%d CPUs, %d TLB sets x %d ways\n",
         (unsigned long)params->sampling_period_size,
         params->random_seed,
         LRUCOARSERANDOM_TLB_NUM_CPUS, LRUCOARSERANDOM_TLB_SETS, LRUCOARSERANDOM_TLB_WAYS);

  cache->eviction_params = params;
  return cache;
}

static void LruCoarseRandom_free(cache_t *cache) {
  LruCoarseRandom_params_t *params = (LruCoarseRandom_params_t *)cache->eviction_params;

  printf("=== LruCoarseRandom Statistics ===\n");
  printf("Sampling period: %lu\n", (unsigned long)params->sampling_period_size);
  printf("Num CPUs:        %d\n", LRUCOARSERANDOM_TLB_NUM_CPUS);
  printf("Total accesses:  %lu\n", (unsigned long)params->total_accesses);
  printf("TLB hits:        %lu (%.2f%%)\n", (unsigned long)params->tlb_hits,
         params->total_accesses > 0
             ? 100.0 * params->tlb_hits / params->total_accesses : 0.0);
  printf("TLB misses:      %lu (%.2f%%)\n", (unsigned long)params->tlb_misses,
         params->total_accesses > 0
             ? 100.0 * params->tlb_misses / params->total_accesses : 0.0);
  printf("TLB evict invals:%lu\n",
         (unsigned long)params->tlb_eviction_invalidations);
  printf("Random selections:%lu\n",
         (unsigned long)params->random_selections);
  printf("============================\n");

  /* Free bucket chain (pages already freed by cache framework) */
  LruCoarseRandomBucket *b = params->newest_bucket;
  while (b != NULL) {
    LruCoarseRandomBucket *next = b->older;
    free(b->pages);
    free(b);
    b = next;
  }

  free(params);
  cache_struct_free(cache);
}

/* =====================================================================
 * Core algorithm
 * ===================================================================== */

static bool LruCoarseRandom_get(cache_t *cache, const request_t *req) {
  LruCoarseRandom_params_t *params = (LruCoarseRandom_params_t *)cache->eviction_params;

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
  if (cpu >= LRUCOARSERANDOM_TLB_NUM_CPUS) {
    ERROR("LruCoarseRandom: cpu_id %u >= LRUCOARSERANDOM_TLB_NUM_CPUS %d\n",
          cpu, LRUCOARSERANDOM_TLB_NUM_CPUS);
    abort();
  }
  bool tlb_hit = lrucoarserandom_tlb_access(params, cpu, (uint64_t)req->obj_id);
  if (tlb_hit) {
    params->tlb_hits++;
  } else {
    params->tlb_misses++;
  }

  /* ---- Periodic report ---- */
  if (params->total_accesses - params->last_report_access >=
      LRUCOARSERANDOM_REPORT_INTERVAL) {
    params->last_report_access = params->total_accesses;
    printf("[LruCoarseRandom @ %luM] period=%lu accesses=%lu tlb_hits=%lu "
           "tlb_misses=%lu random_picks=%lu cache_n_obj=%ld\n",
           (unsigned long)(params->total_accesses / 1000000),
           (unsigned long)params->current_period,
           (unsigned long)params->total_accesses,
           (unsigned long)params->tlb_hits,
           (unsigned long)params->tlb_misses,
           (unsigned long)params->random_selections,
           (long)cache->n_obj);
  }

  /* ---- On TLB hit: just check cache (no promotion / no history update) ---- */
  if (tlb_hit) {
    cache_obj_t *obj = cache->find(cache, req, false);
    return (obj != NULL);
  }

  /* ---- On TLB miss: promote in bucket structure ---- */
  uint64_t T = params->current_period;

  cache_obj_t *obj = cache_find_base(cache, req, false);

  if (obj != NULL) {
    /* Cache hit on TLB miss — move page to bucket T */
    LruCoarseRandomBucket *cur_bucket = (LruCoarseRandomBucket *)obj->LruCoarse.bucket;
    if (cur_bucket == NULL || cur_bucket->timestamp != T) {
      lrucoarserandom_remove_from_bucket(params, obj);
      LruCoarseRandomBucket *new_bucket =
          lrucoarserandom_get_or_create_bucket(params, T);
      lrucoarserandom_add_to_bucket(obj, new_bucket);
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

static cache_obj_t *LruCoarseRandom_find(cache_t *cache, const request_t *req,
                                    bool update_cache) {
  /* find is only called from LruCoarseRandom_get with update_cache=false
   * for TLB hits (read-only lookup). We don't use the default
   * cache_find_base promotion logic — we handle it ourselves in get(). */
  return cache_find_base(cache, req, update_cache);
}

static cache_obj_t *LruCoarseRandom_insert(cache_t *cache, const request_t *req) {
  LruCoarseRandom_params_t *params = (LruCoarseRandom_params_t *)cache->eviction_params;

  cache_obj_t *obj = cache_insert_base(cache, req);

  uint64_t T = params->current_period;
  LruCoarseRandomBucket *b = lrucoarserandom_get_or_create_bucket(params, T);
  lrucoarserandom_add_to_bucket(obj, b);

  return obj;
}

static cache_obj_t *LruCoarseRandom_to_evict(cache_t *cache, const request_t *req) {
  LruCoarseRandom_params_t *params = (LruCoarseRandom_params_t *)cache->eviction_params;
  return lrucoarserandom_pick_eviction_victim(params);
}

static void LruCoarseRandom_evict(cache_t *cache, const request_t *req) {
  LruCoarseRandom_params_t *params = (LruCoarseRandom_params_t *)cache->eviction_params;
  cache_obj_t *victim = lrucoarserandom_pick_eviction_victim(params);
  DEBUG_ASSERT(victim != NULL);

  /* Invalidate evicted page from ALL per-CPU TLBs */
  lrucoarserandom_tlb_invalidate_all(params, (uint64_t)victim->obj_id);

  /* Remove from bucket */
  lrucoarserandom_remove_from_bucket(params, victim);

#if defined(TRACK_DEMOTION)
  if (cache->track_demotion)
    printf("%ld demote %ld %ld\n", cache->n_req, victim->create_time,
           victim->next_access_vtime);
#endif

  cache_evict_base(cache, victim, true);
}

static bool LruCoarseRandom_remove(cache_t *cache, obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) return false;

  LruCoarseRandom_params_t *params = (LruCoarseRandom_params_t *)cache->eviction_params;
  lrucoarserandom_tlb_invalidate_all(params, (uint64_t)obj->obj_id);
  lrucoarserandom_remove_from_bucket(params, obj);
  cache_remove_obj_base(cache, obj, true);
  return true;
}

#ifdef __cplusplus
}
#endif
