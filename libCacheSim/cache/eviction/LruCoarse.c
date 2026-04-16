//
//  LRU with LRU-TLB filtering (multi-CPU)
//
//  Copy of LRU.c with two changes:
//    1. LRU recency is only updated on TLB miss (not every access).
//       TLB hits are invisible to the cache.
//    2. On eviction, the evicted page is invalidated from ALL per-CPU TLBs.
//
//  Per-CPU TLBs: each CPU has its own LRU TLB (512 sets × 4 ways).
//  The CPU ID comes from req->cpu_id (set by the mergedTrace reader).
//
//  Usage:
//    ./bin/cachesim <merged_trace> mergedTrace lruCoarse <size>
//    Number of CPUs is set at compile time via LRUCOARSE_TLB_NUM_CPUS.
//
//  LruCoarse.c
//  libCacheSim
//

#include "dataStructure/hashtable/hashtable.h"
#include "libCacheSim/evictionAlgo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * TLB simulation configuration
 * ===================================================================== */
#define LRUCOARSE_TLB_SETS      512   /* must be power of 2 */
#define LRUCOARSE_TLB_WAYS      4
#define LRUCOARSE_TLB_NUM_CPUS  4     /* number of CPUs — change before compiling */
#define LRUCOARSE_TLB_REPORT_INTERVAL  10000000UL  /* print stats every 10M accesses */

typedef struct {
  bool     valid;
  uint64_t page;
  uint64_t last_access_time;
} LruCoarseSlot;

typedef struct {
  cache_obj_t *q_head;
  cache_obj_t *q_tail;

  /* Per-CPU TLB simulation: tlb[cpu][set][way] */
  LruCoarseSlot tlb[LRUCOARSE_TLB_NUM_CPUS][LRUCOARSE_TLB_SETS][LRUCOARSE_TLB_WAYS];
  uint64_t tlb_global_time;

  /* Stats */
  uint64_t total_accesses;
  uint64_t tlb_hits;
  uint64_t tlb_misses;
  uint64_t tlb_eviction_invalidations;
  uint64_t last_report_access;
} LruCoarse_params_t;

/**
 * Simulate an LRU TLB access on a specific CPU's TLB.
 * Returns true on TLB hit, false on TLB miss.
 */
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

/**
 * Invalidate a page from ALL per-CPU TLBs.
 * Called on cache eviction so that evicted pages don't remain
 * "cached" in any TLB (which would filter future accesses).
 */
static inline void lrucoarse_tlb_invalidate_all(LruCoarse_params_t *params,
                                             uint64_t page) {
  uint32_t set_idx = (uint32_t)(page & (LRUCOARSE_TLB_SETS - 1));

  for (int cpu = 0; cpu < LRUCOARSE_TLB_NUM_CPUS; cpu++) {
    LruCoarseSlot *set = params->tlb[cpu][set_idx];
    for (int w = 0; w < LRUCOARSE_TLB_WAYS; w++) {
      if (set[w].valid && set[w].page == page) {
        set[w].valid = false;
        params->tlb_eviction_invalidations++;
        break;  /* page can appear at most once per set per CPU */
      }
    }
  }
}

// ***********************************************************************
// ****                                                               ****
// ****                   function declarations                       ****
// ****                                                               ****
// ***********************************************************************

static void LruCoarse_free(cache_t *cache);
static bool LruCoarse_get(cache_t *cache, const request_t *req);
static cache_obj_t *LruCoarse_find(cache_t *cache, const request_t *req,
                             bool update_cache);
static cache_obj_t *LruCoarse_insert(cache_t *cache, const request_t *req);
static cache_obj_t *LruCoarse_to_evict(cache_t *cache, const request_t *req);
static void LruCoarse_evict(cache_t *cache, const request_t *req);
static bool LruCoarse_remove(cache_t *cache, obj_id_t obj_id);
static void LruCoarse_print_cache(const cache_t *cache);

// ***********************************************************************
// ****                                                               ****
// ****                   end user facing functions                   ****
// ****                                                               ****
// ****                       init, free, get                         ****
// ***********************************************************************
/**
 * @brief initialize a LruCoarse cache
 *
 * @param ccache_params some common cache parameters
 * @param cache_specific_params specific parameters, should be NULL
 */
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
  cache->print_cache = LruCoarse_print_cache;

  if (ccache_params.consider_obj_metadata) {
    cache->obj_md_size = 8 * 2;
  } else {
    cache->obj_md_size = 0;
  }

  LruCoarse_params_t *params = calloc(1, sizeof(LruCoarse_params_t));
  params->q_head = NULL;
  params->q_tail = NULL;

  printf("LruCoarse: initialized with %d CPUs, %d sets x %d ways per TLB\n",
         LRUCOARSE_TLB_NUM_CPUS, LRUCOARSE_TLB_SETS, LRUCOARSE_TLB_WAYS);

  cache->eviction_params = params;

  return cache;
}

/**
 * free resources used by this cache
 *
 * @param cache
 */
static void LruCoarse_free(cache_t *cache) {
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;

  printf("=== LruCoarse TLB Statistics ===\n");
  printf("Num CPUs:        %d\n", LRUCOARSE_TLB_NUM_CPUS);
  printf("Total accesses:  %lu\n", (unsigned long)params->total_accesses);
  printf("TLB hits:        %lu (%.2f%%)\n", (unsigned long)params->tlb_hits,
         params->total_accesses > 0
             ? 100.0 * params->tlb_hits / params->total_accesses
             : 0.0);
  printf("TLB misses:      %lu (%.2f%%)\n", (unsigned long)params->tlb_misses,
         params->total_accesses > 0
             ? 100.0 * params->tlb_misses / params->total_accesses
             : 0.0);
  printf("TLB evict invals:%lu\n", (unsigned long)params->tlb_eviction_invalidations);
  printf("=========================================\n");

  free(params);
  cache_struct_free(cache);
}

/**
 * @brief this function is the user facing API
 * it performs the following logic
 *
 * ```
 * if obj in cache:
 *    update_metadata
 *    return true
 * else:
 *    if cache does not have enough space:
 *        evict until it has space to insert
 *    insert the object
 *    return false
 * ```
 *
 * @param cache
 * @param req
 * @return true if cache hit, false if cache miss
 *
 * CHANGE vs LRU: we do NOT call cache_get_base directly.
 * Instead we simulate the TLB first and only pass update_cache=true
 * to find() on a TLB miss.
 */
static bool LruCoarse_get(cache_t *cache, const request_t *req) {
  LruCoarse_params_t *params =
      (LruCoarse_params_t *)cache->eviction_params;

  cache->n_req += 1;

  /* TLB simulation on the CPU that issued this access */
  params->total_accesses++;
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

  /* Periodic report */
  if (params->total_accesses - params->last_report_access >= LRUCOARSE_TLB_REPORT_INTERVAL) {
    params->last_report_access = params->total_accesses;
    printf("[LruCoarse @ %luM] accesses=%lu tlb_hits=%lu tlb_misses=%lu "
           "cache_n_obj=%ld\n",
           (unsigned long)(params->total_accesses / 1000000),
           (unsigned long)params->total_accesses,
           (unsigned long)params->tlb_hits,
           (unsigned long)params->tlb_misses,
           (long)cache->n_obj);
  }

  /* TLB hit  → find(update_cache=false): lookup only, no LRU promotion
   * TLB miss → find(update_cache=true):  lookup + move-to-head           */
  cache_obj_t *obj = cache->find(cache, req, !tlb_hit);

  if (obj != NULL) {
    return true;
  }

  /* Cache miss — evict and insert (same as cache_get_base) */
  while (cache->get_occupied_byte(cache) + req->obj_size + cache->obj_md_size >
         cache->cache_size) {
    cache->evict(cache, req);
  }

  cache->insert(cache, req);
  return false;
}

// ***********************************************************************
// ****                                                               ****
// ****       developer facing APIs (used by cache developer)         ****
// ****                                                               ****
// ***********************************************************************

/**
 * @brief check whether an object is in the cache
 *
 * @param cache
 * @param req
 * @param update_cache whether to update the cache,
 *  if true, the object is promoted
 *  and if the object is expired, it is removed from the cache
 * @return true on hit, false on miss
 */
static cache_obj_t *LruCoarse_find(cache_t *cache, const request_t *req,
                             bool update_cache) {
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;
  cache_obj_t *cache_obj = cache_find_base(cache, req, update_cache);

  if (cache_obj && likely(update_cache)) {
    /* lru_head is the newest, move cur obj to lru_head */
      move_obj_to_head(&params->q_head, &params->q_tail, cache_obj);
  }
  return cache_obj;
}

/**
 * @brief insert an object into the cache,
 * update the hash table and cache metadata
 * this function assumes the cache has enough space
 * and eviction is not part of this function
 *
 * @param cache
 * @param req
 * @return the inserted object
 */
static cache_obj_t *LruCoarse_insert(cache_t *cache, const request_t *req) {
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;

  cache_obj_t *obj = cache_insert_base(cache, req);
  prepend_obj_to_head(&params->q_head, &params->q_tail, obj);

  return obj;
}

/**
 * @brief find the object to be evicted
 * this function does not actually evict the object or update metadata
 * not all eviction algorithms support this function
 * because the eviction logic cannot be decoupled from finding eviction
 * candidate, so use assert(false) if you cannot support this function
 *
 * @param cache the cache
 * @return the object to be evicted
 */
static cache_obj_t *LruCoarse_to_evict(cache_t *cache, const request_t *req) {
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;

  DEBUG_ASSERT(params->q_tail != NULL || cache->occupied_byte == 0);

  cache->to_evict_candidate_gen_vtime = cache->n_req;
  return params->q_tail;
}

/**
 * @brief evict an object from the cache
 * it needs to call cache_evict_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param req not used
 */
static void LruCoarse_evict(cache_t *cache, const request_t *req) {
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;
  cache_obj_t *obj_to_evict = params->q_tail;
  DEBUG_ASSERT(params->q_tail != NULL);

  /* Invalidate evicted page from ALL per-CPU TLBs */
  lrucoarse_tlb_invalidate_all(params, (uint64_t)obj_to_evict->obj_id);

  // we can simply call remove_obj_from_list here, but for the best performance,
  // we chose to do it manually
  // remove_obj_from_list(&params->q_head, &params->q_tail, obj)

  params->q_tail = params->q_tail->queue.prev;
  if (likely(params->q_tail != NULL)) {
    params->q_tail->queue.next = NULL;
  } else {
    /* cache->n_obj has not been updated */
    DEBUG_ASSERT(cache->n_obj == 1);
    params->q_head = NULL;
  }

#if defined(TRACK_DEMOTION)
  if (cache->track_demotion)
    printf("%ld demote %ld %ld\n", cache->n_req, obj_to_evict->create_time,
           obj_to_evict->next_access_vtime);
#endif

  cache_evict_base(cache, obj_to_evict, true);
}

/**
 * @brief remove the given object from the cache
 * note that eviction should not call this function, but rather call
 * `cache_evict_base` because we track extra metadata during eviction
 *
 * and this function is different from eviction
 * because it is used to for user trigger
 * remove, and eviction is used by the cache to make space for new objects
 *
 * it needs to call cache_remove_obj_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param obj
 */
static void LruCoarse_remove_obj(cache_t *cache, cache_obj_t *obj) {
  assert(obj != NULL);

  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;

  remove_obj_from_list(&params->q_head, &params->q_tail, obj);
  cache_remove_obj_base(cache, obj, true);
}

/**
 * @brief remove an object from the cache
 * this is different from cache_evict because it is used to for user trigger
 * remove, and eviction is used by the cache to make space for new objects
 *
 * it needs to call cache_remove_obj_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param obj_id
 * @return true if the object is removed, false if the object is not in the
 * cache
 */
static bool LruCoarse_remove(cache_t *cache, obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) {
    return false;
  }
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;

  remove_obj_from_list(&params->q_head, &params->q_tail, obj);
  cache_remove_obj_base(cache, obj, true);

  return true;
}

static void LruCoarse_print_cache(const cache_t *cache) {
  LruCoarse_params_t *params = (LruCoarse_params_t *)cache->eviction_params;
  cache_obj_t *cur = params->q_head;
  if (cur == NULL) {
    printf("empty\n");
    return;
  }
  while (cur != NULL) {
    printf("%lu->", (unsigned long)cur->obj_id);
    cur = cur->queue.next;
  }
  printf("END\n");
}

#ifdef __cplusplus
}
#endif
