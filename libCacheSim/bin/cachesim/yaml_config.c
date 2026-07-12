#define _GNU_SOURCE
#include "yaml_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include "libCacheSim/log.h"
#include "libCacheSim/sim_config.h"

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static void die(const char *msg) {
  LOG(ERROR, STREAM_Main, "yaml_config error: %s", msg);
  exit(1);
}

static void die2(const char *ctx, const char *detail) {
  LOG(ERROR, STREAM_Main, "yaml_config error: %s: %s", ctx, detail);
  exit(1);
}

/* Parse boolean scalar: "true"/"yes"/"1" → true; anything else → false */
static bool parse_bool(const char *s) {
  return (strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0 ||
          strcmp(s, "1") == 0);
}

/* Convert size string to bytes.
 * Recognised suffixes (case-insensitive): B, KiB/KB/K, MiB/MB/M, GiB/GB/G,
 * TiB/TB/T.  No suffix → plain byte count. */
uint64_t parse_size_str(const char *s) {
  if (s == NULL || s[0] == '\0') return 0;

  char *end = NULL;
  double val = strtod(s, &end);
  if (end == s) return 0;  /* no numeric part */

  /* Skip optional whitespace */
  while (*end == ' ') end++;

  if (*end == '\0' || strcasecmp(end, "b") == 0) {
    return (uint64_t)val;
  } else if (strcasecmp(end, "kib") == 0 || strcasecmp(end, "kb") == 0 ||
             strcasecmp(end, "k") == 0) {
    return (uint64_t)(val * 1024);
  } else if (strcasecmp(end, "mib") == 0 || strcasecmp(end, "mb") == 0 ||
             strcasecmp(end, "m") == 0) {
    return (uint64_t)(val * 1024 * 1024);
  } else if (strcasecmp(end, "gib") == 0 || strcasecmp(end, "gb") == 0 ||
             strcasecmp(end, "g") == 0) {
    return (uint64_t)(val * 1024 * 1024 * 1024);
  } else if (strcasecmp(end, "tib") == 0 || strcasecmp(end, "tb") == 0 ||
             strcasecmp(end, "t") == 0) {
    return (uint64_t)(val * 1024ULL * 1024 * 1024 * 1024);
  }
  return 0;
}

/* Safe strncpy that always NUL-terminates */
static void safe_copy(char *dst, size_t dsz, const char *src) {
  strncpy(dst, src, dsz - 1);
  dst[dsz - 1] = '\0';
}

/* -----------------------------------------------------------------------
 * libyaml event helpers
 * ----------------------------------------------------------------------- */

/* Advance parser to the next event, aborting on error */
static void next_event(yaml_parser_t *parser, yaml_event_t *ev) {
  if (!yaml_parser_parse(parser, ev)) {
    die("yaml parse error");
  }
}

/* Expect a scalar and return its value (points into the event; caller must
 * copy before calling next_event again).  ev must already be initialised. */
static const char *expect_scalar(yaml_parser_t *parser, yaml_event_t *ev) {
  yaml_event_delete(ev);
  next_event(parser, ev);
  if (ev->type != YAML_SCALAR_EVENT) {
    die("expected scalar value");
  }
  return (const char *)ev->data.scalar.value;
}

/* -----------------------------------------------------------------------
 * Section parsers
 * ----------------------------------------------------------------------- */

/* Parse the "trace:" mapping */
static void parse_trace_section(yaml_parser_t *parser, yaml_event_t *ev,
                                 sim_global_config_t *g) {
  /* ev is currently the MAPPING_START after the "trace" key */
  while (true) {
    yaml_event_delete(ev);
    next_event(parser, ev);

    if (ev->type == YAML_MAPPING_END_EVENT) break;
    if (ev->type != YAML_SCALAR_EVENT)
      die("expected key scalar in trace section");

    char key[64];
    safe_copy(key, sizeof(key), (const char *)ev->data.scalar.value);

    const char *val = expect_scalar(parser, ev);

    if (strcmp(key, "path") == 0) {
      safe_copy(g->trace_path, sizeof(g->trace_path), val);
    } else if (strcmp(key, "type") == 0) {
      safe_copy(g->trace_type, sizeof(g->trace_type), val);
    } else if (strcmp(key, "params") == 0) {
      safe_copy(g->trace_params, sizeof(g->trace_params), val);
    } else if (strcmp(key, "num_req") == 0) {
      g->num_req = (int64_t)strtoll(val, NULL, 10);
    } else if (strcmp(key, "sample_ratio") == 0) {
      g->sample_ratio = strtod(val, NULL);
    }
    /* unknown keys silently ignored */
  }
}

/* Parse the "global:" mapping */
static void parse_global_section(yaml_parser_t *parser, yaml_event_t *ev,
                                  sim_global_config_t *g) {
  while (true) {
    yaml_event_delete(ev);
    next_event(parser, ev);

    if (ev->type == YAML_MAPPING_END_EVENT) break;
    if (ev->type != YAML_SCALAR_EVENT)
      die("expected key scalar in global section");

    char key[64];
    safe_copy(key, sizeof(key), (const char *)ev->data.scalar.value);

    const char *val = expect_scalar(parser, ev);

    if (strcmp(key, "queue_depth") == 0) {
      g->queue_depth = atoi(val);
    } else if (strcmp(key, "ignore_obj_size") == 0) {
      g->ignore_obj_size = parse_bool(val);
    } else if (strcmp(key, "consider_obj_metadata") == 0) {
      g->consider_obj_metadata = parse_bool(val);
    } else if (strcmp(key, "use_ttl") == 0) {
      g->use_ttl = parse_bool(val);
    } else if (strcmp(key, "verbose") == 0) {
      g->verbose = parse_bool(val);
    } else if (strcmp(key, "print_head_req") == 0) {
      g->print_head_req = parse_bool(val);
    } else if (strcmp(key, "report_interval") == 0) {
      g->report_interval = (uint64_t)strtoull(val, NULL, 10);
    }
  }
}

/* Parse the "logging:" mapping */
static void parse_logging_section(yaml_parser_t *parser, yaml_event_t *ev,
                                   sim_global_config_t *g) {
  while (true) {
    yaml_event_delete(ev);
    next_event(parser, ev);

    if (ev->type == YAML_MAPPING_END_EVENT) break;
    if (ev->type != YAML_SCALAR_EVENT)
      die("expected key scalar in logging section");

    char key[64];
    safe_copy(key, sizeof(key), (const char *)ev->data.scalar.value);

    const char *val = expect_scalar(parser, ev);

    if (strcmp(key, "dir") == 0) {
      safe_copy(g->log_dir, sizeof(g->log_dir), val);
    } else if (strcmp(key, "level") == 0) {
      safe_copy(g->log_level, sizeof(g->log_level), val);
    } else if (strcmp(key, "streams") == 0) {
      safe_copy(g->log_streams, sizeof(g->log_streams), val);
    }
  }
}

/* Parse the "output:" mapping */
static void parse_output_section(yaml_parser_t *parser, yaml_event_t *ev,
                                  sim_global_config_t *g) {
  while (true) {
    yaml_event_delete(ev);
    next_event(parser, ev);

    if (ev->type == YAML_MAPPING_END_EVENT) break;
    if (ev->type != YAML_SCALAR_EVENT)
      die("expected key scalar in output section");

    char key[64];
    safe_copy(key, sizeof(key), (const char *)ev->data.scalar.value);

    const char *val = expect_scalar(parser, ev);

    if (strcmp(key, "path") == 0) {
      safe_copy(g->output_path, sizeof(g->output_path), val);
    }
  }
}

/* Parse one configuration mapping (already past MAPPING_START) */
static void parse_one_config(yaml_parser_t *parser, yaml_event_t *ev,
                              sim_config_t *c) {
  /* Initialise with defaults */
  memset(c, 0, sizeof(*c));
  c->warmup_sec  = -1;    /* -1 means no default warmup from seconds */
  c->warmup_frac = 0.0;

  while (true) {
    yaml_event_delete(ev);
    next_event(parser, ev);

    if (ev->type == YAML_MAPPING_END_EVENT) break;
    if (ev->type != YAML_SCALAR_EVENT)
      die("expected key scalar in configuration entry");

    char key[64];
    safe_copy(key, sizeof(key), (const char *)ev->data.scalar.value);

    const char *val = expect_scalar(parser, ev);

    if (strcmp(key, "policy") == 0) {
      safe_copy(c->policy, sizeof(c->policy), val);
    } else if (strcmp(key, "cache_size") == 0) {
      c->cache_size = parse_size_str(val);
      if (c->cache_size == 0) {
        die2("cache_size", "cannot parse size string");
      }
      safe_copy(c->cache_size_str, sizeof(c->cache_size_str), val);
    } else if (strcmp(key, "warmup_sec") == 0) {
      c->warmup_sec = atoi(val);
    } else if (strcmp(key, "warmup_frac") == 0) {
      c->warmup_frac = strtod(val, NULL);
    } else if (strcmp(key, "eviction_params") == 0) {
      safe_copy(c->eviction_params, sizeof(c->eviction_params), val);
    } else if (strcmp(key, "admission") == 0) {
      safe_copy(c->admission, sizeof(c->admission), val);
    } else if (strcmp(key, "admission_params") == 0) {
      safe_copy(c->admission_params, sizeof(c->admission_params), val);
    } else if (strcmp(key, "prefetch") == 0) {
      safe_copy(c->prefetch, sizeof(c->prefetch), val);
    } else if (strcmp(key, "prefetch_params") == 0) {
      safe_copy(c->prefetch_params, sizeof(c->prefetch_params), val);
    }
  }

  if (c->policy[0] == '\0') die("configuration entry missing required field: policy");
  if (c->cache_size == 0)    die("configuration entry missing required field: cache_size");
}

/* Parse the "configurations:" sequence */
static void parse_configurations_section(yaml_parser_t *parser,
                                          yaml_event_t *ev,
                                          sim_config_t **configs_out,
                                          int *n_configs_out) {
  /* ev is currently the SEQUENCE_START */
  int capacity = 16;
  int n        = 0;
  sim_config_t *arr = malloc(sizeof(sim_config_t) * (size_t)capacity);
  if (!arr) die("OOM allocating config array");

  while (true) {
    yaml_event_delete(ev);
    next_event(parser, ev);

    if (ev->type == YAML_SEQUENCE_END_EVENT) break;
    if (ev->type != YAML_MAPPING_START_EVENT)
      die("expected mapping in configurations sequence");

    if (n == capacity) {
      capacity *= 2;
      sim_config_t *tmp = realloc(arr, sizeof(sim_config_t) * (size_t)capacity);
      if (!tmp) die("OOM growing config array");
      arr = tmp;
    }

    parse_one_config(parser, ev, &arr[n]);
    n++;
  }

  *configs_out  = arr;
  *n_configs_out = n;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void parse_yaml_config(const char *path, sim_global_config_t *gcfg,
                       sim_config_t **configs, int *n_configs) {
  /* Set defaults */
  memset(gcfg, 0, sizeof(*gcfg));
  gcfg->num_req       = -1;
  gcfg->sample_ratio  = 1.0;
  gcfg->queue_depth   = 0;   /* 0 → simulator uses default 1024 */
  gcfg->verbose        = false;
  gcfg->print_head_req = true;
  /* Logging defaults: empty log_dir → stderr fallback; INFO level; ALL streams */
  gcfg->log_dir[0]     = '\0';
  safe_copy(gcfg->log_level,   sizeof(gcfg->log_level),   "INFO");
  safe_copy(gcfg->log_streams, sizeof(gcfg->log_streams), "ALL");

  *configs  = NULL;
  *n_configs = 0;

  FILE *fp = fopen(path, "r");
  if (!fp) {
    LOG(ERROR, STREAM_Main, "yaml_config: cannot open '%s': %s", path,
        strerror(errno));
    exit(1);
  }

  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) die("yaml_parser_initialize failed");
  yaml_parser_set_input_file(&parser, fp);

  yaml_event_t ev;
  memset(&ev, 0, sizeof(ev));

  /* Consume STREAM_START and DOCUMENT_START */
  next_event(&parser, &ev);
  if (ev.type != YAML_STREAM_START_EVENT) die("expected STREAM_START");
  yaml_event_delete(&ev);

  next_event(&parser, &ev);
  if (ev.type != YAML_DOCUMENT_START_EVENT) die("expected DOCUMENT_START");
  yaml_event_delete(&ev);

  /* Root must be a mapping */
  next_event(&parser, &ev);
  if (ev.type != YAML_MAPPING_START_EVENT) die("expected root mapping");
  yaml_event_delete(&ev);

  while (true) {
    next_event(&parser, &ev);

    if (ev.type == YAML_MAPPING_END_EVENT) break;  /* end of root mapping */
    if (ev.type == YAML_DOCUMENT_END_EVENT) break;
    if (ev.type == YAML_STREAM_END_EVENT)   break;

    if (ev.type != YAML_SCALAR_EVENT)
      die("expected top-level key scalar");

    char section[64];
    safe_copy(section, sizeof(section), (const char *)ev.data.scalar.value);
    yaml_event_delete(&ev);

    /* Peek at the next event to determine value type */
    next_event(&parser, &ev);

    if (strcmp(section, "trace") == 0) {
      if (ev.type != YAML_MAPPING_START_EVENT) die("trace: expected mapping");
      parse_trace_section(&parser, &ev, gcfg);
    } else if (strcmp(section, "global") == 0) {
      if (ev.type != YAML_MAPPING_START_EVENT) die("global: expected mapping");
      parse_global_section(&parser, &ev, gcfg);
    } else if (strcmp(section, "logging") == 0) {
      if (ev.type != YAML_MAPPING_START_EVENT) die("logging: expected mapping");
      parse_logging_section(&parser, &ev, gcfg);
    } else if (strcmp(section, "output") == 0) {
      if (ev.type != YAML_MAPPING_START_EVENT) die("output: expected mapping");
      parse_output_section(&parser, &ev, gcfg);
    } else if (strcmp(section, "configurations") == 0) {
      if (ev.type != YAML_SEQUENCE_START_EVENT)
        die("configurations: expected sequence");
      parse_configurations_section(&parser, &ev, configs, n_configs);
    } else {
      die2("unknown top-level key in YAML config", section);
    }
    yaml_event_delete(&ev);
  }

  yaml_event_delete(&ev);
  yaml_parser_delete(&parser);
  fclose(fp);

  /* Validate required fields */
  if (gcfg->trace_path[0] == '\0') die("required field missing: trace.path");
  if (gcfg->trace_type[0] == '\0') die("required field missing: trace.type");
  if (*n_configs == 0)             die("configurations sequence is empty");

  /* Generate default output path if not specified */
  if (gcfg->output_path[0] == '\0') {
    const char *base = strrchr(gcfg->trace_path, '/');
    base = (base != NULL) ? base + 1 : gcfg->trace_path;
    /* Limit base name to ensure output fits: 512 - "result/" (7) - ".cachesim" (9) - NUL (1) = 495 */
    snprintf(gcfg->output_path, sizeof(gcfg->output_path), "result/%.495s.cachesim",
             base);
  }
}
