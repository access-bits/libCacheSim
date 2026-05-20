# AGENTS.md — libCacheSim Architecture Reference

> **Read this first before modifying the codebase.**
> **Reflect any structural changes in this file before merging.**

---

## Overview

libCacheSim simulates cache eviction algorithms using a pluggable `cache_t` abstraction supporting multiple policies, optional admission filters, and optional prefetchers. **Focus: eviction subsystem.**

**Core tree:**
```
libCacheSim/
├── include/libCacheSim/     # Public API
├── cache/                   # Cache logic + eviction/admission/prefetch implementations
│   ├── cache.c              # Base: lifecycle, get/find/insert/evict primitives
│   ├── eviction/            # One .c per policy
│   ├── admission/           # Filters (bloom, size-based, etc.)
│   └── prefetch/            # Prefetchers
├── dataStructure/           # Hashtable, pqueue, splay, bloom
├── profiler/                # MRC profilers; simulator.c contains multi-simulator logic
├── traceReader/             # Trace parsers
└── bin/                     # Binaries (cachesim)
```

---

## Key Abstractions

### 1. `cache_t` — The Cache Object

**File:** `libCacheSim/include/libCacheSim/cache.h`

Manual vtable: every eviction algorithm fills in function pointers (`get`, `find`, `insert`, `evict`, etc.) in its `_init`.

**Core fields:**
- `hashtable` — O(1) object lookup (ALL algorithms use this)
- `eviction_params` — opaque per-algorithm state
- `q_head`, `q_tail` — doubly-linked list for single-queue policies
- `n_req`, `n_obj`, `occupied_byte`, `cache_size` — counters (mutate only via `cache_insert_base` / `cache_remove_obj_base`)
- `admissioner`, `prefetcher` — optional composable components

### 2. `cache_obj_t` — Per-Object Metadata

**File:** `libCacheSim/include/libCacheSim/cacheObj.h`

One per cached object. Core: `obj_id`, `obj_size`, `next_access_vtime` (oracle), `freq`, queue links. All algorithm metadata shares a `union` — adding a new algorithm requires adding a struct to the union. The size of `cache_obj_t` is the size of the largest union member.

### 3. `request_t` — A Single Access Event

**File:** `libCacheSim/include/libCacheSim/request.h`

Key fields: `obj_id`, `obj_size`, `clock_time`, `next_access_vtime` (oracle), `ttl`, `op`.

### 4. `admissioner_t` — Admission Filter (optional)

**File:** `libCacheSim/include/libCacheSim/admissionAlgo.h`

Vtable-pattern filter attached to `cache_t`. If non-NULL, called before insertion. Available: bloom-filter, probabilistic, size-based, AdaptSize.

---

## The Base Layer (cache.c)

**File:** `libCacheSim/cache/cache.c`

Shared primitives that every eviction algorithm must call. They own all shared state transitions.

| Function | Purpose |
|---|---|
| `cache_struct_init` | Allocate, zero-init `cache_t`, create hashtable, set defaults |
| `cache_struct_free` | Free hashtable, admissioner, prefetcher, `cache_t` |
| `cache_get_base` | Canonical request loop (lookup, evict-to-size, insert, prefetch hook) |
| `cache_find_base` | Hashtable lookup; TTL expiry; freq++, vtime update on hit |
| `cache_insert_base` | `hashtable_insert` + `occupied_byte++`, `n_obj++` |
| `cache_evict_base` | Prefetcher hook, then call `cache_remove_obj_base` |
| `cache_remove_obj_base` | `occupied_byte--`, `n_obj--` |
| `cache_can_insert_default` | Call `admissioner->admit` if present |

**Request flow:** `get()` → lookup (hit?) → on miss: admission gate + evict-to-size + insert + prefetcher.

---

## Per-Algorithm Layer (eviction/)

**Directory:** `libCacheSim/cache/eviction/`

Each `.c` file implements one eviction policy.

### Required Pattern

1. **Init:** Allocate cache, fill vtable (`get`, `find`, `insert`, `evict`), allocate `eviction_params`
2. **Evict:** Select victim from policy-specific structure, call `cache_evict_base()` to remove
3. **Free:** Cleanup params, call `cache_struct_free()`

### Key files

| File | Policy |
|---|---|
| `LRU.c` | LRU — simplest reference; single queue |
| `FIFO.c` | FIFO — good template |
| `S3FIFO.c` | Multi-queue with ghosts |
| `Sieve.c` | Lazy FIFO variant |
| `Clock.c` | Clock approximation |
| `ARC.c` | Adaptive Replacement |
| `fifo/` | FIFO variants |
| `GLCache/` | Learning-based with segments |

---

## Multi-Simulator Architecture (simulator.c)

**File:** `libCacheSim/profiler/simulator.c`

### Traditional: `simulate_with_multi_caches`

One thread per cache; each clones reader and reads entire trace. For N configs: N trace reads, N file descriptors.

### New: `simulate_with_single_reader`

Single reader reads trace once. Requests fanned into N bounded queues. Worker threads pop from own queue in parallel. **One trace read, true parallelism.**

### Bounded Queue: `bounded_queue_t`

Ring buffer with semaphore-based synchronization. Reader accumulates requests locally, waits for all workers to drain, bulk-pushes batch.

```c
typedef struct {
  request_t *buf;        /* ring buffer */
  int        capacity;
  int        head, tail, count;
  GMutex     mutex;
  sem_t      empty_sem;  /* worker posts when queue drains */
  sem_t      ready_sem;  /* reader posts when batch ready */
  bool       closed;
} bounded_queue_t;
```

**Key functions:**
- `bqueue_create(capacity)` — init (empty_sem starts at 1)
- `bqueue_push_batch(q, batch, size)` — bulk memcpy, signal worker
- `bqueue_process_batch(q, ...)` — hold lock, process all requests directly from buffer, post empty
- `bqueue_close(q)` — signal EOF

### Batch-and-Barrier Pattern

**Reader:** Fill local batch buffer → wait for all queues empty (barrier) → push batch to all queues → repeat.

**Worker:** Wait for batch ready → acquire lock once, process all requests directly from queue buffer (no copy) → release lock → repeat.

**Key:** No lock contention because reader is blocked at barrier when worker holds lock. No local buffer allocation in worker.

### Worker Function: `_simulate_from_queue`

**Three-phase warmup:** 
1. `warmup_reader` requests (raw timestamps, always warmup)
2. Count/time-based warmup from main reader (normalized timestamps)
3. Measured simulation (statistics recorded)

Loop: wait for batch ready → call `bqueue_process_batch()` → check EOF.

### Synchronization Model

**Pattern:** Barrier → push batch (post ready) → workers process (post empty).

**Lock overhead:** 2N per batch (N reader + N worker); amortized to 2N / queue_depth per request.

**Memory:** O(N × queue_depth × sizeof(request_t)) ≈ (N+1) × 100 KB. No per-worker buffers.

**Tradeoffs:** Minimal contention, simple code. Slowest worker paces system (lock-step). No pipeline overlap.

### Public APIs

#### `simulate_with_single_reader`

Read trace once, fan to N workers processing different caches in parallel.

**Key params:** `queue_depth` (≤0 → 1024), `warmup_reader`, `warmup_frac` / `warmup_sec`.

#### `simulate_at_multi_sizes_single_reader`

MRC sweep with single reader. Same as `simulate_at_multi_sizes` but reads trace only once.

---

## Data Structures (dataStructure/)

| File | Used by |
|---|---|
| `hashtable/chainedHashTableV2.h` | All (default) |
| `pqueue.c/h` | Cost/size-aware policies |
| `splay.c/h` | Frequency-based policies |
| `bloom.c/h` | Bloom-filter admission |

---

## Compile-Time Configuration (include/config.h)

| Macro | Default | Effect |
|---|---|---|
| `HASHTABLE_TYPE` | `CHAINED_HASHTABLEV2` | Hashtable impl |
| `HASH_TYPE` | `XXHASH3` | Hash function |
| `HASH_POWER_DEFAULT` | `23` | Initial hashtable size (2^23) |
| `SUPPORT_TTL` | off | Per-object TTL expiry |
| `TRACK_EVICTION_V_AGE` | off | Eviction age histogram |

---

## Rules for Modifying the Architecture

1. **Adding a new eviction algorithm:**
   - Add `.c` under `cache/eviction/`
   - Add metadata struct to `union` in `cacheObj.h`
   - Declare `FOO_init` in `include/libCacheSim/evictionAlgo.h`
   - Register in simulator dispatch table
   - **Update this file**

2. **Changing `cache_t` fields:**
   - Zero-init in `cache_struct_init`
   - Update `clone_cache` if needed
   - **Update this file**

3. **Changing `cache_obj_t` fields:**
   - Struct is `__attribute__((packed))` — alignment aware
   - Union member ≤ largest existing → free. Otherwise justify cost
   - **Update this file**

4. **Changing `cache_get_base` / `cache_evict_base` / `cache_insert_base`:**
   - Called by every algorithm — affects all policies
   - **Update this file**

5. **Modifying multi-simulator architecture (simulator.c):**
   - `bounded_queue_t` changes affect memory/latency
   - Worker loop changes must preserve thread-safety
   - **Update this file**

---

## Quick Reference: What to Read

| Goal | Files |
|---|---|
| Understand full eviction flow | `cache.h`, `cache.c`, `LRU.c` |
| Add new eviction algorithm | `cache.h`, `cacheObj.h`, `FIFO.c` |
| Understand hashtable | `dataStructure/hashtable/` |
| Add admission filter | `admissionAlgo.h`, `cache/admission/` |
| Change compile-time knobs | `include/config.h.in`, `CMakeLists.txt` |
| Understand traces | `include/libCacheSim/reader.h`, `traceReader/` |
| Run single-cache MRC | `profiler/simulator.c:simulate_at_multi_sizes` |
| Run multi-cache (old) | `profiler/simulator.c:simulate_with_multi_caches` |
| Run multi-cache single-reader (new) | `profiler/simulator.c:simulate_with_single_reader` |
| Run MRC single-reader (new) | `profiler/simulator.c:simulate_at_multi_sizes_single_reader` |
| End-to-end simulator (cachesim binary) | `bin/`, `example/cacheSimulator/` |
