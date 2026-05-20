//
//  simulator.h
//
//  Created by Juncheng on 5/24/16.
//  Copyright © 2016 Juncheng. All rights reserved.
//

#ifndef simulator_h
#define simulator_h

#include "cache.h"
#include "log.h"
#include "reader.h"
#include "sim_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Simulate a list of configurations reading the trace exactly once.
 *
 * Thread pool size equals n_configs — every configuration runs concurrently.
 * Caches must be pre-created by the caller with admission/prefetch attached.
 * The function frees each cache after its worker finishes.
 *
 * @param reader         trace reader (consumed once on the calling thread)
 * @param global_cfg     global settings: queue_depth, etc.
 * @param configs        per-config settings: warmup_sec, warmup_frac, etc.
 * @param caches         pre-created cache_t* array of length n_configs
 * @param n_configs      number of configurations
 * @param config_streams per-worker log stream IDs (length n_configs); may be
 *                       NULL to skip per-worker log routing
 * @return heap-allocated cache_stat_t[n_configs]; caller must free
 */
cache_stat_t *simulate_with_config_list(
    reader_t *reader, sim_global_config_t *global_cfg,
    sim_config_t *configs, cache_t **caches, int n_configs,
    const stream_id_t *config_streams);

#ifdef __cplusplus
}
#endif

#endif /* simulator_h */
