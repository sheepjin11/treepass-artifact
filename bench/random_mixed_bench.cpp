// random_mixed_bench: minimal YCSB-style driver for the TreePass artifact.
//
// Supports just enough to run the smoke matrix (workload_name ∈ {load, A, C}
// against query_dist ∈ {unif, zipf, prefix}). All scan/delete/rmw, tail-
// latency, trace-replay, num_repeats >1, and dataset-generation code paths
// are removed; the canonical bench script is run_smoke.py at the repo root.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "key_generator.h"
#include "trace_io.h"
#include "splinterdb_interface.h"

#include "gflags/gflags.h"

static MmapKeyArray trace;


// ------------------------------------------------------------
// Trace loading: read a one-key-per-line CSV (optionally
// `key,value_size`), persist it as an LDTR v2 binary on first run, then
// mmap that binary into the global `trace` array.
// ------------------------------------------------------------
static void GetTraceFromFile(int trace_num) {
  const std::string csv_path = FLAGS_trace_load_file.empty()
                                   ? FLAGS_trace_name
                                   : FLAGS_trace_load_file;
  if (csv_path.empty()) {
    fprintf(stderr, "[Error] one of --trace_name or --trace_load_file is required\n");
    exit(1);
  }

  // Derive .bin sibling path for the LDTR cache.
  std::string bin_path = csv_path;
  if (auto dot = bin_path.rfind('.'); dot != std::string::npos) {
    bin_path = bin_path.substr(0, dot);
  }
  bin_path += ".bin";

  if (!std::ifstream(bin_path).good()) {
    FILE* fp = fopen(csv_path.c_str(), "r");
    if (!fp) {
      fprintf(stderr, "[Error] failed to open CSV: %s\n", csv_path.c_str());
      exit(1);
    }
    key_vector tmp_trace;
    std::vector<uint32_t> tmp_val_sizes;
    tmp_trace.reserve(trace_num);
    tmp_val_sizes.reserve(trace_num);

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
      }
      if (len == 0) continue;

      uint32_t val_sz = static_cast<uint32_t>(FLAGS_value_size);
      size_t key_len = len;
      if (char* comma = static_cast<char*>(memchr(line, ',', len))) {
        key_len = comma - line;
        val_sz  = static_cast<uint32_t>(strtoul(comma + 1, nullptr, 10));
      }
      tmp_trace.emplace_back(line, key_len);
      tmp_val_sizes.push_back(val_sz);
    }
    fclose(fp);

    uint32_t max_key_len = 0;
    for (auto& k : tmp_trace) {
      max_key_len = std::max<uint32_t>(max_key_len, k.size());
    }
    if (max_key_len != static_cast<uint32_t>(FLAGS_key_size)) {
      FLAGS_key_size = static_cast<int>(max_key_len);
    }
    if (!save_load_trace(bin_path, tmp_trace, FLAGS_key_size, tmp_val_sizes)) {
      fprintf(stderr, "[Error] failed to save LDTR binary: %s\n", bin_path.c_str());
      exit(1);
    }
  }

  if (!trace.open(bin_path, trace_num)) {
    fprintf(stderr, "[Error] failed to mmap LDTR: %s\n", bin_path.c_str());
    exit(1);
  }
  if (trace.key_len() != static_cast<uint32_t>(FLAGS_key_size)) {
    FLAGS_key_size = static_cast<int>(trace.key_len());
  }
}

// ------------------------------------------------------------
// Workload fraction parsing
// ------------------------------------------------------------
// EXTERNAL_CONFIG passes one comma-separated set of fractions:
//   insert, update, read, scan, delete, rmw
// Scan/delete/rmw are not supported by this minimal bench; they must be 0.
struct WorkloadFracs {
  double insert = 0;
  double update = 0;
  double read   = 0;
};

static WorkloadFracs ParseWorkloadFracs(const std::string& s) {
  WorkloadFracs out;
  std::vector<double> v;
  std::stringstream ss(s);
  for (std::string tok; std::getline(ss, tok, ',');) {
    v.push_back(std::stod(tok));
  }
  if (v.size() != 6) {
    fprintf(stderr,
            "[Error] --workload_frac_list expects 6 comma-separated values "
            "(insert,update,read,scan,delete,rmw); got %zu\n",
            v.size());
    exit(1);
  }
  if (v[3] != 0 || v[4] != 0 || v[5] != 0) {
    fprintf(stderr,
            "[Error] scan/delete/rmw are not supported by this minimal bench; "
            "fractions [3..5] must be 0\n");
    exit(1);
  }
  out.insert = v[0];
  out.update = v[1];
  out.read   = v[2];
  return out;
}

// ------------------------------------------------------------
// Key generation per distribution
// ------------------------------------------------------------
static key_vector GenerateKeys(const std::string& dist,
                               size_t num_existing,
                               size_t num_searches) {
  key_vector keys;
  keys.reserve(num_searches);
  if (dist == "unif") {
    keys = get_search_keys_unif(trace, static_cast<int>(num_existing),
                                static_cast<int>(num_searches));
  } else if (dist == "zipf") {
    keys = get_search_keys_zipf(trace, static_cast<int>(num_existing),
                                /*num_future_keys=*/0,
                                static_cast<int>(num_searches));
  } else if (dist == "prefix") {
    GenerateTwoTermExpKeys gen;
    Random64 rng(static_cast<uint64_t>(std::random_device{}()));
    gen.InitiateExpDistribution(
        static_cast<int64_t>(num_existing),
        FLAGS_keyrange_dist_a, FLAGS_keyrange_dist_b,
        FLAGS_keyrange_dist_c, FLAGS_keyrange_dist_d,
        /*use_prefix_modeling=*/true);
    keys = get_search_keys_prefix_dist(
        trace, static_cast<int>(num_existing),
        static_cast<int>(num_searches),
        rng, gen,
        FLAGS_key_dist_a, FLAGS_key_dist_b,
        /*use_prefix_modeling=*/true);
  } else {
    fprintf(stderr, "[Error] unsupported query_dist '%s' "
            "(expected unif|zipf|prefix)\n", dist.c_str());
    exit(1);
  }
  return keys;
}

// ------------------------------------------------------------
// One-shot phase runner: spawn kNumThreads, each consumes a contiguous
// slice of the (read_keys, update_keys) vectors and issues mixed I/U
// operations. Returns the wall-clock elapsed time in nanoseconds.
// ------------------------------------------------------------
template <typename DB>
static double RunOneRWPhase(DB& db, size_t num_threads,
                            const key_vector& read_keys,
                            const key_vector& update_keys) {
  std::atomic<bool> ok{true};
  auto thread_fn = [&](size_t tid) {
    splinterdb_register_thread(db.get_raw_handle());
    RandomGenerator val_gen;
    size_t reads_per_thread = read_keys.size() / num_threads;
    size_t r_start          = tid * reads_per_thread;
    size_t r_end            = (tid == num_threads - 1) ? read_keys.size()
                                                       : r_start + reads_per_thread;
    size_t updates_per_thread = update_keys.size() / num_threads;
    size_t u_start = tid * updates_per_thread;
    size_t u_end   = (tid == num_threads - 1) ? update_keys.size()
                                              : u_start + updates_per_thread;

    // Interleave reads and updates so the cache sees the YCSB-like mix.
    size_t r_idx = r_start, u_idx = u_start;
    while (r_idx < r_end || u_idx < u_end) {
      if (r_idx < r_end) {
        std::string out;
        if (!db.Read(read_keys[r_idx], &out)) ok = false;
        r_idx++;
      }
      if (u_idx < u_end) {
        size_t val_sz = static_cast<size_t>(FLAGS_value_size);
        char*  value  = val_gen.Generate(val_sz);
        if (!db.Update(update_keys[u_idx], value, val_sz)) ok = false;
        u_idx++;
      }
    }
    splinterdb_deregister_thread(db.get_raw_handle());
  };

  auto t0 = std::chrono::high_resolution_clock::now();
  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (size_t t = 0; t < num_threads; ++t) {
    threads.emplace_back(thread_fn, t);
  }
  for (auto& th : threads) th.join();
  auto t1 = std::chrono::high_resolution_clock::now();
  if (!ok) {
    fprintf(stderr, "[Error] phase aborted: a DB op returned false\n");
    exit(1);
  }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

// ------------------------------------------------------------
// LOAD: each thread inserts its slice of the trace keys.
// ------------------------------------------------------------
template <typename DB>
static double RunLoadPhase(DB& db, size_t num_threads, size_t load_num) {
  std::atomic<bool> ok{true};
  size_t per_thread = load_num / num_threads;
  size_t remainder  = load_num % num_threads;
  auto thread_fn = [&](size_t tid) {
    splinterdb_register_thread(db.get_raw_handle());
    RandomGenerator val_gen;
    size_t start = tid * per_thread;
    size_t end   = start + per_thread + (tid == num_threads - 1 ? remainder : 0);
    for (size_t k = start; k < end; ++k) {
      size_t val_sz = static_cast<size_t>(FLAGS_value_size);
      char*  value  = val_gen.Generate(val_sz);
      if (!db.Insert(trace[k], value, val_sz)) ok = false;
    }
    splinterdb_deregister_thread(db.get_raw_handle());
  };
  auto t0 = std::chrono::high_resolution_clock::now();
  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (size_t t = 0; t < num_threads; ++t) {
    threads.emplace_back(thread_fn, t);
  }
  for (auto& th : threads) th.join();
  auto t1 = std::chrono::high_resolution_clock::now();
  if (!ok) {
    fprintf(stderr, "[Error] load aborted: a DB op returned false\n");
    exit(1);
  }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

// ------------------------------------------------------------
// Driver
// ------------------------------------------------------------
template <typename DB>
static void RunBenchmark() {
  WorkloadFracs fracs   = ParseWorkloadFracs(FLAGS_workload_frac_list);
  const size_t kNumThreads = static_cast<size_t>(FLAGS_thread_num);
  const bool   is_load     = (FLAGS_workload_name == "load");

  // Total trace footprint: load keys (always FLAGS_num).
  GetTraceFromFile(FLAGS_num);

  DB db;
  db.InitializeDatabase();

  // -------------------- LOAD --------------------
  if (is_load) {
    size_t load_num = static_cast<size_t>(FLAGS_num * FLAGS_load_frac);
    if (load_num == 0) {
      fprintf(stderr, "[Error] workload=load requires --load_frac > 0\n");
      exit(1);
    }
    double elapsed = RunLoadPhase(db, kNumThreads, load_num);
    double thru    = static_cast<double>(load_num) / elapsed * 1e9;
    std::cout << "Load throughput: " << thru << " ops/sec"
              << ", elapsed: " << elapsed / 1e9 << " sec"
              << ", load_num: " << load_num << std::endl;
    db.ShutdownDatabase();
    return;
  }

  // -------------------- QUERY (warmup + timed) --------------------
  const size_t warmup_num = static_cast<size_t>(FLAGS_warmup_num);
  const size_t phase_num  = static_cast<size_t>(FLAGS_phase_op_num);
  const size_t num_keys   = static_cast<size_t>(FLAGS_num);

  // Pre-generate read + update key vectors. Sized by op count × fraction.
  size_t warm_reads   = static_cast<size_t>(warmup_num * fracs.read);
  size_t warm_updates = static_cast<size_t>(warmup_num * fracs.update);
  size_t phase_reads  = static_cast<size_t>(phase_num  * fracs.read);
  size_t phase_updates = static_cast<size_t>(phase_num * fracs.update);

  key_vector warm_read_keys   = GenerateKeys(FLAGS_query_dist, num_keys, warm_reads);
  key_vector warm_update_keys = GenerateKeys(FLAGS_query_dist, num_keys, warm_updates);
  key_vector read_keys        = GenerateKeys(FLAGS_query_dist, num_keys, phase_reads);
  key_vector update_keys      = GenerateKeys(FLAGS_query_dist, num_keys, phase_updates);

  // Warmup phase (untimed; we reset stats right after it).
  if (warmup_num > 0) {
    (void)RunOneRWPhase(db, kNumThreads, warm_read_keys, warm_update_keys);
    if (FLAGS_use_stats) db.ResetStats();
  }

  // Timed phase.
  double elapsed = RunOneRWPhase(db, kNumThreads, read_keys, update_keys);
  size_t total_ops = read_keys.size() + update_keys.size();
  double thru = static_cast<double>(total_ops) / elapsed * 1e9;
  std::cout << "total throughput: " << thru << " ops/sec"
            << ", elapsed: " << elapsed / 1e9 << " sec" << std::endl;

  if (FLAGS_use_stats) db.PrintTrunkLookupStats();
  db.ShutdownDatabase();
}

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage("TreePass YCSB-style bench driver.");
  gflags::ParseCommandLineFlags(&argc, &argv, /*remove_flags=*/true);

  std::cout << "==================================== TreePass bench start ===================================="
            << "\n"
            << " workload="  << FLAGS_workload_name
            << " query_dist=" << FLAGS_query_dist
            << " threads="   << FLAGS_thread_num
            << " cache="     << FLAGS_block_cache_capacity << " MB"
            << " phase_ops=" << FLAGS_phase_op_num
            << std::endl;
  RunBenchmark<SplinterDBInterface>();
  std::cout << "==================================== TreePass bench done  ====================================" << std::endl;
  return 0;
}
