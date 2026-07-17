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
#define MGLRU_CPU_FEATURE_IDX 0

/* Filtered trace mode feature indices */
#define MGLRU_TAG_FEATURE_IDX 1
#define MGLRU_FREQUENCY_FEATURE_IDX 2

static inline uint32_t mglru_get_cpu_feature(const request_t *req) {
  if (req->n_features <= MGLRU_CPU_FEATURE_IDX) {
    return 0;
  }
  return (uint32_t)(uint8_t)req->features[MGLRU_CPU_FEATURE_IDX];
}

static inline uint8_t mglru_get_tag(const request_t *req) {
  if (req->n_features <= MGLRU_TAG_FEATURE_IDX) {
    return 0;
  }
  return (uint8_t)req->features[MGLRU_TAG_FEATURE_IDX];
}

static inline uint32_t mglru_get_frequency(const request_t *req) {
  if (req->n_features <= MGLRU_FREQUENCY_FEATURE_IDX) {
    return 1;  /* Default frequency if not provided */
  }
  return (uint32_t)req->features[MGLRU_FREQUENCY_FEATURE_IDX];
}

/* One entry in the software TLB used by MGLRU's optional filtering stage. */
typedef struct {
  bool valid;
  uint64_t page;
  uint64_t last_access_time;
} MGLRUSlot;

/*
 * One generation in MGLRU.
 * Objects are linked in FIFO order inside a generation, and generations are
 * linked oldest <-> newest.
 */
typedef struct MGLRUGeneration {
  uint64_t seq;
  cache_obj_t *head; 
  cache_obj_t *tail; 
  struct MGLRUGeneration *newer;
  struct MGLRUGeneration *older;
} MGLRUGeneration;

/*
 * Per-cache-instance MGLRU state.
 * This is the central state block for generation layout, mark tracking,
 * optional TLB metadata, and run-time counters.
 */
typedef struct {
  /* Generation chain anchors and monotonic sequence ID. */
  MGLRUGeneration *newest_gen;
  MGLRUGeneration *oldest_gen;
  uint64_t current_gen_seq;

  /* Access window controls for triggering generation rotation. */
  uint64_t window_accesses;
  uint64_t accesses_in_window;

  /* Mode selection */
  bool filtered_trace;  /* If true, use tag/frequency from features; if false, use TLB filtering */

  /* Mark state accumulated within the current window. */
  GHashTable *marked_set;
  /* Fast count of currently marked resident objects. */
  uint64_t marked_count;

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
  /* GLib direct hash keys are pointer-sized integers. */
  return (gpointer)(uintptr_t)id;
}

static inline bool mglru_is_power_of_two(uint32_t x) {
  return x != 0 && (x & (x - 1U)) == 0;
}

/* Get the first slot of one CPU/set slice in the flattened TLB array. */
static inline MGLRUSlot *mglru_tlb_set(MGLRU_params_t *params,
                                       uint32_t cpu_id,
                                       uint32_t set_idx) {
  size_t base = ((size_t)cpu_id * params->tlb_sets + set_idx) * params->tlb_ways;
  return &params->tlb[base];
}

static inline bool mglru_tlb_access(MGLRU_params_t *params,
                                    uint32_t cpu_id,
                                    uint64_t page) {
  /* TLB disabled: force misses and skip TLB metadata updates. */
  if (params->tlb_sets == 0 || params->tlb == NULL) {
    return false;
  }

  uint32_t set_idx = (uint32_t)(page & (params->tlb_sets - 1U));
  MGLRUSlot *set = mglru_tlb_set(params, cpu_id, set_idx);

  /* Hit path: refresh age and return true. */
  for (uint32_t w = 0; w < params->tlb_ways; w++) {
    if (set[w].valid && set[w].page == page) {
      set[w].last_access_time = params->tlb_global_time++;
      return true;
    }
  }

  /* Miss path: use empty way first; otherwise evict least-recently-used way. */
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
  /* Remove an evicted page from all CPU-local TLBs. */
  if (params->tlb_sets == 0 || params->tlb == NULL) {
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

/* Unmark on eviction/remove so mark_count reflects current residency. */
static inline void mglru_unmark(MGLRU_params_t *params, obj_id_t obj_id) {
  if (g_hash_table_remove(params->marked_set, mglru_obj_id_to_key(obj_id))) {
    params->marked_count--;
  }
}

/* Mark object once per window. */
static inline void mglru_mark(MGLRU_params_t *params, obj_id_t obj_id) {
  gpointer key = mglru_obj_id_to_key(obj_id);
  if (g_hash_table_contains(params->marked_set, key)) {
    return;
  }
  g_hash_table_add(params->marked_set, key);
  params->marked_count++;
}

/* Clear all per-window mark state after rotation/promotion. */
static inline void mglru_clear_marks(MGLRU_params_t *params) {
  g_hash_table_remove_all(params->marked_set);
  params->marked_count = 0;
}

/* Append a brand-new newest generation and advance sequence number. */
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

/* Remove an empty generation from the doubly linked generation chain. */
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

/* Place object at tail of a generation and update embedded pointers. */
static inline void mglru_add_to_gen_tail(cache_obj_t *obj, MGLRUGeneration *gen) {
  append_obj_to_tail(&gen->head, &gen->tail, obj);
  obj->LruCoarse.bucket = (void *)gen;
  obj->LruCoarse.bucket_idx = -1;
}

/* Detach object from its generation; drop empty non-newest generations. */
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

/* Fast path: promote touched object immediately. */
static inline void mglru_promote_obj_to_newest(MGLRU_params_t *params,
                                               cache_obj_t *obj) {
  MGLRUGeneration *newest = params->newest_gen;
  MGLRUGeneration *cur_gen = (MGLRUGeneration *)obj->LruCoarse.bucket;
  if (cur_gen == NULL || cur_gen == newest) {
    return;
  }

  mglru_remove_from_gen(params, obj);
  mglru_add_to_gen_tail(obj, newest);
  params->generation_promotions++;
}

/* Rotate generations when window fills: create newest, promote, clear marks. */
static inline void mglru_rotate_generation_if_needed(cache_t *cache,
                                                     MGLRU_params_t *params) {
  if (params->accesses_in_window < params->window_accesses) {
    return;
  }

  params->accesses_in_window = 0;
  mglru_create_newest_generation(params);
  mglru_clear_marks(params);
}

/* Primary victim search: evict from oldest non-empty generation, excluding newest. */
static cache_obj_t *mglru_pick_victim_scan(MGLRU_params_t *params) {
  for (MGLRUGeneration *gen = params->oldest_gen; gen != NULL; gen = gen->newer) {
    if (gen == params->newest_gen) 
      break;
    if (gen->head != NULL) 
      return gen->head;
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

/* Parse cache-specific parameters string into validated policy state. */
static void MGLRU_parse_params(const char *cache_specific_params,
                               MGLRU_params_t *params) {
  /* Defaults favor non-filtered mode with a set-associative TLB. */
  params->window_accesses = MGLRU_DEFAULT_WINDOW_ACCESSES;
  params->tlb_sets = MGLRU_DEFAULT_TLB_SETS;
  params->tlb_ways = MGLRU_DEFAULT_TLB_WAYS;
  params->tlb_num_cpus = MGLRU_DEFAULT_TLB_NUM_CPUS;
  params->tlb_report_interval = MGLRU_DEFAULT_REPORT_INTERVAL;
  params->filtered_trace = false;  /* Default: use TLB filtering */

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
    } else if (strcasecmp(key, "filtered-trace") == 0 ||
               strcasecmp(key, "filtered_trace") == 0) {
      params->filtered_trace = (strcasecmp(value, "true") == 0 || strcasecmp(value, "1") == 0);
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
  /* Parameter invariants checked before serving requests. */
  if (params->window_accesses == 0) {
    LOG(ERROR, STREAM_Utils, "MGLRU: window_accesses must be > 0\n");
    abort();
  }
  if (!params->filtered_trace) {
    /* TLB validation only needed for non-filtered mode */
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
}

cache_t *MGLRU_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params) {
  /* Register MGLRU callbacks into generic cache_t vtable. */
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
  /* Fill params from string and then allocate runtime containers. */
  MGLRU_parse_params(cache_specific_params, params);

  params->marked_set = g_hash_table_new(g_direct_hash, g_direct_equal);
  params->marked_count = 0;

  if (!params->filtered_trace && params->tlb_sets > 0) {
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
  /* Print summary counters and free all generation/mark/TLB structures. */
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
  free(params->tlb);
  free(params);
  cache_struct_free(cache);
}

static bool MGLRU_get(cache_t *cache, const request_t *req) {
  /*
   * Request path:
   * 1) account access and rotate generation if needed,
   * 2) hit handling and mode-specific marking,
   * 3) miss handling with repeated evict-until-fit then insert.
   */
  MGLRU_params_t *params = (MGLRU_params_t *)cache->eviction_params;

  cache->n_req += 1;
  params->total_accesses++;

  if (params->filtered_trace) {
    /* Filtered trace mode: use tag and frequency from features */
    uint8_t tag = mglru_get_tag(req);
    if (tag == 0) {
      params->accesses_in_window++;
    } else if (tag == 1 || tag == 2) {
      uint32_t frequency = mglru_get_frequency(req);
      if (frequency > 1) {
        params->accesses_in_window += (frequency - 1);
      }
    }
  } else {
    params->accesses_in_window++;
  }

  mglru_rotate_generation_if_needed(cache, params);

  if (!params->filtered_trace) {
    /* TLB filtering logic - only used in non-filtered mode */
    uint32_t cpu = mglru_get_cpu_feature(req);
    if (params->tlb_sets > 0 && cpu >= params->tlb_num_cpus) {
      LOG(WARN, STREAM_Utils,
          "MGLRU: cpu_id %u >= tlb_num_cpus %u\n", cpu,
          params->tlb_num_cpus);
      cache_request_worker_exit(cache, 2, "MGLRU: cpu_id >= tlb_num_cpus");
      return false;
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
        mglru_promote_obj_to_newest(params, obj);
      }
      return true;
    }
  } else {
    /* Filtered trace mode: mark when tag is 1 or 2 */
    uint8_t tag = mglru_get_tag(req);
    cache_obj_t *obj = cache_find_base(cache, req, false);
    if (obj != NULL) {
      params->total_hits++;
      mglru_mark(params, obj->obj_id);
      mglru_promote_obj_to_newest(params, obj);
      return true;
    }
  }

  params->total_misses++;
  while (cache->get_occupied_byte(cache) + req->obj_size + cache->obj_md_size >
         cache->cache_size) {
    cache->evict(cache, req);
    if (cache_should_worker_exit(cache)) return false;
  }

  cache->insert(cache, req);
  return false;
}

static cache_obj_t *MGLRU_find(cache_t *cache, const request_t *req,
                               bool update_cache) {
  /* Shared lookup helper; MGLRU metadata updates happen in get/insert/evict. */
  return cache_find_base(cache, req, update_cache);
}

static cache_obj_t *MGLRU_insert(cache_t *cache, const request_t *req) {
  /* New resident always enters at newest generation tail. */
  MGLRU_params_t *params = (MGLRU_params_t *)cache->eviction_params;
  if (params->newest_gen == NULL) {
    mglru_create_newest_generation(params);
  }

  cache_obj_t *obj = cache_insert_base(cache, req);
  mglru_add_to_gen_tail(obj, params->newest_gen);
  return obj;
}

static cache_obj_t *MGLRU_to_evict(cache_t *cache, const request_t *req) {
  /* Thin wrapper kept for consistency with cache interface expectations. */
  MGLRU_params_t *params = (MGLRU_params_t *)cache->eviction_params;
  return mglru_pick_victim_scan(params);
}

static void MGLRU_evict(cache_t *cache, const request_t *req) {
  /* Evict oldest resident from generation chain. */
  MGLRU_params_t *params = (MGLRU_params_t *)cache->eviction_params;
  cache_obj_t *victim = MGLRU_to_evict(cache, req);

  if (victim == NULL) {
    LOG(WARN, STREAM_Utils,
        "MGLRU: no victim found; requesting worker stop\n");
    cache_request_worker_exit(cache, 3,
                              "MGLRU eviction returned no victim");
    return;
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
  /* Explicit object removal path used by generic cache operations. */
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
