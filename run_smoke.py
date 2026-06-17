#!/usr/bin/env python3
"""TreePass artifact smoke runner.

Runs the YCSB A-F matrix across {unif, zipf, prefix} query distributions
against TreePass on a small synthetic load. Self-contained: builds the
TreePass library and the bench binary, then drives the bench directly.

Usage:
    python3 run_smoke.py [--threads N] [--cache-mb N] [--phase-ops N]
"""
from __future__ import annotations

import argparse
import datetime
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TREEPASS_DIR = ROOT / "splinterdb"
BENCH_BUILD = ROOT / "build" / "tp_only"
TREEPASS_LIB_BUILD = Path("/tmp/treepass-bench-build")
LOAD_CSV = ROOT / "examples" / "smoke_load.csv"
RESULT_DIR = ROOT / "result"

# Default db_path is the repo root so the smoke runs on a fresh clone with
# no extra setup. Pass --db-path /mnt/nvme (or any NVMe mount) for paper-
# grade timing.
DEFAULT_DB_PATH = str(ROOT)

# Smoke dataset shape — matches what examples/gen_smoke_load.py produces.
KEY_SIZE = 24
VALUE_SIZE = 100
LOAD_NUM = 1_000_000

# Representative YCSB workloads. Frac slots: insert, update, read, scan,
# delete, rmw.  A (50/50 read/update) covers the write-mixed case; C (100 %
# read) covers the read-only case. The other YCSB letters mostly vary the
# read/update ratio between these two, and the insert-heavy variants
# (D, E) would need a larger load CSV to keep their inserts in-range.
WORKLOADS = [
    ("A", "0,0.5,0.5,0,0,0"),  # 50/50 read/update
    ("C", "0,0,1,0,0,0"),      # 100 % read
]

DISTRIBUTIONS = ["unif", "zipf", "prefix"]


def sh(cmd, *, env=None, cwd=None):
    """Run a command with stdout/stderr passed through; abort on failure."""
    print(f"$ {' '.join(map(str, cmd))}", flush=True)
    subprocess.check_call([str(c) for c in cmd], env=env, cwd=cwd)


def ensure_load_csv() -> None:
    if LOAD_CSV.is_file():
        return
    print(f"[smoke] {LOAD_CSV} missing; generating...")
    sh([sys.executable, ROOT / "examples" / "gen_smoke_load.py"])


def build_treepass(jobs: int) -> None:
    env = os.environ.copy()
    env.setdefault("CC", "gcc")  # Makefile config snapshot needs at least one of CC/BUILD_*
    if TREEPASS_LIB_BUILD.exists():
        shutil.rmtree(TREEPASS_LIB_BUILD, ignore_errors=True)
    env["BUILD_ROOT"] = str(TREEPASS_LIB_BUILD)
    # 'libs' alone builds libsplinterdb.a (what bench links against) and
    # skips the upstream functional/unit test targets that pull in extra
    # SplinterDB internals the smoke does not exercise.
    sh(["make", "-j", str(jobs), "libs"], cwd=TREEPASS_DIR, env=env)


def build_bench(jobs: int) -> Path:
    BENCH_BUILD.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.setdefault("CC", "gcc")
    env["BUILD_ROOT"] = str(TREEPASS_LIB_BUILD)
    sh(
        [
            "cmake",
            str(ROOT),
            f"-B{BENCH_BUILD}",
            "-Dvalue=TREEPASS",
            "-DBUILD_MODE=release",
            "-DBUILD_ASAN=0",
            "-DBUILD_UBSAN=0",
            "-DBENCH_NAME=random_mixed",
        ],
        env=env,
    )
    sh(["make", "-C", BENCH_BUILD, "-j", str(jobs)], env=env)
    binary = BENCH_BUILD / "bench" / "random_mixed_bench"
    if not binary.is_file():
        sys.exit(f"[smoke] bench binary missing at {binary}")
    return binary


def run_phase(
    binary: Path,
    *,
    db_path: str,
    workload_name: str,
    workload_frac_list: str,
    query_dist: str,
    cache_mb: int,
    threads: int,
    phase_ops: int,
    is_load: bool,
    log_path: Path,
) -> None:
    warmup_ops = 0 if is_load else phase_ops // 2
    cmd = [
        binary,
        "--db_type=TREEPASS",
        f"--db_path={db_path}",
        f"--num={LOAD_NUM}",
        f"--key_size={KEY_SIZE}",
        f"--value_size={VALUE_SIZE}",
        f"--query_dist={query_dist}",
        f"--trace_name={LOAD_CSV}",
        f"--trace_load_file={LOAD_CSV}",
        f"--warmup_num={warmup_ops}",
        f"--phase_op_num={phase_ops}",
        f"--block_cache_capacity={cache_mb}",
        f"--db_size_in_GB={2}",
        f"--thread_num={threads}",
        f"--workload_name={workload_name}",
        f"--workload_frac_list={workload_frac_list}",
        f"--load_frac={1 if is_load else 0}",
        f"--use_existing_db={'false' if is_load else 'true'}",
        "--use_stats=true",
        "--use_direct_io=true",
    ]
    log_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"[smoke] -> {log_path.name}")
    with log_path.open("w") as f:
        f.write(f"# cmd: {' '.join(map(str, cmd))}\n")
        f.flush()
        subprocess.check_call([str(c) for c in cmd], stdout=f, stderr=subprocess.STDOUT)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--threads", type=int, default=4,
                   help="Bench thread count (default: 4)")
    p.add_argument("--cache-mb", type=int, default=64,
                   help="Cache size in MB for the query phase (default: 64). "
                        "Write-heavy YCSB phases (A, D, F) need this much "
                        "headroom for memtable + compaction pages; smaller "
                        "values may trip the 'cache locked' assertion.")
    p.add_argument("--load-cache-mb", type=int, default=512,
                   help="Cache size in MB for the load phase. Loads need more "
                        "headroom for memtable / compaction. (default: 512)")
    p.add_argument("--phase-ops", type=int, default=8_000_000,
                   help="Timed ops per query phase (default: 2,000,000)")
    p.add_argument("--db-path", default=DEFAULT_DB_PATH,
                   help="Directory holding the SplinterDB data file. "
                        "Defaults to the repo root so the smoke runs on a "
                        "fresh clone with no extra setup. Pass an NVMe mount "
                        "(e.g. --db-path /mnt/nvme) for paper-grade timing.")
    p.add_argument("--build-jobs", type=int, default=max(1, os.cpu_count() or 4),
                   help="Parallel make jobs (default: nproc)")
    p.add_argument("--skip-build", action="store_true",
                   help="Reuse existing TreePass + bench binaries.")
    return p.parse_args()


def main() -> None:
    args = parse_args()
    ensure_load_csv()
    if not args.skip_build:
        build_treepass(args.build_jobs)
    binary = build_bench(args.build_jobs)

    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out = RESULT_DIR / f"smoke_{ts}"
    out.mkdir(parents=True, exist_ok=True)
    print(f"[smoke] writing logs under {out}")

    # 1. LOAD with large cache (one-time priming).
    run_phase(
        binary,
        db_path=args.db_path,
        workload_name="load",
        workload_frac_list="0,0,0,0,0,0",
        query_dist="unif",
        cache_mb=args.load_cache_mb,
        threads=args.threads,
        phase_ops=args.phase_ops,
        is_load=True,
        log_path=out / "TREEPASS_load.txt",
    )

    # 2. YCSB A / C across unif / zipf / prefix.
    for dist in DISTRIBUTIONS:
        for name, fracs in WORKLOADS:
            log = out / f"TREEPASS_YCSB_{name}_{dist}.txt"
            run_phase(
                binary,
                db_path=args.db_path,
                workload_name=name,
                workload_frac_list=fracs,
                query_dist=dist,
                cache_mb=args.cache_mb,
                threads=args.threads,
                phase_ops=args.phase_ops,
                is_load=False,
                log_path=log,
            )
    print(f"[smoke] done — see {out}")


if __name__ == "__main__":
    main()
