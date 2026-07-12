#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT_DIR/build/bin/cachesim"

CONFIGS=(
  # "$ROOT_DIR/configs/memcached_accessbit.yaml"
  # "$ROOT_DIR/configs/memcached_pebs.yaml"
  # "$ROOT_DIR/configs/pagerank_accessbit.yaml"
  # "$ROOT_DIR/configs/pagerank_pebs.yaml"
  # "$ROOT_DIR/configs/tpch-sparksql_accessbit.yaml"
  # "$ROOT_DIR/configs/tpch-sparksql_pebs.yaml"
  # "$ROOT_DIR/configs/memcached-lru-practical.yaml"
  # "$ROOT_DIR/configs/pagerank-lru-practical.yaml"
  "$ROOT_DIR/configs/tpch-sparksql-lru-practical.yaml"
)

# echo "Ensuring cachesim is up to date..."
# cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build"
# cmake --build "$ROOT_DIR/build" -j

if [[ ! -x "$BIN" ]]; then
  echo "Error: cachesim binary not found or not executable after build: $BIN"
  exit 1
fi

for cfg in "${CONFIGS[@]}"; do
  if [[ ! -f "$cfg" ]]; then
    echo "Error: missing config file: $cfg"
    exit 1
  fi

  echo "============================================================"
  echo "Running config: $cfg"
  echo "Started at: $(date '+%Y-%m-%d %H:%M:%S')"
  "$BIN" "$cfg"
  echo "Completed: $cfg"
  echo "Finished at: $(date '+%Y-%m-%d %H:%M:%S')"
  echo "============================================================"
  echo
done

echo "All 6 config runs completed successfully."
