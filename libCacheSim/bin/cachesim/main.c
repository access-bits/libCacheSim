

#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <assert.h>
#include <errno.h>
#include <libgen.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_init.h"
#include "libCacheSim/log.h"
#include "libCacheSim/admissionAlgo.h"
#include "libCacheSim/cache.h"
#include "libCacheSim/prefetchAlgo.h"
#include "libCacheSim/reader.h"
#include "libCacheSim/sim_config.h"
#include "libCacheSim/simulator.h"
#include "libCacheSim/eviction_analyzer_factory.h"
#include "utils/include/mystr.h"
#include "utils/include/mysys.h"
#include "yaml_config.h"
#include "../cli_reader_utils.h"

/* Build a filesystem-safe stream name for a per-config log stream.
 * Format: config_N_policy_Xunit (e.g. config_0_lru_100M) */
static void make_config_stream_name(int idx, const sim_config_t *cfg,
                                    char *out, size_t sz) {
  /* Use the original cache size string from YAML (e.g. "100M", "1G") */
  snprintf(out, sz, "config_%d_%s_%s", idx, cfg->policy, cfg->cache_size_str);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s config.yaml\n", argv[0]);
    return 1;
  }

  /* ---- Parse YAML ---- */
  sim_global_config_t gcfg;
  sim_config_t       *configs   = NULL;
  int                 n_configs  = 0;
  parse_yaml_config(argv[1], &gcfg, &configs, &n_configs);

  /* ---- Initialise structured logger ---- */
  log_config_t log_cfg = {
      .dir     = gcfg.log_dir,
      .level   = gcfg.log_level,
      .streams = gcfg.log_streams,
  };
  log_init(&log_cfg);

  LOG(INFO, STREAM_Main, "Loaded config: %d configuration(s), trace=%s",
      n_configs, gcfg.trace_path);

  /* ---- Set up trace reader ---- */
  reader_init_param_t rp;
  memset(&rp, 0, sizeof(rp));
  rp.ignore_obj_size       = gcfg.ignore_obj_size;
  rp.ignore_size_zero_req  = true;
  rp.obj_id_is_num         = true;
  rp.cap_at_n_req          = gcfg.num_req;

  parse_reader_params(gcfg.trace_params[0] ? gcfg.trace_params : NULL, &rp);

  if (gcfg.sample_ratio > 0.0 && gcfg.sample_ratio < 1.0 - 1e-6) {
    rp.sampler = create_spatial_sampler(gcfg.sample_ratio);
  }

  /* For CSV/text traces without an explicit object-size field, disable size */
  trace_type_e trace_type =
      trace_type_str_to_enum(gcfg.trace_type, gcfg.trace_path);
  if ((trace_type == CSV_TRACE || trace_type == PLAIN_TXT_TRACE) &&
      rp.obj_size_field == -1) {
    gcfg.ignore_obj_size = true;
    gcfg.consider_obj_metadata = false;
    rp.ignore_obj_size = true;
  }

  reader_t *reader = setup_reader(gcfg.trace_path, trace_type, &rp);

  if (gcfg.consider_obj_metadata && should_disable_obj_metadata(reader)) {
    gcfg.consider_obj_metadata = false;
  }

  /* ---- Create one cache per configuration ---- */
  cache_t **caches = malloc(sizeof(cache_t *) * (size_t)n_configs);
  if (!caches) {
    LOG(ERROR, STREAM_Main, "OOM allocating cache pointer array");
    log_shutdown();
    return 1;
  }

  for (int i = 0; i < n_configs; i++) {
    caches[i] = create_cache(
        gcfg.trace_path,
        gcfg.trace_type,
        configs[i].policy,
        configs[i].cache_size,
        configs[i].eviction_params[0] ? configs[i].eviction_params : NULL,
        gcfg.consider_obj_metadata);

    if (configs[i].admission[0]) {
      caches[i]->admissioner = create_admissioner(
          configs[i].admission,
          configs[i].admission_params[0] ? configs[i].admission_params : NULL);
    }

    if (configs[i].prefetch[0]) {
      caches[i]->prefetcher = create_prefetcher(
          configs[i].prefetch,
          configs[i].prefetch_params[0] ? configs[i].prefetch_params : NULL,
          configs[i].cache_size);
    }

    /* Set up eviction analyzers for this cache */
    if (!setup_eviction_analyzers(caches[i], configs[i].eviction_analyzers,
                                  configs[i].n_eviction_analyzers)) {
      LOG(ERROR, STREAM_Main, "Failed to set up eviction analyzers for cache %d",
          i);
      log_shutdown();
      return 1;
    }
  }

  /* ---- Run simulation ---- */
  /* Register per-config log streams for per-worker fan-out routing */
  stream_id_t *config_streams = malloc(sizeof(stream_id_t) * (size_t)n_configs);
  for (int i = 0; i < n_configs; i++) {
    char name[SIM_STR_MAX * 2 + 32];
    make_config_stream_name(i, &configs[i], name, sizeof(name));
    config_streams[i] = log_register_stream(name);
  }

  /* Worker-init callback: routes each worker thread to its config log stream */

  LOG(INFO, STREAM_Reader, "Starting simulation with %d config(s)", n_configs);
  cache_stat_t *result =
      simulate_with_config_list(reader, &gcfg, configs, caches, n_configs,
                                config_streams);
  free(config_streams);

  /* ---- Output results ---- */
  /* Ensure output directory exists */
  char *output_dir = rindex(gcfg.output_path, '/');
  if (output_dir != NULL) {
    size_t dir_len = (size_t)(output_dir - gcfg.output_path);
    char dir_path[SIM_PATH_MAX];
    snprintf(dir_path, dir_len + 1, "%s", gcfg.output_path);
    create_dir(dir_path);
  }

  FILE *output_file = fopen(gcfg.output_path, "a");
  if (output_file == NULL) {
    LOG(ERROR, STREAM_Main, "cannot open output file '%s': %s", gcfg.output_path,
        strerror(errno));
    log_shutdown();
    return 1;
  }

  /* Determine a common size unit for display */
  uint64_t size_unit = 1;
  const char *size_unit_str = "B";
  if (!gcfg.ignore_obj_size && n_configs > 0) {
    uint64_t s = result[0].cache_size;
    if (s > (uint64_t)1024 * 1024 * 1024) {
      size_unit     = (uint64_t)1024 * 1024 * 1024;
      size_unit_str = "GiB";
    } else if (s > (uint64_t)1024 * 1024) {
      size_unit     = (uint64_t)1024 * 1024;
      size_unit_str = "MiB";
    } else if (s > (uint64_t)1024) {
      size_unit     = 1024;
      size_unit_str = "KiB";
    }
  }

  printf("\n");
  for (int i = 0; i < n_configs; i++) {
    double miss_ratio =
        result[i].n_req > 0
            ? (double)result[i].n_miss / (double)result[i].n_req
            : 0.0;

    double byte_miss_ratio =
        result[i].n_req_byte > 0
            ? (double)result[i].n_miss_byte / (double)result[i].n_req_byte
            : 0.0;

    double cost_saving_ratio =
        result[i].n_req_cost > 0
            ? 1.0 - result[i].n_miss_cost / result[i].n_req_cost
            : 0.0;

    bool show_cost = fabs(1.0 - cost_saving_ratio - miss_ratio) > 1e-9;

    char line[1024];
    int  n = snprintf(line, sizeof(line),
                      "%s %s cache size %8ld%s, %lld req, miss ratio %.4lf",
                      gcfg.trace_path, result[i].cache_name,
                      (long)(result[i].cache_size / size_unit), size_unit_str,
                      (long long)result[i].n_req, miss_ratio);

    if (!gcfg.ignore_obj_size)
      n += snprintf(line + n, sizeof(line) - (size_t)n,
                    ", byte miss ratio %.4lf", byte_miss_ratio);

    if (show_cost)
      n += snprintf(line + n, sizeof(line) - (size_t)n,
                    ", cost saving ratio %.4lf", cost_saving_ratio);

    snprintf(line + n, sizeof(line) - (size_t)n, "\n");

    printf("%s", line);
    fprintf(output_file, "%s", line);
  }

  fclose(output_file);

  /* ---- Clean up ---- */
  close_reader(reader);
  /* caches are freed by simulate_with_config_list workers */
  free(caches);
  free(configs);
  if (n_configs > 0)
    my_free(sizeof(cache_stat_t) * (size_t)n_configs, result);

  log_shutdown();
  return 0;
}

