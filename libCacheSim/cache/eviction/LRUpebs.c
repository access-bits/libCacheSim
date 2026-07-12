//
//  LRUpebs — LRU with periodic stack updates
//
//  LRU that only updates the stack every sampling_period accesses.
//  This simulates a periodic sampling approach (like Performance Event-Based Sampling).
//
//  Runtime parameters (via --eviction-params):
//    sampling_period=N     number of accesses per update period (default 1)
//
//  Usage:
//    ./bin/cachesim <trace> mergedTrace lrupebs <size> 
//        --ignore-obj-size 1 --eviction-params "sampling_period=10000"
//
//  LRUpebs.c
//  libCacheSim
//

#include <string.h>
#include <stdlib.h>

#include "dataStructure/hashtable/hashtable.h"
#include "libCacheSim/evictionAlgo.h"

#ifdef __cplusplus
extern "C" {
#endif

// #define USE_BELADY

// ***********************************************************************
// ****                                                               ****
// ****                   function declarations                       ****
// ****                                                               ****
// ***********************************************************************

static void LRUpebs_free(cache_t *cache);
static bool LRUpebs_get(cache_t *cache, const request_t *req);
static cache_obj_t *LRUpebs_find(cache_t *cache, const request_t *req,
                             bool update_cache);
static cache_obj_t *LRUpebs_insert(cache_t *cache, const request_t *req);
static cache_obj_t *LRUpebs_to_evict(cache_t *cache, const request_t *req);
static void LRUpebs_evict(cache_t *cache, const request_t *req);
static bool LRUpebs_remove(cache_t *cache, obj_id_t obj_id);
static void LRUpebs_print_cache(const cache_t *cache);

/* =====================================================================
 * Data structure for LRUpebs
 * ===================================================================== */
typedef struct {
  cache_obj_t *q_head;
  cache_obj_t *q_tail;
  uint64_t sampling_period_size;   /* update stack every N accesses */
  uint64_t accesses_in_period;     /* current count */
} LRUpebs_params_t;

/* =====================================================================
 * Parameter parsing
 * ===================================================================== */
static void LRUpebs_parse_params(const char *cache_specific_params,
                                 LRUpebs_params_t *params) {
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

    if (strcasecmp(key, "sampling-period") == 0 ||
      strcasecmp(key, "sampling_period") == 0) {
      params->sampling_period_size = (uint64_t)strtoull(value, NULL, 10);
      if (params->sampling_period_size == 0) params->sampling_period_size = 1;
    } else {
      LOG(ERROR, STREAM_Utils, "LRUpebs: unknown param \"%s\"\n", key);
      abort();
    }
  }
  free(params_str);
}

// ***********************************************************************
// ****                                                               ****
// ****                   end user facing functions                   ****
// ****                                                               ****
// ****                       init, free, get                         ****
// ***********************************************************************
/**
 * @brief initialize a LRUpebs cache
 *
 * @param ccache_params some common cache parameters
 * @param cache_specific_params LRUpebs specific parameters
 */
cache_t *LRUpebs_init(const common_cache_params_t ccache_params,
                      const char *cache_specific_params) {
  cache_t *cache =
      cache_struct_init("LRUpebs", ccache_params, cache_specific_params);
  cache->cache_init = LRUpebs_init;
  cache->cache_free = LRUpebs_free;
  cache->get = LRUpebs_get;
  cache->find = LRUpebs_find;
  cache->insert = LRUpebs_insert;
  cache->evict = LRUpebs_evict;
  cache->remove = LRUpebs_remove;
  cache->to_evict = LRUpebs_to_evict;
  cache->get_occupied_byte = cache_get_occupied_byte_default;
  cache->can_insert = cache_can_insert_default;
  cache->get_n_obj = cache_get_n_obj_default;
  cache->print_cache = LRUpebs_print_cache;

  if (ccache_params.consider_obj_metadata) {
    cache->obj_md_size = 8 * 2;
  } else {
    cache->obj_md_size = 0;
  }

#ifdef USE_BELADY
  snprintf(cache->cache_name, CACHE_NAME_ARRAY_LEN, "LRUpebs_Belady");
#endif

  LRUpebs_params_t *params = malloc(sizeof(LRUpebs_params_t));
  LRUpebs_parse_params(cache_specific_params, params);
  params->q_head = NULL;
  params->q_tail = NULL;
  params->accesses_in_period = 0;
  cache->eviction_params = params;

  LOG(INFO, STREAM_Utils, "LRUpebs: initialized — sampling_period=%lu\n",
      (unsigned long)params->sampling_period_size);

  return cache;
}

/**
 * free resources used by this cache
 *
 * @param cache
 */
static void LRUpebs_free(cache_t *cache) {
  LRUpebs_params_t *params = (LRUpebs_params_t *)cache->eviction_params;
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
 */
static bool LRUpebs_get(cache_t *cache, const request_t *req) {
  return cache_get_base(cache, req);
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
 *  if true and sampling period is satisfied, the object is promoted
 *  and if the object is expired, it is removed from the cache
 * @return true on hit, false on miss
 */
static cache_obj_t *LRUpebs_find(cache_t *cache, const request_t *req,
                                 bool update_cache) {
  LRUpebs_params_t *params = (LRUpebs_params_t *)cache->eviction_params;
  cache_obj_t *cache_obj = cache_find_base(cache, req, update_cache);

  if (cache_obj && likely(update_cache)) {
    /* Increment access counter for this period */
    params->accesses_in_period++;

    /* Only update LRU stack if sampling period is reached */
    if (params->accesses_in_period >= params->sampling_period_size) {
#ifdef USE_BELADY
      if (req->next_access_vtime != INT64_MAX)
#endif
        move_obj_to_head(&params->q_head, &params->q_tail, cache_obj);
      
      /* Reset counter for next period */
      params->accesses_in_period = 0;
    }
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
static cache_obj_t *LRUpebs_insert(cache_t *cache, const request_t *req) {
  LRUpebs_params_t *params = (LRUpebs_params_t *)cache->eviction_params;

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
static cache_obj_t *LRUpebs_to_evict(cache_t *cache, const request_t *req) {
  LRUpebs_params_t *params = (LRUpebs_params_t *)cache->eviction_params;

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
static void LRUpebs_evict(cache_t *cache, const request_t *req) {
  LRUpebs_params_t *params = (LRUpebs_params_t *)cache->eviction_params;
  cache_obj_t *obj_to_evict = params->q_tail;
  DEBUG_ASSERT(params->q_tail != NULL);

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
static void LRUpebs_remove_obj(cache_t *cache, cache_obj_t *obj) {
  assert(obj != NULL);

  LRUpebs_params_t *params = (LRUpebs_params_t *)cache->eviction_params;

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
static bool LRUpebs_remove(cache_t *cache, obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) {
    return false;
  }
  LRUpebs_params_t *params = (LRUpebs_params_t *)cache->eviction_params;

  remove_obj_from_list(&params->q_head, &params->q_tail, obj);
  cache_remove_obj_base(cache, obj, true);

  return true;
}

static void LRUpebs_print_cache(const cache_t *cache) {
  LRUpebs_params_t *params = (LRUpebs_params_t *)cache->eviction_params;
  cache_obj_t *cur = params->q_head;
  // print from the most recent to the least recent
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
