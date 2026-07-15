//
//  LRU with LRU-TLB filtering (multi-CPU)
//
//  Copy of LRU.c with two changes:
//    1. LRU recency is only updated on TLB miss (not every access).
//       TLB hits are invisible to the cache.
//    2. On eviction, the evicted page is invalidated from ALL per-CPU TLBs.
//
//  Per-CPU TLBs: each CPU has its own LRU TLB (512 sets × 4 ways).
//  The CPU ID comes from request features[0] (set by the mergedTrace reader).
//
//  Usage:
//    ./bin/cachesim <merged_trace> mergedTrace lrutlbfiltered <size>
//    Number of CPUs is set at compile time via LRUTLBFILTERED_TLB_NUM_CPUS.
//
//  LRUTLBFiltered.c
//  libCacheSim
//

#include "dataStructure/hashtable/hashtable.h"
#include "libCacheSim/evictionAlgo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * TLB runtime defaults
 * ===================================================================== */
#define LRUTLBFILTERED_DEFAULT_TLB_SETS             512U
#define LRUTLBFILTERED_DEFAULT_TLB_WAYS             4U
#define LRUTLBFILTERED_DEFAULT_TLB_NUM_CPUS         4U
#define LRUTLBFILTERED_DEFAULT_TLB_REPORT_INTERVAL  10000000UL
#define LRUTLBFILTERED_CPU_FEATURE_IDX              0

static inline uint32_t lrutlbfiltered_get_cpu_feature(const request_t *req) {
  if (req->n_features <= LRUTLBFILTERED_CPU_FEATURE_IDX) {
    return 0;
  }
  return (uint32_t)(uint8_t)req->features[LRUTLBFILTERED_CPU_FEATURE_IDX];
}

typedef struct {
  bool     valid;
  uint64_t page;
  uint64_t last_access_time;
} LRUTLBFilteredSlot;

typedef struct {
  cache_obj_t *q_head;
  cache_obj_t *q_tail;

  /* TLB configuration */
  uint32_t tlb_sets;            /* must be power of 2 */
  uint32_t tlb_ways;
  uint32_t tlb_num_cpus;
  uint64_t tlb_report_interval;

  /* Per-CPU TLB simulation in flat layout: [cpu][set][way] */
  LRUTLBFilteredSlot *tlb;
  uint64_t tlb_global_time;

  /* Stats */
  uint64_t total_accesses;
  uint64_t tlb_hits;
  uint64_t tlb_misses;
  uint64_t tlb_eviction_invalidations;
  uint64_t last_report_access;
} LRUTLBFiltered_params_t;

static inline bool lrutlbfiltered_is_power_of_two(uint32_t x) {
  return x != 0 && (x & (x - 1U)) == 0;
}

static inline LRUTLBFilteredSlot *lrutlbfiltered_tlb_set(
    LRUTLBFiltered_params_t *params, uint32_t cpu_id, uint32_t set_idx) {
  size_t base = ((size_t)cpu_id * params->tlb_sets + set_idx) * params->tlb_ways;
  return &params->tlb[base];
}

static void LRUTLBFiltered_parse_params(const char *cache_specific_params,
                                        LRUTLBFiltered_params_t *params) {
  params->tlb_sets = LRUTLBFILTERED_DEFAULT_TLB_SETS;
  params->tlb_ways = LRUTLBFILTERED_DEFAULT_TLB_WAYS;
  params->tlb_num_cpus = LRUTLBFILTERED_DEFAULT_TLB_NUM_CPUS;
  params->tlb_report_interval = LRUTLBFILTERED_DEFAULT_TLB_REPORT_INTERVAL;

  if (cache_specific_params == NULL || cache_specific_params[0] == '\0') {
    return;
  }

  char *params_str = strdup(cache_specific_params);
  char *p = params_str;

  while (p != NULL && p[0] != '\0') {
    char *key = strsep(&p, "=");
    char *value = strsep(&p, ",");
    while (p != NULL && *p == ' ') {
      p++;
    }

    if (key == NULL || value == NULL) {
      LOG(ERROR, STREAM_Utils,
          "LRUTLBFiltered: malformed params string: %s\n",
          cache_specific_params);
      abort();
    }

    if (strcasecmp(key, "sets") == 0) {
      params->tlb_sets = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcasecmp(key, "ways") == 0) {
      params->tlb_ways = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcasecmp(key, "num-cpus") == 0 ||
               strcasecmp(key, "num_cpus") == 0) {
      params->tlb_num_cpus = (uint32_t)strtoul(value, NULL, 10);
    } else if (strcasecmp(key, "report-interval") == 0 ||
               strcasecmp(key, "report_interval") == 0) {
      params->tlb_report_interval = (uint64_t)strtoull(value, NULL, 10);
    } else {
      LOG(ERROR, STREAM_Utils,
          "LRUTLBFiltered: unknown param \"%s\"\n", key);
      abort();
    }
  }

  free(params_str);

  if (!lrutlbfiltered_is_power_of_two(params->tlb_sets)) {
    LOG(ERROR, STREAM_Utils,
        "LRUTLBFiltered: sets (%u) must be a power of two\n",
        params->tlb_sets);
    abort();
  }
  if (params->tlb_ways == 0) {
    LOG(ERROR, STREAM_Utils,
        "LRUTLBFiltered: ways must be > 0\n");
    abort();
  }
  if (params->tlb_num_cpus == 0) {
    LOG(ERROR, STREAM_Utils,
        "LRUTLBFiltered: num_cpus must be > 0\n");
    abort();
  }
}

/**
 * Simulate an LRU TLB access on a specific CPU's TLB.
 * Returns true on TLB hit, false on TLB miss.
 */
static inline bool lrutlbfiltered_tlb_access(LRUTLBFiltered_params_t *params,
                                     uint8_t cpu_id, uint64_t page) {
  uint32_t set_idx = (uint32_t)(page & (params->tlb_sets - 1U));
  LRUTLBFilteredSlot *set = lrutlbfiltered_tlb_set(params, cpu_id, set_idx);

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

/**
 * Invalidate a page from ALL per-CPU TLBs.
 * Called on cache eviction so that evicted pages don't remain
 * "cached" in any TLB (which would filter future accesses).
 */
static inline void lrutlbfiltered_tlb_invalidate_all(LRUTLBFiltered_params_t *params,
                                             uint64_t page) {
  uint32_t set_idx = (uint32_t)(page & (params->tlb_sets - 1U));

  for (uint32_t cpu = 0; cpu < params->tlb_num_cpus; cpu++) {
    LRUTLBFilteredSlot *set = lrutlbfiltered_tlb_set(params, cpu, set_idx);
    for (uint32_t w = 0; w < params->tlb_ways; w++) {
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

static void LRUTLBFiltered_free(cache_t *cache);
static bool LRUTLBFiltered_get(cache_t *cache, const request_t *req);
static cache_obj_t *LRUTLBFiltered_find(cache_t *cache, const request_t *req,
                             bool update_cache);
static cache_obj_t *LRUTLBFiltered_insert(cache_t *cache, const request_t *req);
static cache_obj_t *LRUTLBFiltered_to_evict(cache_t *cache, const request_t *req);
static void LRUTLBFiltered_evict(cache_t *cache, const request_t *req);
static bool LRUTLBFiltered_remove(cache_t *cache, obj_id_t obj_id);
static void LRUTLBFiltered_print_cache(const cache_t *cache);

// ***********************************************************************
// ****                                                               ****
// ****                   end user facing functions                   ****
// ****                                                               ****
// ****                       init, free, get                         ****
// ***********************************************************************
/**
 * @brief initialize a LRUTLBFiltered cache
 *
 * @param ccache_params some common cache parameters
 * @param cache_specific_params specific parameters, should be NULL
 */
cache_t *LRUTLBFiltered_init(const common_cache_params_t ccache_params,
                  const char *cache_specific_params) {
  cache_t *cache =
      cache_struct_init("LRUTLBFiltered", ccache_params, cache_specific_params);
  cache->cache_init = LRUTLBFiltered_init;
  cache->cache_free = LRUTLBFiltered_free;
  cache->get = LRUTLBFiltered_get;
  cache->find = LRUTLBFiltered_find;
  cache->insert = LRUTLBFiltered_insert;
  cache->evict = LRUTLBFiltered_evict;
  cache->remove = LRUTLBFiltered_remove;
  cache->to_evict = LRUTLBFiltered_to_evict;
  cache->get_occupied_byte = cache_get_occupied_byte_default;
  cache->can_insert = cache_can_insert_default;
  cache->get_n_obj = cache_get_n_obj_default;
  cache->print_cache = LRUTLBFiltered_print_cache;

  if (ccache_params.consider_obj_metadata) {
    cache->obj_md_size = 8 * 2;
  } else {
    cache->obj_md_size = 0;
  }

  LRUTLBFiltered_params_t *params = calloc(1, sizeof(LRUTLBFiltered_params_t));
  LRUTLBFiltered_parse_params(cache_specific_params, params);

  size_t n_tlb_entries = (size_t)params->tlb_num_cpus * params->tlb_sets *
                         params->tlb_ways;
  params->tlb = calloc(n_tlb_entries, sizeof(LRUTLBFilteredSlot));
  if (params->tlb == NULL) {
    LOG(ERROR, STREAM_Utils,
        "LRUTLBFiltered: failed to allocate TLB (%u cpus x %u sets x %u ways)\n",
        params->tlb_num_cpus, params->tlb_sets, params->tlb_ways);
    abort();
  }

  params->q_head = NULL;
  params->q_tail = NULL;

  LOG(INFO, STREAM_Utils,
      "LRUTLBFiltered: initialized with %u CPUs, %u sets x %u ways per TLB, "
      "report_interval=%lu",
      params->tlb_num_cpus, params->tlb_sets, params->tlb_ways,
      (unsigned long)params->tlb_report_interval);

  cache->eviction_params = params;

  return cache;
}

/**
 * free resources used by this cache
 *
 * @param cache
 */
static void LRUTLBFiltered_free(cache_t *cache) {
  LRUTLBFiltered_params_t *params = (LRUTLBFiltered_params_t *)cache->eviction_params;

  LOG(INFO, STREAM_Utils, "=== LRUTLBFiltered TLB Statistics ===");
  LOG(INFO, STREAM_Utils, "Num CPUs:        %u", params->tlb_num_cpus);
  LOG(INFO, STREAM_Utils, "TLB sets/ways:   %u/%u", params->tlb_sets,
    params->tlb_ways);
  LOG(INFO, STREAM_Utils, "Total accesses:  %lu",
    (unsigned long)params->total_accesses);
  LOG(INFO, STREAM_Utils, "TLB hits:        %lu (%.2f%%)",
    (unsigned long)params->tlb_hits,
         params->total_accesses > 0
             ? 100.0 * params->tlb_hits / params->total_accesses
             : 0.0);
  LOG(INFO, STREAM_Utils, "TLB misses:      %lu (%.2f%%)",
    (unsigned long)params->tlb_misses,
         params->total_accesses > 0
             ? 100.0 * params->tlb_misses / params->total_accesses
             : 0.0);
  LOG(INFO, STREAM_Utils, "TLB evict invals:%lu",
    (unsigned long)params->tlb_eviction_invalidations);
  LOG(INFO, STREAM_Utils, "========================================");

  free(params->tlb);
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
static bool LRUTLBFiltered_get(cache_t *cache, const request_t *req) {
  LRUTLBFiltered_params_t *params =
      (LRUTLBFiltered_params_t *)cache->eviction_params;

  cache->n_req += 1;

  /* TLB simulation on the CPU that issued this access */
  params->total_accesses++;
  uint32_t cpu = lrutlbfiltered_get_cpu_feature(req);
  if (cpu >= params->tlb_num_cpus) {
    LOG(ERROR, STREAM_Utils,
        "LRUTLBFiltered: cpu_id %u >= tlb_num_cpus %u\n",
        cpu, params->tlb_num_cpus);
    abort();
  }
  uint64_t page = (uint64_t)req->obj_id;
  bool tlb_hit = lrutlbfiltered_tlb_access(params, (uint8_t)cpu, page);
  if (tlb_hit) {
    params->tlb_hits++;
  } else {
    params->tlb_misses++;
  }

  /* Periodic report */
  if (params->tlb_report_interval > 0 &&
      params->total_accesses - params->last_report_access >=
          params->tlb_report_interval) {
    params->last_report_access = params->total_accesses;
    LOG(INFO, STREAM_Utils,
        "[LRUTLBFiltered @ %luM] accesses=%lu tlb_hits=%lu tlb_misses=%lu "
        "cache_n_obj=%ld",
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
static cache_obj_t *LRUTLBFiltered_find(cache_t *cache, const request_t *req,
                             bool update_cache) {
  LRUTLBFiltered_params_t *params = (LRUTLBFiltered_params_t *)cache->eviction_params;
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
static cache_obj_t *LRUTLBFiltered_insert(cache_t *cache, const request_t *req) {
  LRUTLBFiltered_params_t *params = (LRUTLBFiltered_params_t *)cache->eviction_params;

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
static cache_obj_t *LRUTLBFiltered_to_evict(cache_t *cache, const request_t *req) {
  LRUTLBFiltered_params_t *params = (LRUTLBFiltered_params_t *)cache->eviction_params;

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
static void LRUTLBFiltered_evict(cache_t *cache, const request_t *req) {
  LRUTLBFiltered_params_t *params = (LRUTLBFiltered_params_t *)cache->eviction_params;
  cache_obj_t *obj_to_evict = params->q_tail;
  DEBUG_ASSERT(params->q_tail != NULL);

  /* Invalidate evicted page from ALL per-CPU TLBs */
  lrutlbfiltered_tlb_invalidate_all(
      params, (uint64_t)obj_to_evict->obj_id);

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
    LOG(INFO, STREAM_Utils, "%ld demote %ld %ld", cache->n_req,
        obj_to_evict->create_time, obj_to_evict->next_access_vtime);
#endif

  cache_evict_base(cache, obj_to_evict, true);
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
static bool LRUTLBFiltered_remove(cache_t *cache, obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) {
    return false;
  }
  LRUTLBFiltered_params_t *params = (LRUTLBFiltered_params_t *)cache->eviction_params;

  remove_obj_from_list(&params->q_head, &params->q_tail, obj);
  cache_remove_obj_base(cache, obj, true);

  return true;
}

static void LRUTLBFiltered_print_cache(const cache_t *cache) {
  LRUTLBFiltered_params_t *params = (LRUTLBFiltered_params_t *)cache->eviction_params;
  cache_obj_t *cur = params->q_head;
  if (cur == NULL) {
    LOG(INFO, STREAM_Utils, "empty");
    return;
  }

  char out[4096];
  int written = 0;
  while (cur != NULL) {
    int n = snprintf(out + written, sizeof(out) - (size_t)written, "%lu->",
                     (unsigned long)cur->obj_id);
    if (n < 0 || n >= (int)(sizeof(out) - (size_t)written)) {
      snprintf(out + written, sizeof(out) - (size_t)written, "...");
      written = (int)strlen(out);
      break;
    }
    written += n;
    cur = cur->queue.next;
  }
  snprintf(out + written, sizeof(out) - (size_t)written, "END");
  LOG(INFO, STREAM_Utils, "%s", out);
}

#ifdef __cplusplus
}
#endif
