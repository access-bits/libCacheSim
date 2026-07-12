/* sim_config.h — YAML-driven simulator configuration structs
 *
 * sim_global_config_t  — trace-level / global settings
 * sim_config_t         — per-simulation (policy, size, warmup, etc.)
 *
 * These are plain data structs; no init functions live here.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum string length for names, paths, and param strings */
#define SIM_STR_MAX  256
#define SIM_PATH_MAX 512

/* -----------------------------------------------------------------------
 * Per-simulation configuration
 * ----------------------------------------------------------------------- */
typedef struct {
  char     policy[SIM_STR_MAX];           /* eviction policy name, e.g. "lru" */
  uint64_t cache_size;                    /* cache size in bytes */
  char     cache_size_str[SIM_STR_MAX];   /* original size string from YAML, e.g. "100M" */

  /* Warmup: warmup_sec and warmup_frac are OR-ed (first condition satisfied wins).
   * Set both to 0/0.0 to skip warmup. */
  int      warmup_sec;                    /* seconds of trace time used as warmup */
  double   warmup_frac;                   /* fraction [0,1) of trace used as warmup */

  char     eviction_params[SIM_STR_MAX];  /* optional eviction params, or "" */
  char     admission[SIM_STR_MAX];        /* admission algo name, or "" */
  char     admission_params[SIM_STR_MAX]; /* optional admission params, or "" */
  char     prefetch[SIM_STR_MAX];         /* prefetch algo name, or "" */
  char     prefetch_params[SIM_STR_MAX];  /* optional prefetch params, or "" */
} sim_config_t;

/* -----------------------------------------------------------------------
 * Global / trace-level configuration shared across all simulations
 * ----------------------------------------------------------------------- */
typedef struct {
  char    trace_path[SIM_PATH_MAX];   /* path to trace file */
  char    trace_type[SIM_STR_MAX];    /* trace type string, e.g. "binary", "csv" */
  char    trace_params[SIM_STR_MAX];  /* optional trace reader params string */
  int64_t num_req;                    /* max requests to process, -1 = all */
  double  sample_ratio;               /* object sampling ratio; 1.0 = no sampling */

  /* Queue depth for the batch-and-barrier bounded queues.
   * 0 or negative → use default (1024). */
  int     queue_depth;

  bool    ignore_obj_size;
  bool    consider_obj_metadata;
  bool    use_ttl;
  bool    verbose;
  bool    print_head_req;

  uint64_t report_interval;          /* print per-worker stats every N requests (0 = disabled) */

  char    output_path[SIM_PATH_MAX];  /* output file path */

  /* Logging — all fields are optional; empty log_dir falls back to stderr */
  char    log_dir[SIM_PATH_MAX];      /* directory for log files */
  char    log_level[SIM_STR_MAX];     /* min level: DEBUG/INFO/WARN/ERROR */
  char    log_streams[SIM_STR_MAX];   /* "ALL" or comma-separated stream names */
} sim_global_config_t;

#ifdef __cplusplus
}
#endif
