//
//  the BeladyLruTlbFiltered eviction algorithm (MIN with LRU-TLB filtering)
//
//  Includes TLB simulation: maintains per-CPU set-associative TLBs with LRU
//  eviction to track two types of violations:
//    Type 1: TLB simulation result disagrees with trace's tlb_miss field
//    Type 2: Belady evicts a page that is currently resident in some TLB
//
//  BeladyLruTlbFiltered.c
//  libCacheSim
//
//
// Created by Juncheng Yang on 3/30/21.
// Modified for LRU-TLB filtering with TLB simulation
//

#include <string.h>
#include "dataStructure/hashtable/hashtable.h"
#include "dataStructure/pqueue.h"
#include "libCacheSim/evictionAlgo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * TLB simulation configuration
 * ===================================================================== */
#define BELADY_TLB_NUM_CPUS  4
#define BELADY_TLB_SETS      512   /* must be power of 2 */
#define BELADY_TLB_WAYS      4
#define BELADY_TLB_REPORT_INTERVAL  10000000UL  /* print stats every 10M accesses */

typedef struct {
  bool     valid;
  uint64_t page;
  uint64_t last_access_time;
} BeladyTlbSlot;

typedef struct BeladyLruTlbFiltered_params {
  /* a priority queue recording the next access time */
  pqueue_t *pq;

  /* TLB simulation state */
  BeladyTlbSlot tlb[BELADY_TLB_NUM_CPUS][BELADY_TLB_SETS][BELADY_TLB_WAYS];
  uint64_t tlb_global_time[BELADY_TLB_NUM_CPUS];

  /* Violation counters */
  uint64_t violation_type1;  /* sim TLB result != trace tlb_miss */
  uint64_t violation_type2;  /* evicted page is in some TLB */
  uint64_t total_accesses;
  uint64_t total_evictions;
  uint64_t last_report_access;  /* total_accesses at last periodic report */
} BeladyLruTlbFiltered_params_t;

// #define EVICT_IMMEDIATELY_IF_NO_FUTURE_ACCESS 1

/* =====================================================================
 * TLB simulation helpers
 * ===================================================================== */

/**
 * Simulate a TLB access for a given CPU.
 * Returns true on TLB hit, false on TLB miss.
 * On miss, installs the page using LRU eviction.
 */
static inline bool belady_tlb_access(BeladyLruTlbFiltered_params_t *params,
                                     uint8_t cpu_id, uint64_t page) {
  if (cpu_id >= BELADY_TLB_NUM_CPUS) cpu_id = 0;
  uint32_t set_idx = (uint32_t)(page & (BELADY_TLB_SETS - 1));
  BeladyTlbSlot *set = params->tlb[cpu_id][set_idx];

  /* Lookup */
  for (int w = 0; w < BELADY_TLB_WAYS; w++) {
    if (set[w].valid && set[w].page == page) {
      set[w].last_access_time = params->tlb_global_time[cpu_id]++;
      return true;  /* hit */
    }
  }

  /* Miss: find an invalid slot first */
  int victim = -1;
  for (int w = 0; w < BELADY_TLB_WAYS; w++) {
    if (!set[w].valid) { victim = w; break; }
  }

  /* Otherwise evict the LRU slot */
  if (victim < 0) {
    victim = 0;
    for (int w = 1; w < BELADY_TLB_WAYS; w++) {
      if (set[w].last_access_time < set[victim].last_access_time)
        victim = w;
    }
  }

  set[victim].valid = true;
  set[victim].page = page;
  set[victim].last_access_time = params->tlb_global_time[cpu_id]++;
  return false;  /* miss */
}

/**
 * Check if a page is resident in any TLB (any CPU).
 * Returns true if found in at least one TLB.
 */
static inline bool belady_tlb_is_resident(BeladyLruTlbFiltered_params_t *params,
                                          uint64_t page) {
  for (int cpu = 0; cpu < BELADY_TLB_NUM_CPUS; cpu++) {
    uint32_t set_idx = (uint32_t)(page & (BELADY_TLB_SETS - 1));
    BeladyTlbSlot *set = params->tlb[cpu][set_idx];
    for (int w = 0; w < BELADY_TLB_WAYS; w++) {
      if (set[w].valid && set[w].page == page) {
        return true;
      }
    }
  }
  return false;
}

// ***********************************************************************
// ****                                                               ****
// ****                   function declarations                       ****
// ****                                                               ****
// ***********************************************************************

static void BeladyLruTlbFiltered_free(cache_t *cache);
static bool BeladyLruTlbFiltered_get(cache_t *cache, const request_t *req);
static cache_obj_t *BeladyLruTlbFiltered_find(cache_t *cache, const request_t *req,
                                bool update_cache);
static cache_obj_t *BeladyLruTlbFiltered_insert(cache_t *cache, const request_t *req);
static cache_obj_t *BeladyLruTlbFiltered_to_evict(cache_t *cache, const request_t *req);
static void BeladyLruTlbFiltered_evict(cache_t *cache, const request_t *req);
static bool BeladyLruTlbFiltered_remove(cache_t *cache, obj_id_t obj_id);
static void BeladyLruTlbFiltered_remove_obj(cache_t *cache, cache_obj_t *obj);

// ***********************************************************************
// ****                                                               ****
// ****                   end user facing functions                   ****
// ****                                                               ****
// ****                       init, free, get                         ****
// ***********************************************************************

/**
 * @brief initialize a BeladyLruTlbFiltered cache
 *
 * @param ccache_params some common cache parameters
 * @param cache_specific_params BeladyLruTlbFiltered specific parameters, should be NULL
 */
cache_t *BeladyLruTlbFiltered_init(const common_cache_params_t ccache_params,

                     const char *cache_specific_params) {
  cache_t *cache =
      cache_struct_init("BeladyLruTlbFiltered", ccache_params, cache_specific_params);
  cache->cache_init = BeladyLruTlbFiltered_init;
  cache->cache_free = BeladyLruTlbFiltered_free;
  cache->get = BeladyLruTlbFiltered_get;
  cache->find = BeladyLruTlbFiltered_find;
  cache->insert = BeladyLruTlbFiltered_insert;
  cache->evict = BeladyLruTlbFiltered_evict;
  cache->to_evict = BeladyLruTlbFiltered_to_evict;
  cache->remove = BeladyLruTlbFiltered_remove;

  BeladyLruTlbFiltered_params_t *params = my_malloc(BeladyLruTlbFiltered_params_t);
  memset(params, 0, sizeof(BeladyLruTlbFiltered_params_t));
  cache->eviction_params = params;

  params->pq = pqueue_init((unsigned long)8e6);
  return cache;
}

/**
 * free resources used by this cache
 *
 * @param cache
 */
static void BeladyLruTlbFiltered_free(cache_t *cache) {
  BeladyLruTlbFiltered_params_t *params = cache->eviction_params;

  /* Print statistics */
  printf("=== BeladyLruTlbFiltered Statistics ===\n");
  printf("Total accesses:       %lu\n", (unsigned long)params->total_accesses);
  printf("Total evictions:      %lu\n", (unsigned long)params->total_evictions);
  // printf("Violation Type 1 (TLB sim mismatch):   %lu\n",
  //        (unsigned long)params->violation_type1);
  // printf("Violation Type 2 (evict TLB-resident): %lu\n",
  //        (unsigned long)params->violation_type2);
  printf("=======================================\n");

  pq_node_t *node = pqueue_pop(params->pq);
  while (node) {
    my_free(sizeof(pq_node_t), node);
    node = pqueue_pop(params->pq);
  }
  pqueue_free(params->pq);
  my_free(sizeof(BeladyLruTlbFiltered_params_t), params);

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
 */
static bool BeladyLruTlbFiltered_get(cache_t *cache, const request_t *req) {
  /* -2 means the trace does not have next_access ts information */
  DEBUG_ASSERT(req->next_access_vtime != -2);
  BeladyLruTlbFiltered_params_t *params = cache->eviction_params;

  /* TLB simulation: commented out — re-simulation on merged trace does not
   * match per-CPU TLB results from the preprocessing pipeline.
   */
  params->total_accesses++;
  // bool sim_hit = belady_tlb_access(params, req->cpu_id, (uint64_t)req->obj_id);
  // bool trace_miss = (req->tlb_miss != 0);
  // if (sim_hit == trace_miss) {
  //   /* sim says hit but trace says miss, or vice versa */
  //   params->violation_type1++;
  // }

  /* Periodic report */
  if (params->total_accesses - params->last_report_access >= BELADY_TLB_REPORT_INTERVAL) {
    params->last_report_access = params->total_accesses;
    printf("[BeladyLruTlbFiltered @ %luM] accesses=%lu evictions=%lu\n",
           (unsigned long)(params->total_accesses / 1000000),
           (unsigned long)params->total_accesses,
           (unsigned long)params->total_evictions);
  }

  DEBUG_ASSERT(cache->n_obj == (int64_t)params->pq->size - 1);
  bool ret = cache_get_base(cache, req);

  return ret;
}

// ***********************************************************************
// ****                                                               ****
// ****       developer facing APIs (used by cache developer)         ****
// ****                                                               ****
// ***********************************************************************

/**
 * @brief find an object in the cache
 *
 * @param cache
 * @param req
 * @param update_cache whether to update the cache,
 *  if true, the object is promoted
 *  and if the object is expired, it is removed from the cache
 * @return the object or NULL if not found
 */
static cache_obj_t *BeladyLruTlbFiltered_find(cache_t *cache, const request_t *req,
                                bool update_cache) {
  BeladyLruTlbFiltered_params_t *params = cache->eviction_params;
  cache_obj_t *cached_obj = cache_find_base(cache, req, update_cache);

  if (!update_cache) return cached_obj;

  if (cached_obj == NULL) {
    return NULL;
  }

  cached_obj->BeladyLruTlbFiltered.next_access_vtime = req->next_access_vtime;
  pqueue_pri_t pri = {.pri = req->next_access_vtime};
  pqueue_change_priority(params->pq, pri,
                         (pq_node_t *)(cached_obj->BeladyLruTlbFiltered.pq_node));
  DEBUG_ASSERT(
      ((pq_node_t *)hashtable_find(cache->hashtable, req)->BeladyLruTlbFiltered.pq_node)
          ->pri.pri == req->next_access_vtime);

#if defined(EVICT_IMMEDIATELY_IF_NO_FUTURE_ACCESS)
  if (req->next_access_vtime == INT64_MAX) {
    BeladyLruTlbFiltered_evict(cache, req);
  }
#endif

  return cached_obj;
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
static cache_obj_t *BeladyLruTlbFiltered_insert(cache_t *cache, const request_t *req) {
  BeladyLruTlbFiltered_params_t *params = cache->eviction_params;

  if (req->next_access_vtime == -1) {
    ERROR("next access time is -1, please use INT64_MAX instead\n");
  }

  cache_obj_t *cached_obj = cache_insert_base(cache, req);

  pq_node_t *node = my_malloc(pq_node_t);
  node->obj_id = req->obj_id;
  node->pri.pri = req->next_access_vtime;
  pqueue_insert(params->pq, (void *)node);
  cached_obj->BeladyLruTlbFiltered.pq_node = node;
  cached_obj->BeladyLruTlbFiltered.next_access_vtime = req->next_access_vtime;

  DEBUG_ASSERT(
      ((pq_node_t *)hashtable_find(cache->hashtable, req)->BeladyLruTlbFiltered.pq_node)
          ->pri.pri == req->next_access_vtime);

#if defined(EVICT_IMMEDIATELY_IF_NO_FUTURE_ACCESS)
  if (req->next_access_vtime == INT64_MAX) {
    BeladyLruTlbFiltered_evict(cache, req);
  }
#endif

  return cached_obj;
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
static cache_obj_t *BeladyLruTlbFiltered_to_evict(cache_t *cache, const request_t *req) {
  BeladyLruTlbFiltered_params_t *params = cache->eviction_params;
  pq_node_t *node = (pq_node_t *)pqueue_peek(params->pq);
  return hashtable_find_obj_id(cache->hashtable, node->obj_id);
}

/**
 * @brief evict an object from the cache
 * it needs to call cache_evict_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param req not used
 */
static void BeladyLruTlbFiltered_evict(cache_t *cache, const request_t *req) {
  BeladyLruTlbFiltered_params_t *params = cache->eviction_params;
  pq_node_t *node = (pq_node_t *)pqueue_pop(params->pq);

  cache_obj_t *obj_to_evict =
      hashtable_find_obj_id(cache->hashtable, node->obj_id);
  DEBUG_ASSERT(node == obj_to_evict->BeladyLruTlbFiltered.pq_node);

  /* Violation type 2: commented out — TLB sim disabled */
  params->total_evictions++;
  // if (belady_tlb_is_resident(params, (uint64_t)obj_to_evict->obj_id)) {
  //   params->violation_type2++;
  // }

  obj_to_evict->BeladyLruTlbFiltered.pq_node = NULL;
  my_free(sizeof(pq_node_t), node);

  cache_evict_base(cache, obj_to_evict, true);
}

static void BeladyLruTlbFiltered_remove_obj(cache_t *cache, cache_obj_t *obj) {
  BeladyLruTlbFiltered_params_t *params = cache->eviction_params;
  DEBUG_ASSERT(obj != NULL);

  if (obj->BeladyLruTlbFiltered.pq_node != NULL) {
    /* if it is NULL, it means we have deleted the entry in pq before this */
    pqueue_remove(params->pq, obj->BeladyLruTlbFiltered.pq_node);
    my_free(sizeof(pq_node_t), obj->BeladyLruTlbFiltered.pq_node);
    obj->BeladyLruTlbFiltered.pq_node = NULL;
  }

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
static bool BeladyLruTlbFiltered_remove(cache_t *cache, obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) {
    return false;
  }

  BeladyLruTlbFiltered_remove_obj(cache, obj);
  return true;
}

#ifdef __cplusplus
}
#endif
