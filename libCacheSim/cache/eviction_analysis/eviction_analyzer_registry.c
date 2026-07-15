/*
 * eviction_analyzer_registry.c — Registry management for eviction analyzers
 *
 * Handles registration, lifecycle, and notification of analyzers.
 */

#include "libCacheSim/eviction_analyzer.h"
#include "libCacheSim/cache.h"
#include "libCacheSim/log.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Registry embedded in cache_t (struct definition in cache.h)
 * ================================================================ */

/**
 * Initialize an empty registry
 */
static void registry_init(eviction_analyzer_registry_t *reg) {
  reg->analyzers = NULL;
  reg->n_analyzers = 0;
  reg->capacity = 0;
}

/**
 * Free all resources in registry (but not analyzers themselves)
 */
static void registry_destroy(eviction_analyzer_registry_t *reg) {
  free(reg->analyzers);
  reg->analyzers = NULL;
  reg->n_analyzers = 0;
  reg->capacity = 0;
}

/**
 * Grow registry capacity
 */
static bool registry_grow(eviction_analyzer_registry_t *reg) {
  int new_capacity = (reg->capacity > 0) ? reg->capacity * 2 : 4;
  eviction_analyzer_t **new_analyzers = 
      realloc(reg->analyzers, sizeof(eviction_analyzer_t *) * new_capacity);
  
  if (!new_analyzers) {
    LOG(ERROR, STREAM_Cache, "OOM growing analyzer registry (capacity %d → %d)",
        reg->capacity, new_capacity);
    return false;
  }
  
  reg->analyzers = new_analyzers;
  reg->capacity = new_capacity;
  return true;
}

/* ================================================================
 * Public API
 * ================================================================ */

bool cache_register_eviction_analyzer(cache_t *cache,
                                      eviction_analyzer_t *analyzer) {
  if (!cache || !analyzer) {
    return false;
  }

  eviction_analyzer_registry_t *reg = &cache->analyzer_registry;

  /* Grow if needed */
  if (reg->n_analyzers >= reg->capacity) {
    if (!registry_grow(reg)) {
      return false;
    }
  }

  /* Add to registry */
  reg->analyzers[reg->n_analyzers++] = analyzer;
  
  /* Start the analyzer */
  if (analyzer->start) {
    analyzer->start(analyzer);
  }
  
  LOG(DEBUG, STREAM_Cache, "Registered eviction analyzer: %s", analyzer->name);
  return true;
}

void cache_finalize_eviction_analyzers(cache_t *cache,
                                       const char *output_dir) {
  if (!cache) {
    return;
  }

  eviction_analyzer_registry_t *reg = &cache->analyzer_registry;

  /* Finalize all analyzers */
  for (int i = 0; i < reg->n_analyzers; i++) {
    eviction_analyzer_t *analyzer = reg->analyzers[i];
    if (analyzer && analyzer->finalize) {
      analyzer->finalize(analyzer, output_dir);
    }
  }

  /* Free all analyzers */
  for (int i = 0; i < reg->n_analyzers; i++) {
    eviction_analyzer_t *analyzer = reg->analyzers[i];
    if (analyzer && analyzer->free) {
      analyzer->free(analyzer);
    }
  }

  /* Destroy registry */
  registry_destroy(reg);
}

void cache_notify_eviction_analyzers(cache_t *cache,
                                     cache_obj_t *evicted_obj,
                                     request_t *req) {
  if (!cache || !req) {
    return;
  }

  eviction_analyzer_registry_t *reg = &cache->analyzer_registry;

  /* Call all registered analyzers */
  for (int i = 0; i < reg->n_analyzers; i++) {
    eviction_analyzer_t *analyzer = reg->analyzers[i];
    if (analyzer && analyzer->process) {
      analyzer->process(analyzer, evicted_obj, req);
    }
  }
}

/**
 * Call this from cache_create_with_cache_size() or equivalent
 * to initialize the registry
 */
void cache_init_analyzer_registry(cache_t *cache) {
  if (cache) {
    registry_init(&cache->analyzer_registry);
  }
}
