//
//  LRUAccessBit — Coarse-grained LRU with per-CPU TLB filtering + random eviction simulating Access Bit
////
//  Memory accesses are grouped into sampling periods of size X.  All TLB
//  misses within the same period share a single coarse timestamp.  Pages
//  in the cache are bucketed by their most-recent coarse timestamp.
//
//  Runtime parameters (via --eviction-params):
//    sampling_period=N     number of accesses per coarse period (default 1)
//
//  Usage:
//    ./bin/cachesim <trace> mergedTrace lruAccessBit <size> 
//        --ignore-obj-size 1 --eviction-params "sampling_period=1000"
//
//  LRUAccessBit.c
//  libCacheSim
//

#include <assert.h>
#include <string.h>
#include <strings.h>
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
#define LRUACCESSBIT_DEFAULT_TLB_SETS      512U
#define LRUACCESSBIT_DEFAULT_TLB_WAYS      4U
#define LRUACCESSBIT_DEFAULT_TLB_NUM_CPUS  4U
#define LRUACCESSBIT_DEFAULT_REPORT_INTERVAL  10000000UL  /* print stats every 10M */

/* =====================================================================
 * Data structures
 * ===================================================================== */

/* ---- TLB slot ---- */
typedef struct {
  bool     valid;
  uint64_t page;
  uint64_t last_access_time;
} LRUAccessBitSlot;

/* ---- Bucket: one per coarse timestamp that has cached pages ---- */
typedef struct LRUAccessBitBucket {
  uint64_t timestamp;
  cache_obj_t **pages;                   /* dynamic array of page pointers */
  int32_t n_pages;                       /* logical size */
  int32_t capacity;                      /* allocated slots */
  struct LRUAccessBitBucket *newer;         /* towards head (newest) */
  struct LRUAccessBitBucket *older;         /* towards tail (oldest) */
} LRUAccessBitBucket;

/* ---- Algorithm parameters ---- */
typedef struct {
  /* Bucket chain: newest ←→ oldest */
  LRUAccessBitBucket *newest_bucket;
  LRUAccessBitBucket *oldest_bucket;

  /* Coarse timing */
  uint64_t current_period;
  uint64_t accesses_in_period;
  uint64_t sampling_period_size;         /* X — runtime param */

  /* TLB configuration */
  uint32_t tlb_sets;                     /* must be power of 2 */
  uint32_t tlb_ways;
  uint32_t tlb_num_cpus;
  uint64_t tlb_report_interval;

  /* Per-CPU TLB simulation in flat layout: [cpu][set][way] */
  LRUAccessBitSlot *tlb;
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
} LRUAccessBit_params_t;

static inline bool lruaccessbit_is_power_of_two(uint32_t x) {
  return x != 0 && (x & (x - 1U)) == 0;
}

static inline LRUAccessBitSlot *lruaccessbit_tlb_set(
    LRUAccessBit_params_t *params, uint32_t cpu_id, uint32_t set_idx) {
  size_t base = ((size_t)cpu_id * params->tlb_sets + set_idx) * params->tlb_ways;
  return &params->tlb[base];
}

/* =====================================================================
 * TLB helpers
 * ===================================================================== */
static inline bool lruaccessbit_tlb_access(LRUAccessBit_params_t *params,
                                        uint32_t cpu_id, uint64_t page) {
  if (params->tlb_sets == 0) {
    return false;
  }

  uint32_t set_idx = (uint32_t)(page & (params->tlb_sets - 1U));
  LRUAccessBitSlot *set = lruaccessbit_tlb_set(params, cpu_id, set_idx);

  for (uint32_t w = 0; w < params->tlb_ways; w++) {
    if (set[w].valid && set[w].page == page) {
      set[w].last_access_time = params->tlb_global_time++;
      return true;
    }
  }

  int victim = -1;
  for (uint32_t w = 0; w < params->tlb_ways; w++) {
    if (!set[w].valid) { victim = w; break; }
  }
  if (victim < 0) {
    victim = 0;
    for (uint32_t w = 1; w < params->tlb_ways; w++) {
      if (set[w].last_access_time < set[victim].last_access_time)
        victim = w;
    }
  }
  set[victim].valid = true;
  set[victim].page = page;
  set[victim].last_access_time = params->tlb_global_time++;
  return false;
}

static inline void lruaccessbit_tlb_invalidate_all(LRUAccessBit_params_t *params,
                                                 uint64_t page) {
  if (params->tlb_sets == 0) {
    return;
  }

  uint32_t set_idx = (uint32_t)(page & (params->tlb_sets - 1U));
  for (uint32_t cpu = 0; cpu < params->tlb_num_cpus; cpu++) {
    LRUAccessBitSlot *set = lruaccessbit_tlb_set(params, cpu, set_idx);
    for (uint32_t w = 0; w < params->tlb_ways; w++) {
      if (set[w].valid && set[w].page == page) {
        set[w].valid = false;
        params->tlb_eviction_invalidations++;
        break;
      }
    }
  }
}

#define LRUACCESSBIT_BUCKET_INIT_CAP  64  /* initial capacity for bucket pages array */

/* =====================================================================
 * Bucket helpers
 * ===================================================================== */

/* Get or create bucket for the current (newest) timestamp.
 * Invariant: ts is always current_period, which is monotonically increasing,
 * so the matching bucket (if any) is always newest_bucket. */
static inline LRUAccessBitBucket *lruaccessbit_get_or_create_bucket(
    LRUAccessBit_params_t *params, uint64_t ts) {
  assert(ts == params->current_period);
  if (params->newest_bucket != NULL && params->newest_bucket->timestamp == ts)
    return params->newest_bucket;

  LRUAccessBitBucket *b = (LRUAccessBitBucket *)calloc(1, sizeof(LRUAccessBitBucket));
  b->timestamp = ts;
  b->capacity = LRUACCESSBIT_BUCKET_INIT_CAP;
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
static inline void lruaccessbit_remove_from_bucket(LRUAccessBit_params_t *params,
                                                 cache_obj_t *obj) {
  LRUAccessBitBucket *b = (LRUAccessBitBucket *)obj->LruCoarse.bucket;
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
static inline void lruaccessbit_add_to_bucket(cache_obj_t *obj,
                                            LRUAccessBitBucket *b) {
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
static cache_obj_t *lruaccessbit_pick_eviction_victim(LRUAccessBit_params_t *params) {
  LRUAccessBitBucket *b = params->oldest_bucket;
  if (b == NULL) {
    LOG(ERROR, STREAM_Utils,
        "LRUAccessBit: pick_eviction_victim called with no buckets (cache empty?)\n");
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
static void LRUAccessBit_free(cache_t *cache);
static bool LRUAccessBit_get(cache_t *cache, const request_t *req);
static cache_obj_t *LRUAccessBit_find(cache_t *cache, const request_t *req,
                                    bool update_cache);
static cache_obj_t *LRUAccessBit_insert(cache_t *cache, const request_t *req);
static cache_obj_t *LRUAccessBit_to_evict(cache_t *cache, const request_t *req);
static void LRUAccessBit_evict(cache_t *cache, const request_t *req);
static bool LRUAccessBit_remove(cache_t *cache, obj_id_t obj_id);

/* =====================================================================
 * Parameter parsing
 * ===================================================================== */
static void LRUAccessBit_parse_params(const char *cache_specific_params,
                                    LRUAccessBit_params_t *params) {
  /* defaults */
  params->sampling_period_size = 1;
  params->tlb_sets = LRUACCESSBIT_DEFAULT_TLB_SETS;
  params->tlb_ways = LRUACCESSBIT_DEFAULT_TLB_WAYS;
  params->tlb_num_cpus = LRUACCESSBIT_DEFAULT_TLB_NUM_CPUS;
  params->tlb_report_interval = LRUACCESSBIT_DEFAULT_REPORT_INTERVAL;

  if (cache_specific_params == NULL || cache_specific_params[0] == '\0')
    goto validate;

  char *params_str = strdup(cache_specific_params);
  char *p = params_str;
  while (p != NULL && p[0] != '\0') {
    char *key = strsep(&p, "=");
    char *value = strsep(&p, ",");
    while (p != NULL && *p == ' ') p++;

    if (strcasecmp(key, "sampling-period") == 0 ||
        strcasecmp(key, "sampling_period") == 0) {
      params->sampling_period_size = (uint64_t)strtoull(value, NULL, 10);
      if (params->sampling_period_size == 0) params->sampling_period_size = 1;
    } else if (strcasecmp(key, "sets") == 0) {
      params->tlb_sets = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcasecmp(key, "ways") == 0) {
      params->tlb_ways = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcasecmp(key, "num-cpus") == 0 ||
               strcasecmp(key, "num_cpus") == 0) {
      params->tlb_num_cpus = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcasecmp(key, "report-interval") == 0 ||
               strcasecmp(key, "report_interval") == 0) {
      params->tlb_report_interval = (uint64_t)strtoull(value, NULL, 10);
    } else if (strcasecmp(key, "random-seed") == 0 ||
               strcasecmp(key, "random_seed") == 0) {
      params->random_seed = (unsigned int)strtoul(value, NULL, 10);
      params->random_seed_set = true;
    } else {
      LOG(ERROR, STREAM_Utils, "LRUAccessBit: unknown param \"%s\"\n", key);
      abort();
    }
  }
  free(params_str);

validate:
  if (params->tlb_sets != 0 && !lruaccessbit_is_power_of_two(params->tlb_sets)) {
    LOG(ERROR, STREAM_Utils,
        "LRUAccessBit: sets (%u) must be 0 or a power of two\n", params->tlb_sets);
    abort();
  }
  if (params->tlb_ways == 0) {
    LOG(ERROR, STREAM_Utils, "LRUAccessBit: ways must be > 0\n");
    abort();
  }
  if (params->tlb_num_cpus == 0) {
    LOG(ERROR, STREAM_Utils, "LRUAccessBit: num_cpus must be > 0\n");
    abort();
  }
}

/* =====================================================================
 * Init / Free
 * ===================================================================== */
cache_t *LRUAccessBit_init(const common_cache_params_t ccache_params,
                         const char *cache_specific_params) {
  cache_t *cache =
      cache_struct_init("LRUAccessBit", ccache_params, cache_specific_params);
  cache->cache_init = LRUAccessBit_init;
  cache->cache_free = LRUAccessBit_free;
  cache->get = LRUAccessBit_get;
  cache->find = LRUAccessBit_find;
  cache->insert = LRUAccessBit_insert;
  cache->evict = LRUAccessBit_evict;
  cache->remove = LRUAccessBit_remove;
  cache->to_evict = LRUAccessBit_to_evict;
  cache->get_occupied_byte = cache_get_occupied_byte_default;
  cache->can_insert = cache_can_insert_default;
  cache->get_n_obj = cache_get_n_obj_default;

  if (ccache_params.consider_obj_metadata) {
    cache->obj_md_size = 8 * 2;
  } else {
    cache->obj_md_size = 0;
  }

  LRUAccessBit_params_t *params = calloc(1, sizeof(LRUAccessBit_params_t));
  LRUAccessBit_parse_params(cache_specific_params, params);

  if (params->tlb_sets > 0) {
    size_t n_tlb_entries = (size_t)params->tlb_num_cpus * params->tlb_sets *
                           params->tlb_ways;
    params->tlb = calloc(n_tlb_entries, sizeof(LRUAccessBitSlot));
    if (params->tlb == NULL) {
      LOG(ERROR, STREAM_Utils,
          "LRUAccessBit: failed to allocate TLB (%u cpus x %u sets x %u ways)\n",
          params->tlb_num_cpus, params->tlb_sets, params->tlb_ways);
      abort();
    }
  } else {
    params->tlb = NULL;
  }

  params->newest_bucket = NULL;
  params->oldest_bucket = NULL;
  params->current_period = 1;
  params->accesses_in_period = 0;

  if (params->random_seed_set) {
    srand(params->random_seed);
  } else {
    unsigned int seed = (unsigned int)(time(NULL) ^ getpid());
    srand(seed);
    params->random_seed = seed;
  }

  LOG(INFO, STREAM_Utils, "LRUAccessBit: initialized — sampling_period=%lu, random_seed=%u, "
      "%u CPUs, %u TLB sets x %u ways, report_interval=%lu\n",
         (unsigned long)params->sampling_period_size,
         params->random_seed,
      params->tlb_num_cpus, params->tlb_sets, params->tlb_ways,
      (unsigned long)params->tlb_report_interval);

  cache->eviction_params = params;
  return cache;
}

static void LRUAccessBit_free(cache_t *cache) {
  LRUAccessBit_params_t *params = (LRUAccessBit_params_t *)cache->eviction_params;

  LOG(INFO, STREAM_Utils, "=== LRUAccessBit Statistics ===");
  LOG(INFO, STREAM_Utils, "Sampling period: %lu", (unsigned long)params->sampling_period_size);
  LOG(INFO, STREAM_Utils, "Num CPUs:        %u", params->tlb_num_cpus);
  LOG(INFO, STREAM_Utils, "TLB sets/ways:   %u/%u", params->tlb_sets,
      params->tlb_ways);
    LOG(INFO, STREAM_Utils, "Report interval: %lu",
      (unsigned long)params->tlb_report_interval);
  LOG(INFO, STREAM_Utils, "Total accesses:  %lu", (unsigned long)params->total_accesses);
  LOG(INFO, STREAM_Utils, "TLB hits:        %lu (%.2f%%)", (unsigned long)params->tlb_hits,
         params->total_accesses > 0
             ? 100.0 * params->tlb_hits / params->total_accesses : 0.0);
  LOG(INFO, STREAM_Utils, "TLB misses:      %lu (%.2f%%)", (unsigned long)params->tlb_misses,
         params->total_accesses > 0
             ? 100.0 * params->tlb_misses / params->total_accesses : 0.0);
  LOG(INFO, STREAM_Utils, "TLB evict invals:%lu",
         (unsigned long)params->tlb_eviction_invalidations);
  LOG(INFO, STREAM_Utils, "Random selections:%lu",
         (unsigned long)params->random_selections);
  LOG(INFO, STREAM_Utils, "============================");

  /* Free bucket chain (pages already freed by cache framework) */
  LRUAccessBitBucket *b = params->newest_bucket;
  while (b != NULL) {
    LRUAccessBitBucket *next = b->older;
    free(b->pages);
    free(b);
    b = next;
  }

  free(params->tlb);
  free(params);
  cache_struct_free(cache);
}

/* =====================================================================
 * Core algorithm
 * ===================================================================== */

static bool LRUAccessBit_get(cache_t *cache, const request_t *req) {
  LRUAccessBit_params_t *params = (LRUAccessBit_params_t *)cache->eviction_params;

  cache->n_req += 1;

  /* ---- advance coarse clock ---- */
  params->total_accesses++;
  params->accesses_in_period++;
  if (params->accesses_in_period >= params->sampling_period_size) {
    params->current_period++;
    params->accesses_in_period = 0;
  }

  /* ---- TLB simulation ---- */
  uint32_t cpu = req->cpu_id;
  if (params->tlb_sets > 0 && cpu >= params->tlb_num_cpus) {
    LOG(ERROR, STREAM_Utils,
        "LRUAccessBit: cpu_id %u >= tlb_num_cpus %u\n",
        cpu, params->tlb_num_cpus);
    abort();
  }
  bool tlb_hit = lruaccessbit_tlb_access(params, cpu, (uint64_t)req->obj_id);
  if (tlb_hit) {
    params->tlb_hits++;
  } else {
    params->tlb_misses++;
  }

  /* ---- Periodic report ---- */
  if (params->tlb_report_interval > 0 &&
      params->total_accesses - params->last_report_access >=
          params->tlb_report_interval) {
    params->last_report_access = params->total_accesses;
    // LOG(INFO, STREAM_Utils,
    //     "[LRUAccessBit @ %luM] period=%lu accesses=%lu tlb_hits=%lu "
    //     "tlb_misses=%lu random_picks=%lu cache_n_obj=%ld",
    //     (unsigned long)(params->total_accesses / 1000000),
    //     (unsigned long)params->current_period,
    //     (unsigned long)params->total_accesses,
    //     (unsigned long)params->tlb_hits,
    //     (unsigned long)params->tlb_misses,
    //     (unsigned long)params->random_selections,
    //     (long)cache->n_obj);
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
    LRUAccessBitBucket *cur_bucket = (LRUAccessBitBucket *)obj->LruCoarse.bucket;
    if (cur_bucket == NULL || cur_bucket->timestamp != T) {
      lruaccessbit_remove_from_bucket(params, obj);
      LRUAccessBitBucket *new_bucket =
          lruaccessbit_get_or_create_bucket(params, T);
      lruaccessbit_add_to_bucket(obj, new_bucket);
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

static cache_obj_t *LRUAccessBit_find(cache_t *cache, const request_t *req,
                                    bool update_cache) {
  /* find is only called from LRUAccessBit_get with update_cache=false
   * for TLB hits (read-only lookup). */
  return cache_find_base(cache, req, update_cache);
}

static cache_obj_t *LRUAccessBit_insert(cache_t *cache, const request_t *req) {
  LRUAccessBit_params_t *params = (LRUAccessBit_params_t *)cache->eviction_params;

  cache_obj_t *obj = cache_insert_base(cache, req);

  uint64_t T = params->current_period;
  LRUAccessBitBucket *b = lruaccessbit_get_or_create_bucket(params, T);
  lruaccessbit_add_to_bucket(obj, b);

  return obj;
}

static cache_obj_t *LRUAccessBit_to_evict(cache_t *cache, const request_t *req) {
  LRUAccessBit_params_t *params = (LRUAccessBit_params_t *)cache->eviction_params;
  return lruaccessbit_pick_eviction_victim(params);
}

static void LRUAccessBit_evict(cache_t *cache, const request_t *req) {
  LRUAccessBit_params_t *params = (LRUAccessBit_params_t *)cache->eviction_params;
  cache_obj_t *victim = lruaccessbit_pick_eviction_victim(params);
  DEBUG_ASSERT(victim != NULL);

  /* Invalidate evicted page from ALL per-CPU TLBs */
  lruaccessbit_tlb_invalidate_all(params, (uint64_t)victim->obj_id);

  /* Remove from bucket */
  lruaccessbit_remove_from_bucket(params, victim);

#if defined(TRACK_DEMOTION)
  if (cache->track_demotion)
    printf("%ld demote %ld %ld\n", cache->n_req, victim->create_time,
           victim->next_access_vtime);
#endif

  cache_evict_base(cache, victim, true);
}

static bool LRUAccessBit_remove(cache_t *cache, obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) return false;

  LRUAccessBit_params_t *params = (LRUAccessBit_params_t *)cache->eviction_params;
  lruaccessbit_tlb_invalidate_all(params, (uint64_t)obj->obj_id);
  lruaccessbit_remove_from_bucket(params, obj);
  cache_remove_obj_base(cache, obj, true);
  return true;
}

#ifdef __cplusplus
}
#endif
