/* eviction_analyzer_factory.h — Create eviction analyzers from config
 *
 * Factory function to instantiate eviction analyzers by type name.
 */

#pragma once

#include "libCacheSim/cache.h"
#include "libCacheSim/sim_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Instantiate and register all eviction analyzers for a cache
 * based on the per-config analyzer specifications.
 *
 * @param cache        target cache
 * @param config       per-config analyzer array (from sim_config_t)
 * @param n_analyzers  number of analyzers in config
 * @return             true on success, false on failure
 */
bool setup_eviction_analyzers(cache_t *cache,
                              const eviction_analyzer_config_t *config,
                              int n_analyzers);

#ifdef __cplusplus
}
#endif
