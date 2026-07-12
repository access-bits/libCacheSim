#include "libCacheSim/cache.h"
#include "libCacheSim/reader.h"
#include "utils/include/mymath.h"
#include "utils/include/mystr.h"
#include "utils/include/mysys.h"
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Heatmap data structure to track per-object statistics */
typedef struct {
  uint64_t access_count;
  uint64_t miss_count;
} heatmap_obj_stat_t;

/**
 * @brief Flush one heatmap interval to the output file
 * @param f Output file handle
 * @param ht GHashTable mapping obj_id to heatmap_obj_stat_t*
 * @param access_start First request count of this interval
 * @param access_end Last request count of this interval
 * @param first Pointer to bool tracking if this is the first interval; will be set to false
 */
static void flush_heatmap_interval(FILE *f, GHashTable *ht, uint64_t access_start,
                                    uint64_t access_end, bool *first) {
  if (!*first) {
    fprintf(f, ",\n");
  }
  *first = false;

  fprintf(f, "  {\n");
  fprintf(f, "    \"access_start\": %lu,\n", access_start);
  fprintf(f, "    \"access_end\": %lu,\n", access_end);
  fprintf(f, "    \"objects\": {\n");

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, ht);
  bool first_obj = true;

  while (g_hash_table_iter_next(&iter, &key, &value)) {
    uint64_t obj_id = (uint64_t)(uintptr_t)key;
    heatmap_obj_stat_t *stat = (heatmap_obj_stat_t *)value;

    if (!first_obj) {
      fprintf(f, ",\n");
    }
    first_obj = false;

    fprintf(f, "      \"%lu\": {\"access_count\": %lu, \"miss_count\": %lu}",
            obj_id, stat->access_count, stat->miss_count);
  }

  fprintf(f, "\n    }\n");
  fprintf(f, "  }");

  g_hash_table_remove_all(ht);
}

void print_head_requests(request_t *req, uint64_t req_cnt) {
  if (req_cnt < 2) {
    print_request(req);
  }
}

void simulate(reader_t *reader, cache_t *cache, int report_interval,
              int warmup_sec, char *ofilepath, bool ignore_obj_size,
              bool print_head_req, bool enable_heatmap, uint64_t heatmap_interval,
              char *heatmap_ofilepath) {
  /* random seed */
  srand(time(NULL));
  set_rand_seed(rand());

  request_t *req = new_request();
  uint64_t req_cnt = 0, miss_cnt = 0;
  uint64_t last_req_cnt = 0, last_miss_cnt = 0;
  uint64_t req_byte = 0, miss_byte = 0;
  double req_cost = 0, miss_cost = 0;

  read_one_req(reader, req);
  uint64_t start_ts = (uint64_t)req->clock_time;
  uint64_t last_report_ts = warmup_sec;

  char detailed_cache_name[256];
  generate_cache_name(cache, detailed_cache_name, 256);

  double start_time = -1;

  /* Heatmap variables */
  GHashTable *heatmap_ht = NULL;
  FILE *heatmap_file = NULL;
  bool heatmap_first = true;
  uint64_t heatmap_next_flush = heatmap_interval;
  uint64_t heatmap_access_start = 0;

  if (enable_heatmap) {
    /* Create directory if it doesn't exist */
    char *heatmap_dir = rindex(heatmap_ofilepath, '/');
    if (heatmap_dir != NULL) {
      size_t dir_length = heatmap_dir - heatmap_ofilepath;
      char dir_path[1024];
      snprintf(dir_path, dir_length + 1, "%s", heatmap_ofilepath);
      create_dir(dir_path);
    }

    heatmap_file = fopen(heatmap_ofilepath, "w");
    if (heatmap_file == NULL) {
      ERROR("cannot open heatmap file %s %s\n", heatmap_ofilepath, strerror(errno));
      exit(1);
    }
    fprintf(heatmap_file, "[\n");

    heatmap_ht = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  }

  while (req->valid) {
    if (print_head_req) {
      print_head_requests(req, req_cnt);
    }

    req->clock_time -= start_ts;
    if (req->clock_time <= warmup_sec) {
      cache->get(cache, req);
      read_one_req(reader, req);
      continue;
    } else {
      if (start_time < 0) {
        start_time = gettime();
      }
    }

    req_cnt++;
    req_byte += req->obj_size;
    req_cost += req->obj_cost;
    bool is_miss = cache->get(cache, req) == false;
    if (is_miss) {
      miss_cnt++;
      miss_byte += req->obj_size;
      miss_cost += req->obj_cost;
    }

    /* Track heatmap statistics */
    if (enable_heatmap) {
      gpointer obj_key = (gpointer)(uintptr_t)req->obj_id;
      heatmap_obj_stat_t *obj_stat = g_hash_table_lookup(heatmap_ht, obj_key);
      if (obj_stat == NULL) {
        obj_stat = g_malloc(sizeof(heatmap_obj_stat_t));
        obj_stat->access_count = 0;
        obj_stat->miss_count = 0;
        g_hash_table_insert(heatmap_ht, obj_key, obj_stat);
      }
      obj_stat->access_count++;
      if (is_miss) {
        obj_stat->miss_count++;
      }

      /* Check if we need to flush this interval */
      if (req_cnt >= heatmap_next_flush) {
        flush_heatmap_interval(heatmap_file, heatmap_ht, heatmap_access_start,
                               req_cnt, &heatmap_first);
        heatmap_access_start = req_cnt;
        heatmap_next_flush = req_cnt + heatmap_interval;
      }
    }

    if (req->clock_time - last_report_ts >= (uint64_t)report_interval &&
        req->clock_time != 0) {
      INFO(
          "%s %s %.2lf hour: %lu requests, %lu misses (%.4lf), interval: %lu "
          "requests, %lu misses (%.4lf)\n",
          mybasename(reader->trace_path), detailed_cache_name,
          (double)req->clock_time / 3600, (unsigned long)req_cnt,
          (unsigned long)miss_cnt, (double)miss_cnt / req_cnt,
          (unsigned long)(req_cnt - last_req_cnt),
          (unsigned long)(miss_cnt - last_miss_cnt),
          (double)(miss_cnt - last_miss_cnt) / (req_cnt - last_req_cnt));
      last_miss_cnt = miss_cnt;
      last_req_cnt = req_cnt;
      last_report_ts = (int64_t)req->clock_time;
    }

    read_one_req(reader, req);
  }

  /* Flush last partial heatmap interval if there are any objects */
  if (enable_heatmap && g_hash_table_size(heatmap_ht) > 0) {
    flush_heatmap_interval(heatmap_file, heatmap_ht, heatmap_access_start, req_cnt,
                           &heatmap_first);
  }

  /* Close and cleanup heatmap resources */
  if (enable_heatmap) {
    fprintf(heatmap_file, "\n]\n");
    fclose(heatmap_file);
    g_hash_table_destroy(heatmap_ht);
  }

  double runtime = gettime() - start_time;

  char output_str[1024];
  char size_str[64];
  double miss_ratio = req_cnt > 0 ? (double)miss_cnt / (double)req_cnt : 0.0;
  double byte_miss_ratio =
      req_byte > 0 ? (double)miss_byte / (double)req_byte : 0.0;
  double cost_saving_ratio = 1.0 - (req_cost > 0 ? miss_cost / req_cost : 0.0);

  if (!ignore_obj_size)
    convert_size_to_str(cache->cache_size, size_str, 64);
  else
    snprintf(size_str, sizeof(size_str), "%lld", (long long)cache->cache_size);

  bool show_cost = fabs(1 - cost_saving_ratio - miss_ratio) > 1e-9;

  int n = snprintf(output_str, sizeof(output_str),
                   "%s %s cache size %8s, %16lu req, miss ratio %.4lf",
                   reader->trace_path, detailed_cache_name, size_str,
                   (unsigned long)req_cnt, miss_ratio);
  if (!ignore_obj_size)
    n += snprintf(output_str + n, sizeof(output_str) - n,
                  ", byte miss ratio %.4lf", byte_miss_ratio);
  if (show_cost)
    n += snprintf(output_str + n, sizeof(output_str) - n,
                  ", cost saving ratio %.4lf", cost_saving_ratio);
  snprintf(output_str + n, sizeof(output_str) - n, ", throughput %.2lf MQPS\n",
           (double)req_cnt / 1000000.0 / runtime);
  printf("%s", output_str);
  char *output_dir = rindex(ofilepath, '/');
  if (output_dir != NULL) {
    size_t dir_length = output_dir - ofilepath;
    char dir_path[1024];
    snprintf(dir_path, dir_length + 1, "%s", ofilepath);
    create_dir(dir_path);
  }
  FILE *output_file = fopen(ofilepath, "a");
  if (output_file == NULL) {
    ERROR("cannot open file %s %s\n", ofilepath, strerror(errno));
    exit(1);
  }
  fprintf(output_file, "%s", output_str);
  fclose(output_file);

#if defined(TRACK_EVICTION_V_AGE)
  while (cache->get_occupied_byte(cache) > 0) {
    cache->evict(cache, req);
  }

#endif
  free_request(req);
  cache->cache_free(cache);
}

#ifdef __cplusplus
}
#endif
