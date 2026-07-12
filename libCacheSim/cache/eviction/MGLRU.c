//
//  MGLRU.c
//  libCacheSim
//
//  MGLRU-inspired eviction with TLB-filtered reference marking.
//
//  Behavior:
//  1) TLB filtering:
//     - On memory hit + TLB miss: mark object as recently accessed.
//     - On memory hit + TLB hit: do nothing.
//     - If sets=0, TLB always misses.
//  2) Aging:
//     - A new generation is created every window_accesses requests.
//     - At generation creation, marked objects are moved to newest generation
//       in ascending obj_id order.
//     - Memory misses insert object at tail of newest generation.
//  3) Eviction:
//     - Scan oldest generation head->tail for first unmarked object.
//     - If no candidate in oldest generation, try next older generation.
//

#include <assert.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dataStructure/hashtable/hashtable.h"
#include "libCacheSim/evictionAlgo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MGLRU_DEFAULT_WINDOW_ACCESSES 10000UL

#define MGLRU_DEFAULT_TLB_SETS 512U
#define MGLRU_DEFAULT_TLB_WAYS 4U
#define MGLRU_DEFAULT_TLB_NUM_CPUS 4U
#define MGLRU_DEFAULT_REPORT_INTERVAL 10000000UL

typedef struct {
  bool valid;
  uint64_t page;
  uint64_t last_access_time;
} MGLRUSlot;

typedef struct MGLRUGeneration {
  uint64_t seq;
  cache_obj_t *head; /* oldest in this generation */
  cache_obj_t *tail; /* newest in this generation */
  struct MGLRUGeneration *newer;
  struct MGLRUGeneration *older;
} MGLRUGeneration;

typedef struct {
  MGLRUGeneration *newest_gen;
  MGLRUGeneration *oldest_gen;
  uint64_t current_gen_seq;

  uint64_t window_accesses;
  uint64_t accesses_in_window;

  /* Marked set for current window: mark on memory-hit + TLB-miss */
  GHashTable *marked_set;
  obj_id_t *marked_ids;
  int64_t marked_n;
  int64_t marked_cap;

  /* TLB configuration */
  uint32_t tlb_sets; /* 0 means disabled: always miss */
  uint32_t tlb_ways;
  uint32_t tlb_num_cpus;
  uint64_t tlb_report_interval;

  MGLRUSlot *tlb;
  uint64_t tlb_global_time;

  /* Stats */
  uint64_t total_accesses;
  uint64_t total_hits;
  uint64_t total_misses;
  uint64_t tlb_hits;
  uint64_t tlb_misses;
  uint64_t tlb_eviction_invalidations;
  uint64_t generation_creations;
  uint64_t generation_promotions;
  uint64_t last_report_access;
} MGLRU_params_t;

static inline gpointer mglru_obj_id_to_key(obj_id_t id) {
  return (gpointer)(uintptr_t)id;
}

static int mglru_compare_obj_id_asc(const void *a, const void *b) {
  const obj_id_t oa = *(const obj_id_t *)a;
  const obj_id_t ob = *(const obj_id_t *)b;
  if (oa < ob) return -1;
  if (oa > ob) return 1;
  return 0;
}

static inline bool mglru_is_power_of_two(uint32_t x) {
  return x != 0 && (x & (x - 1U)) == 0;
}

static inline MGLRUSlot *mglru_tlb_set(MGLRU_params_t *params,
                                       uint32_t cpu_id,
                                       uint32_t set_idx) {
  size_t base = ((size_t)cpu_id * params->tlb_sets + set_idx) * params->tlb_ways;
  return &params->tlb[base];
}

static inline bool mglru_tlb_access(MGLRU_params_t *params,
                                    uint32_t cpu_id,
                                    uint64_t page) {
  if (params->tlb_sets == 0) {
    return false;
  }

  uint32_t set_idx = (uint32_t)(page & (params->tlb_sets - 1U));
  MGLRUSlot *set = mglru_tlb_set(params, cpu_id, set_idx);

  for (uint32_t w = 0; w < params->tlb_ways; w++) {
    if (set[w].valid && set[w].page == page) {
      set[w].last_access_time = params->tlb_global_time++;
      return true;
    }
  }

  int victim = -1;
  for (uint32_t w = 0; w < params->tlb_ways; w++) {
    if (!set[w].valid) {
      victim = (int)w;
      break;
    }
  }

  if (victim < 0) {
    victim = 0;
    for (uint32_t w = 1; w < params->tlb_ways; w++) {
      if (set[w].last_access_time < set[victim].last_access_time) {
        victim = (int)w;
      }
    }
  }

  set[victim].valid = true;
  set[victim].page = page;
  set[victim].last_access_time = params->tlb_global_time++;
  return false;
}

static inline void mglru_tlb_invalidate_all(MGLRU_params_t *params,
                                            uint64_t page) {
  if (params->tlb_sets == 0) {
    return;
  }

  uint32_t set_idx = (uint32_t)(page & (params->tlb_sets - 1U));
  for (uint32_t cpu = 0; cpu < params->tlb_num_cpus; cpu++) {
    MGLRUSlot *set = mglru_tlb_set(params, cpu, set_idx);
    for (uint32_t w = 0; w < params->tlb_ways; w++) {
      if (set[w].valid && set[w].page == page) {
        set[w].valid = false;
        params->tlb_eviction_invalidations++;
        break;
      }
    }
  }
}

static inline void mglru_marked_reserve(MGLRU_params_t *params, int64_t n) {
  if (params->marked_cap >= n) {
    return;
  }

  int64_t new_cap = params->marked_cap == 0 ? 1024 : params->marked_cap;
  while (new_cap < n) {
    new_cap *= 2;
  }

  params->marked_ids = (obj_id_t *)realloc(params->marked_ids,
                                           (size_t)new_cap * sizeof(obj_id_t));
  params->marked_cap = new_cap;
}

static inline bool mglru_is_marked(MGLRU_params_t *params, obj_id_t obj_id) {
  return g_hash_table_contains(params->marked_set, mglru_obj_id_to_key(obj_id));
}

static inline void mglru_unmark(MGLRU_params_t *params, obj_id_t obj_id) {
  g_hash_table_remove(params->marked_set, mglru_obj_id_to_key(obj_id));
}

static inline void mglru_mark(MGLRU_params_t *params, obj_id_t obj_id) {
  gpointer key = mglru_obj_id_to_key(obj_id);
  if (g_hash_table_contains(params->marked_set, key)) {
    return;
  }

  g_hash_table_add(params->marked_set, key);
  mglru_marked_reserve(params, params->marked_n + 1);
  params->marked_ids[params->marked_n++] = obj_id;
}

static inline void mglru_clear_marks(MGLRU_params_t *params) {
  g_hash_table_remove_all(params->marked_set);
  params->marked_n = 0;
}

static MGLRUGeneration *mglru_create_newest_generation(MGLRU_params_t *params) {
  MGLRUGeneration *gen = (MGLRUGeneration *)calloc(1, sizeof(MGLRUGeneration));
  gen->seq = ++params->current_gen_seq;

  gen->newer = NULL;
  gen->older = params->newest_gen;
  if (params->newest_gen != NULL) {
    params->newest_gen->newer = gen;
  }
  params->newest_gen = gen;
  if (params->oldest_gen == NULL) {
    params->oldest_gen = gen;
  }

  params->generation_creations++;
  return gen;
}

static inline void mglru_remove_generation(MGLRU_params_t *params,
                                           MGLRUGeneration *gen) {
  if (gen->newer != NULL) {
    gen->newer->older = gen->older;
  } else {
    params->newest_gen = gen->older;
  }

  if (gen->older != NULL) {
    gen->older->newer = gen->newer;
  } else {
    params->oldest_gen = gen->newer;
  }

  free(gen);
}

static inline void mglru_add_to_gen_tail(cache_obj_t *obj, MGLRUGeneration *gen) {
  append_obj_to_tail(&gen->head, &gen->tail, obj);
  obj->LruCoarse.bucket = (void *)gen;
  obj->LruCoarse.bucket_idx = -1;
}

static inline void mglru_remove_from_gen(MGLRU_params_t *params, cache_obj_t *obj) {
  MGLRUGeneration *gen = (MGLRUGeneration *)obj->LruCoarse.bucket;
  if (gen == NULL) {
    return;
  }

  remove_obj_from_list(&gen->head, &gen->tail, obj);
  obj->LruCoarse.bucket = NULL;
  obj->LruCoarse.bucket_idx = -1;

  if (gen->head == NULL && gen != params->newest_gen) {
    mglru_remove_generation(params, gen);
  }
}

static void mglru_promote_marked_to_newest(cache_t *cache,
                                           MGLRU_params_t *params) {
  if (params->marked_n == 0) {
    return;
  }

  qsort(params->marked_ids, (size_t)params->marked_n, sizeof(obj_id_t),
        mglru_compare_obj_id_asc);

  MGLRUGeneration *newest = params->newest_gen;
  for (int64_t i = 0; i < params->marked_n; i++) {
    obj_id_t id = params->marked_ids[i];
    cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, id);
    if (obj == NULL) {
      continue;
    }

    MGLRUGeneration *cur_gen = (MGLRUGeneration *)obj->LruCoarse.bucket;
    if (cur_gen == newest) {
      continue;
    }

    mglru_remove_from_gen(params, obj);
    mglru_add_to_gen_tail(obj, newest);
    params->generation_promotions++;
  }
}

static inline void mglru_rotate_generation_if_needed(cache_t *cache,
                                                     MGLRU_params_t *params) {
  if (params->accesses_in_window < params->window_accesses) {
    return;
  }

  params->accesses_in_window = 0;
  mglru_create_newest_generation(params);
  mglru_promote_marked_to_newest(cache, params);
  mglru_clear_marks(params);
}

static cache_obj_t *mglru_pick_victim_scan(MGLRU_params_t *params) {
  for (MGLRUGeneration *gen = params->oldest_gen; gen != NULL; gen = gen->newer) {
    for (cache_obj_t *obj = gen->head; obj != NULL; obj = obj->queue.next) {
      if (!mglru_is_marked(params, obj->obj_id)) {
        return obj;
      }
    }
  }

  return NULL;
}

/* Fallback: pick the head of the oldest generation.
 * Used when all objects are marked (every object was recently accessed). */
static cache_obj_t *mglru_pick_victim_oldest_head(MGLRU_params_t *params) {
  return (params->oldest_gen != NULL) ? params->oldest_gen->head : NULL;
}

static void MGLRU_free(cache_t *cache);
static bool MGLRU_get(cache_t *cache, const request_t *req);
static cache_obj_t *MGLRU_find(cache_t *cache, const request_t *req,
                               bool update_cache);
static cache_obj_t *MGLRU_insert(cache_t *cache, const request_t *req);
static cache_obj_t *MGLRU_to_evict(cache_t *cache, const request_t *req);
static void MGLRU_evict(cache_t *cache, const request_t *req);
static bool MGLRU_remove(cache_t *cache, obj_id_t obj_id);

static void MGLRU_parse_params(const char *cache_specific_params,
                               MGLRU_params_t *params) {
  params->window_accesses = MGLRU_DEFAULT_WINDOW_ACCESSES;
  params->tlb_sets = MGLRU_DEFAULT_TLB_SETS;
  params->tlb_ways = MGLRU_DEFAULT_TLB_WAYS;
  params->tlb_num_cpus = MGLRU_DEFAULT_TLB_NUM_CPUS;
  params->tlb_report_interval = MGLRU_DEFAULT_REPORT_INTERVAL;

  if (cache_specific_params == NULL || cache_specific_params[0] == '\0') {
    goto validate;
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
      LOG(ERROR, STREAM_Utils, "MGLRU: malformed params string %s\n",
          cache_specific_params);
      abort();
    }

    if (strcasecmp(key, "window-accesses") == 0 ||
        strcasecmp(key, "window_accesses") == 0 ||
        strcasecmp(key, "sampling-period") == 0 ||
        strcasecmp(key, "sampling_period") == 0) {
      params->window_accesses = (uint64_t)strtoull(value, NULL, 10);
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
    } else {
      LOG(ERROR, STREAM_Utils, "MGLRU: unknown param %s\n", key);
      abort();
    }
  }

  free(params_str);

validate:
  if (params->window_accesses == 0) {
    LOG(ERROR, STREAM_Utils, "MGLRU: window_accesses must be > 0\n");
    abort();
  }
  if (params->tlb_sets != 0 && !mglru_is_power_of_two(params->tlb_sets)) {
    LOG(ERROR, STREAM_Utils,
        "MGLRU: sets (%u) must be 0 or a power of two\n", params->tlb_sets);
    abort();
  }
  if (params->tlb_ways == 0) {
    LOG(ERROR, STREAM_Utils, "MGLRU: ways must be > 0\n");
    abort();
  }
  if (params->tlb_num_cpus == 0) {
    LOG(ERROR, STREAM_Utils, "MGLRU: num_cpus must be > 0\n");
    abort();
  }
}

cache_t *MGLRU_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params) {
  cache_t *cache = cache_struct_init("MGLRU", ccache_params, cache_specific_params);
  cache->cache_init = MGLRU_init;
  cache->cache_free = MGLRU_free;
  cache->get = MGLRU_get;
  cache->find = MGLRU_find;
  cache->insert = MGLRU_insert;
  cache->evict = MGLRU_evict;
  cache->remove = MGLRU_remove;
  cache->to_evict = MGLRU_to_evict;
  cache->get_occupied_byte = cache_get_occupied_byte_default;
  cache->can_insert = cache_can_insert_default;
  cache->get_n_obj = cache_get_n_obj_default;

  if (ccache_params.consider_obj_metadata) {
    cache->obj_md_size = 8 * 2;
  } else {
    cache->obj_md_size = 0;
  }

  MGLRU_params_t *params = (MGLRU_params_t *)calloc(1, sizeof(MGLRU_params_t));
  MGLRU_parse_params(cache_specific_params, params);

  params->marked_set = g_hash_table_new(g_direct_hash, g_direct_equal);
  params->marked_ids = NULL;
  params->marked_n = 0;
  params->marked_cap = 0;

  if (params->tlb_sets > 0) {
    size_t n_tlb_entries = (size_t)params->tlb_num_cpus * params->tlb_sets *
                           params->tlb_ways;
    params->tlb = (MGLRUSlot *)calloc(n_tlb_entries, sizeof(MGLRUSlot));
    if (params->tlb == NULL) {
      LOG(ERROR, STREAM_Utils,
          "MGLRU: failed to allocate TLB (%u cpus x %u sets x %u ways)\n",
          params->tlb_num_cpus, params->tlb_sets, params->tlb_ways);
      abort();
    }
  } else {
    params->tlb = NULL;
  }

  params->newest_gen = NULL;
  params->oldest_gen = NULL;
  params->current_gen_seq = 0;
  mglru_create_newest_generation(params);

  cache->eviction_params = params;

  LOG(INFO, STREAM_Utils,
      "MGLRU: initialized - window_accesses=%lu, sets=%u, ways=%u, num_cpus=%u",
      (unsigned long)params->window_accesses,
      params->tlb_sets,
      params->tlb_ways,
      params->tlb_num_cpus);

  return cache;
}

static void MGLRU_free(cache_t *cache) {
  MGLRU_params_t *params = (MGLRU_params_t *)cache->eviction_params;

  LOG(INFO, STREAM_Utils, "=== MGLRU Statistics ===");
  LOG(INFO, STREAM_Utils, "Total accesses:         %lu", (unsigned long)params->total_accesses);
  LOG(INFO, STREAM_Utils, "Cache hits:             %lu", (unsigned long)params->total_hits);
  LOG(INFO, STREAM_Utils, "Cache misses:           %lu", (unsigned long)params->total_misses);
  LOG(INFO, STREAM_Utils, "TLB hits:               %lu", (unsigned long)params->tlb_hits);
  LOG(INFO, STREAM_Utils, "TLB misses:             %lu", (unsigned long)params->tlb_misses);
  LOG(INFO, STREAM_Utils, "TLB eviction inval:     %lu",
      (unsigned long)params->tlb_eviction_invalidations);
  LOG(INFO, STREAM_Utils, "Generation creations:   %lu",
      (unsigned long)params->generation_creations);
  LOG(INFO, STREAM_Utils, "Generation promotions:  %lu",
      (unsigned long)params->generation_promotions);
  LOG(INFO, STREAM_Utils, "========================");

  while (params->newest_gen != NULL) {
    MGLRUGeneration *next = params->newest_gen->older;
    free(params->newest_gen);
    params->newest_gen = next;
  }

  if (params->marked_set != NULL) {
    g_hash_table_destroy(params->marked_set);
  }
  free(params->marked_ids);
  free(params->tlb);
  free(params);
  cache_struct_free(cache);
}

static bool MGLRU_get(cache_t *cache, const request_t *req) {
  MGLRU_params_t *params = (MGLRU_params_t *)cache->eviction_params;

  cache->n_req += 1;
  params->total_accesses++;
  params->accesses_in_window++;

  mglru_rotate_generation_if_needed(cache, params);

  uint32_t cpu = req->cpu_id;
  if (params->tlb_sets > 0 && cpu >= params->tlb_num_cpus) {
    LOG(ERROR, STREAM_Utils,
        "MGLRU: cpu_id %u >= tlb_num_cpus %u\n", cpu, params->tlb_num_cpus);
    abort();
  }

  bool tlb_hit = mglru_tlb_access(params, cpu, (uint64_t)req->obj_id);
  if (tlb_hit) {
    params->tlb_hits++;
  } else {
    params->tlb_misses++;
  }

  if (params->tlb_report_interval > 0 &&
      params->total_accesses - params->last_report_access >=
          params->tlb_report_interval) {
    params->last_report_access = params->total_accesses;
  }

  cache_obj_t *obj = cache_find_base(cache, req, false);
  if (obj != NULL) {
    params->total_hits++;
    if (!tlb_hit) {
      mglru_mark(params, obj->obj_id);
    }
    return true;
  }

  params->total_misses++;
  while (cache->get_occupied_byte(cache) + req->obj_size + cache->obj_md_size >
         cache->cache_size) {
    cache->evict(cache, req);
  }

  cache->insert(cache, req);
  return false;
}

static cache_obj_t *MGLRU_find(cache_t *cache, const request_t *req,
                               bool update_cache) {
  return cache_find_base(cache, req, update_cache);
}

static cache_obj_t *MGLRU_insert(cache_t *cache, const request_t *req) {
  MGLRU_params_t *params = (MGLRU_params_t *)cache->eviction_params;
  if (params->newest_gen == NULL) {
    mglru_create_newest_generation(params);
  }

  cache_obj_t *obj = cache_insert_base(cache, req);
  mglru_add_to_gen_tail(obj, params->newest_gen);
  return obj;
}

static cache_obj_t *MGLRU_to_evict(cache_t *cache, const request_t *req) {
  MGLRU_params_t *params = (MGLRU_params_t *)cache->eviction_params;
  return mglru_pick_victim_scan(params);
}

static void MGLRU_evict(cache_t *cache, const request_t *req) {
  MGLRU_params_t *params = (MGLRU_params_t *)cache->eviction_params;
  cache_obj_t *victim = MGLRU_to_evict(cache, req);
  if (victim == NULL) {
    WARN_ONCE("MGLRU: all objects marked, falling back to oldest-gen head\n");
    victim = mglru_pick_victim_oldest_head(params);
    if (victim == NULL) {
      LOG(ERROR, STREAM_Utils, "MGLRU: cache is empty, cannot evict\n");
      abort();
    }
  }

  mglru_tlb_invalidate_all(params, (uint64_t)victim->obj_id);
  mglru_unmark(params, victim->obj_id);
  mglru_remove_from_gen(params, victim);

#if defined(TRACK_DEMOTION)
  if (cache->track_demotion)
    printf("%ld demote %ld %ld\n", cache->n_req, victim->create_time,
           victim->next_access_vtime);
#endif

  cache_evict_base(cache, victim, true);
}

static bool MGLRU_remove(cache_t *cache, obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) {
    return false;
  }

  MGLRU_params_t *params = (MGLRU_params_t *)cache->eviction_params;
  mglru_tlb_invalidate_all(params, (uint64_t)obj->obj_id);
  mglru_unmark(params, obj->obj_id);
  mglru_remove_from_gen(params, obj);
  cache_remove_obj_base(cache, obj, true);
  return true;
}

#ifdef __cplusplus
}
#endif
