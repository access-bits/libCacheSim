/* eviction_analyzer_factory.c — Create eviction analyzers from config
 *
 * Single dispatch table for instantiating analyzers by type name.
 * To add a new analyzer: add one line to the analyzer_registry table.
 */

#include "libCacheSim/eviction_analyzer_factory.h"
#include "libCacheSim/eviction_analyzer.h"
#include "custom_analyzers/filtered_trace_monitor.h"

#include "libCacheSim/cache.h"
#include "libCacheSim/log.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Analyzer registry — single table for all analyzer types
 *
 * To add a new analyzer:
 *   1. Implement it in custom_analyzers/my_analyzer.c/.h
 *   2. Add function prototype to this file
 *   3. Add one entry to analyzer_registry[] below
 * ================================================================ */

typedef eviction_analyzer_t *(*analyzer_factory_func)(const char *name,
                                                       const char *params);

typedef struct {
  const char *type;
  analyzer_factory_func create;
} analyzer_registry_entry_t;

static const analyzer_registry_entry_t analyzer_registry[] = {
    {"filtered_trace_monitor", filtered_trace_monitor_create},
};

static const int N_REGISTERED_ANALYZERS =
    sizeof(analyzer_registry) / sizeof(analyzer_registry[0]);

/* ================================================================
 * Factory dispatcher
 * ================================================================ */

static eviction_analyzer_t *create_analyzer_by_type(
    const eviction_analyzer_config_t *cfg) {
  /* Find analyzer type in registry */
  for (int i = 0; i < N_REGISTERED_ANALYZERS; i++) {
    if (strcmp(analyzer_registry[i].type, cfg->type) == 0) {
      return analyzer_registry[i].create(cfg->name, cfg->params);
    }
  }

  LOG(ERROR, STREAM_Cache, "Unknown analyzer type: %s", cfg->type);
  return NULL;
}

/* ================================================================
 * Public API
 * ================================================================ */

bool setup_eviction_analyzers(cache_t *cache,
                              const eviction_analyzer_config_t *configs,
                              int n_analyzers) {
  if (!cache || n_analyzers == 0) {
    return true;  /* No analyzers to set up */
  }

  for (int i = 0; i < n_analyzers; i++) {
    eviction_analyzer_t *analyzer = create_analyzer_by_type(&configs[i]);
    if (!analyzer) {
      LOG(ERROR, STREAM_Cache, "Failed to create analyzer: %s", configs[i].type);
      return false;
    }

    if (!cache_register_eviction_analyzer(cache, analyzer)) {
      LOG(ERROR, STREAM_Cache, "Failed to register analyzer: %s", configs[i].name);
      analyzer->free(analyzer);
      return false;
    }
  }

  LOG(INFO, STREAM_Cache, "Set up %d eviction analyzer(s)", n_analyzers);
  return true;
}
