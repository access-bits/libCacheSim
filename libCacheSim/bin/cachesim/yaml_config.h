/* yaml_config.h — Parse a YAML configuration file into sim_global_config_t
 * and an array of sim_config_t.
 *
 * Usage:
 *   sim_global_config_t gcfg;
 *   sim_config_t *configs = NULL;
 *   int n_configs = 0;
 *   parse_yaml_config("config.yaml", &gcfg, &configs, &n_configs);
 *   // ... use gcfg and configs[] ...
 *   free(configs);
 */

#pragma once

#include "libCacheSim/sim_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a YAML configuration file.
 *
 * On success, *configs is heap-allocated (caller must free()) and *n_configs
 * is set to the number of entries parsed from the "configurations" sequence.
 * gcfg is filled from the "trace", "global", and "output" mappings.
 *
 * Aborts with an error message on any parse failure or missing required field.
 *
 * @param path      path to the YAML file
 * @param gcfg      output global config (caller-allocated)
 * @param configs   output pointer to heap-allocated sim_config_t array
 * @param n_configs output number of configs parsed
 */
void parse_yaml_config(const char *path, sim_global_config_t *gcfg,
                       sim_config_t **configs, int *n_configs);

/**
 * @brief Convert a size string (e.g. "1GiB", "512MiB", "1048576") to bytes.
 * Recognised suffixes (case-insensitive): B, KiB/KB/K, MiB/MB/M, GiB/GB/G,
 * TiB/TB/T.  No suffix → plain byte count.
 * Returns 0 on parse error.
 */
uint64_t parse_size_str(const char *s);

#ifdef __cplusplus
}
#endif
