# TreePass

[![DOI](https://zenodo.org/badge/1272154254.svg)](https://doi.org/10.5281/zenodo.20731284)

## Layout

```
.
├── splinterdb/           TreePass-extended SplinterDB fork
├── bench/                YCSB-style bench (random_mixed_bench)
├── examples/             Smoke-load generator
├── run_smoke.py          Build + run YCSB-A,C × unif,zipf,prefix
└── README.md
```

### TreePass changes inside `splinterdb/`

- `src/treepass.{c,h}` — TreePass cache backend, derived from upstream
  `src/clockcache.{c,h}` with TreePass-specific extensions.
- `src/btree.c`, `src/btree_private.h`, `src/trunk.c`, `src/core.c`,
  `src/routing_filter.c`, `src/splinterdb.c`, `src/cache.h`,
  `include/splinterdb/splinterdb.h` — other upstream SplinterDB files
  modified to embed Tree Counters in pivot entries and route them
  through eviction.
- All other files are stock SplinterDB.

# Prerequisites

## Software packages

```
- Ubuntu 22.04, kernel 5.15+, glibc 2.30+
- gcc / g++ via build-essential
- cmake 3.17+
- python3 (>= 3.7)
- libsnappy-dev, libaio-dev, libxxhash-dev, libgflags-dev, libtbb-dev
```

Ubuntu 18.04 / glibc 2.27 is **not** supported (missing `gettid()` libc wrapper);
the default Python 3.6 there also rejects the `from __future__ import
annotations` directive in `run_smoke.py`.

Install:

```bash
sudo apt-get install -y \
    build-essential cmake git python3 \
    libsnappy-dev libaio-dev libxxhash-dev libgflags-dev libtbb-dev
```

## Hardware

The paper measurements are taken on a 64-core, 256 GB-RAM machine
with NVMe SSDs. The smoke is sized to run comfortably on much smaller
boxes; the defaults `run_smoke.py` uses are:

```
--threads        4    bench worker threads
--cache-mb       64   query-phase block cache size
--load-cache-mb  512  one-shot load-phase block cache size
--phase-ops      8M   timed ops per phase
--db-path  /mnt/nvme  directory that holds the SplinterDB data file
--build-jobs   nproc  parallel make for the TreePass + bench build
```

At those defaults the smoke needs roughly:

- **CPU**: at least 4 cores (lower with `--threads`)
- **RAM**: ~2 GB free (load cache + query cache + DB working set + headroom)
- **Disk**: ~500 MB at `--db-path`; NVMe recommended for paper-like
  timing, any local disk still runs the smoke

Override `--db-path /some/writable/dir` if the default mount is
absent or not user-writable on your machine. Every knob above is a
flag on `run_smoke.py` — see `python3 run_smoke.py --help`.

# Quick Start

## Clone

```bash
git clone https://github.com/sheepjin11/treepass-artifact.git
cd treepass-artifact
```

## How to run the smoke benchmark

`run_smoke.py` builds TreePass + the bench, generates a synthetic 1 M
key load file, loads the dataset once, then runs YCSB-A and YCSB-C
across the `unif`, `zipf`, and `prefix` query distributions (6 timed
configs + load). It does **not** need root; only `apt-get install`
above does.

Logs land under `result/smoke_<timestamp>/`, one per config.

Usage:

```
python3 run_smoke.py \
    [--threads N] [--cache-mb N] [--load-cache-mb N] \
    [--phase-ops N] [--db-path PATH] [--build-jobs N]
```

Defaults: 4 threads, 64 MB query cache, 512 MB load cache, 8 M
ops/phase, `/mnt/nvme` db path, all-core build.

Default run:

```bash
python3 run_smoke.py
```

End-to-end runtime including the build: about **7 minutes** on a
32-core machine with NVMe.

Each per-config log ends with this shape (numbers elided):

```
====================== TreePass bench start ======================
 workload=C query_dist=zipf threads=4 cache=64 MB phase_ops=8000000
total throughput: <ops/sec>, elapsed: <sec>
Overall Statistics
------------------------------------------------------------------
| lookups:           <N>
| lookups found:     <N>
| lookups not found: <N>
------------------------------------------------------------------

Cache Statistics
------|---------|---------|----------|--------|-----|------|---------|
 type |  trunk  |  branch | memtable | filter | log | misc |  TOTAL  |
------|---------|---------|----------|--------|-----|------|---------|
 hits |   ...   |   ...   |    ...   |   ...  |  0  |   0  |   ...   |
 miss |   ...   |   ...   |    ...   |   ...  |  0  |   0  |   ...   |
 read |   ...   |   ...   |    ...   |   ...  |  0  |   0  |   ...   |
 hit% |  XX.XX% |  XX.XX% |   XX.XX% |  XX.XX%| 0.0%| 0.0% |  XX.XX% |
------|---------|---------|----------|--------|-----|------|---------|

(Allocator Stats block from stock SplinterDB ...)

====================== TreePass bench done  ======================
```

A run is successful when every per-config log finishes with the
`TreePass bench done` banner and a non-degenerate `hit ratio` row.

## How to verify the build only (no dataset)

Build just the TreePass library (~10 s on a modern box) and check
that the static archive landed:

```bash
cd splinterdb
export CC=gcc
make -j$(nproc) libs
ls build/release/lib/libsplinterdb.a
```

`make libs` skips the upstream functional / unit test targets the
smoke does not exercise, so it sidesteps any test-side compatibility
issue (e.g. older glibc missing the `gettid()` wrapper). A successful
build prints `Building archive build/release/lib/libsplinterdb.a`
and the `ls` command resolves to the archive path.

> **Env tip.** SplinterDB's upstream Makefile snapshots build config via
> `env | grep -E "BUILD_|CC"` and aborts when no match. `run_smoke.py`
> exports `CC=gcc` for the child make; if you invoke `make` directly
> under `splinterdb/`, run `export CC=gcc` first.

# License

Apache-2.0, inherited from SplinterDB. See `splinterdb/LICENSE`.
