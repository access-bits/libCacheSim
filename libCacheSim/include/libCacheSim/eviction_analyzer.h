/*
 * eviction_analyzer.h — Interface for pluggable eviction analysis
 *
 * Analyzers are called on every request to observe eviction patterns.
 * If an object was evicted on this request, evicted_obj is non-NULL.
 * Otherwise, evicted_obj is NULL (hit, or no eviction needed).
 *
 * Usage:
 *   1. Implement the interface (or use factory to instantiate)
 *   2. Register with cache via cache_register_eviction_analyzer()
 *   3. Analyzer's start() called at cache creation
 *   4. Analyzer's process() called on every request
 *   5. Analyzer's finalize() called at cache free
 *   6. Analyzer's free() called for cleanup
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* obj_id_t mirrored to avoid circular include of cacheObj.h */
#ifndef OBJ_ID_T_DEFINED
#define OBJ_ID_T_DEFINED
typedef uint64_t obj_id_t;
#endif
/* Sentinel meaning "no eviction happened on this request". */
#define OBJ_ID_NONE ((obj_id_t)UINT64_MAX)

/* Forward declarations */
typedef struct cache cache_t;
typedef struct cache_obj cache_obj_t;
typedef struct request request_t;

/* ================================================================
 * Eviction Analyzer Interface
 * ================================================================ */

typedef struct eviction_analyzer {
  char name[128];              /* Human-readable analyzer name */
  void *data;                 /* Opaque analyzer state/context */

  /**
   * Called when analyzer is registered and cache is created.
   * Use to initialize state, open files, etc.
   */
  void (*start)(struct eviction_analyzer *self);

  /**
   * Called on every cache request.
   * @param self              analyzer instance
   * @param evicted_id        obj_id of the evicted object, or OBJ_ID_NONE if no eviction
   * @param req               the request that triggered this (never NULL)
   */
  void (*process)(struct eviction_analyzer *self,
                  obj_id_t evicted_id,
                  request_t *req);

  /**
   * Called when cache is freed, before analyzer cleanup.
   * Use to flush buffers, write final reports, etc.
   * @param self              analyzer instance
   * @param output_dir        directory for output files (may be NULL)
   */
  void (*finalize)(struct eviction_analyzer *self,
                   const char *output_dir);

  /**
   * Called to free all resources owned by analyzer.
   * After this, analyzer must not be used.
   * @param self              analyzer instance
   */
  void (*free)(struct eviction_analyzer *self);
} eviction_analyzer_t;

/* ================================================================
 * Registry Management
 * ================================================================ */

/**
 * Register an analyzer to be called for every request in this cache.
 * Takes ownership of analyzer — will call analyzer->free() at cache free.
 *
 * @param cache    cache instance
 * @param analyzer analyzer to register (non-NULL)
 * @return         true on success, false on OOM
 */
bool cache_register_eviction_analyzer(cache_t *cache,
                                      eviction_analyzer_t *analyzer);

/**
 * Finalize all registered analyzers and free resources.
 * Called internally by cache_free() — do not call manually.
 *
 * @param cache        cache instance
 * @param output_dir   directory for analyzer outputs (may be NULL)
 */
void cache_finalize_eviction_analyzers(cache_t *cache,
                                       const char *output_dir);

/**
 * Internal: Notify all registered analyzers of a request.
 * Reads cache->last_evicted_id, passes it to every analyzer, then resets it to OBJ_ID_NONE.
 *
 * @param cache        cache instance
 * @param req          the request
 */
void cache_notify_eviction_analyzers(cache_t *cache,
                                     request_t *req);

/**
 * Internal: Initialize analyzer registry (called by cache_struct_init).
 *
 * @param cache  cache instance
 */
void cache_init_analyzer_registry(cache_t *cache);

#ifdef __cplusplus
}
#endif
