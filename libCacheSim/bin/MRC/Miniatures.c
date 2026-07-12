#include <math.h>

#include "cache/cacheUtils.h"
#include "libCacheSim/evictionAlgo.h"
#include "libCacheSim/plugin.h"
#include "libCacheSim/simulator.h"
#include "mrc_internal.h"
#include "utils/include/myprint.h"
#include "utils/include/mystr.h"

cache_stat_t *generate_mini_mrc(struct MINI_arguments *args) {
  int n_configs = args->n_cache_size * args->n_eviction_algo;

  sim_global_config_t global_cfg;
  memset(&global_cfg, 0, sizeof(global_cfg));
  global_cfg.queue_depth = 0;
  global_cfg.report_interval = (uint64_t)args->report_interval;

  sim_config_t *configs = my_malloc_n(sim_config_t, n_configs);
  memset(configs, 0, sizeof(sim_config_t) * (size_t)n_configs);

  for (int i = 0; i < n_configs; i++) {
    configs[i].warmup_sec = args->warmup_sec;
    configs[i].warmup_frac = 0.0;
    configs[i].cache_size = args->caches[i]->cache_size;
  }

  cache_stat_t *result = simulate_with_config_list(
      args->reader, &global_cfg, configs, args->caches, n_configs, NULL);

  my_free(sizeof(sim_config_t) * (size_t)n_configs, configs);
  return result;
}
