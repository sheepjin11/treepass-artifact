#pragma once

#include "config.h"
#include "zipf.h"  // ScrambledZipfianGenerator

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using key_type   = std::string;
using key_vector = std::vector<key_type>;

// ---------------------------------------------------------------------------
// Uniform distribution over the loaded key range.
// ---------------------------------------------------------------------------
template <typename KeyArray>
key_vector get_search_keys_unif(const KeyArray& array, int num_keys, int num_searches) {
  std::mt19937_64 gen(std::random_device{}());
  std::uniform_int_distribution<int> dist(0, num_keys - 1);
  key_vector keys;
  keys.reserve(num_searches);
  for (int i = 0; i < num_searches; ++i) {
    keys.push_back(array[dist(gen)]);
  }
  return keys;
}

// ---------------------------------------------------------------------------
// Scrambled Zipfian distribution over the loaded key range.
// ---------------------------------------------------------------------------
template <typename KeyArray>
key_vector get_search_keys_zipf(const KeyArray& array,
                                int num_existing_keys,
                                int /* num_future_keys */,
                                int num_searches) {
  int keyspace = std::max(num_existing_keys, 1);
  ScrambledZipfianGenerator zipf(keyspace, FLAGS_zipf_const);
  key_vector keys;
  keys.reserve(num_searches);
  for (int i = 0; i < num_searches; ++i) {
    int pos;
    do { pos = zipf.nextValue(); } while (pos < 0 || pos >= num_existing_keys);
    keys.push_back(array[pos]);
  }
  return keys;
}

// ---------------------------------------------------------------------------
// MixGraph-style prefix distribution: hot ranges contribute most of the
// queries, within-range offsets follow a power-law.
// ---------------------------------------------------------------------------
namespace key_gen_internal {
  template <typename KeyArray>
  inline key_vector& sorted_keys(const KeyArray& array, int num_keys) {
    static key_vector sv;
    if (sv.empty() || static_cast<int>(sv.size()) != num_keys) {
      sv.clear();
      sv.reserve(num_keys);
      for (int i = 0; i < num_keys; ++i) sv.push_back(array[i]);
      std::sort(sv.begin(), sv.end());
    }
    return sv;
  }
}  // namespace key_gen_internal

template <typename KeyArray>
key_vector get_search_keys_prefix_dist(const KeyArray& array,
                                       int num_keys,
                                       int num_searches,
                                       Random64& rand_gen,
                                       GenerateTwoTermExpKeys& gen_exp,
                                       double key_dist_a,
                                       double key_dist_b,
                                       bool use_prefix_modeling) {
  const auto& sorted = key_gen_internal::sorted_keys(array, num_keys);
  key_vector keys;
  keys.reserve(num_searches);
  for (int i = 0; i < num_searches; ++i) {
    int64_t ini_rand = GetRandomKey(&rand_gen);
    int64_t key_rand = gen_exp.DistGetKeyID(ini_rand, key_dist_a, key_dist_b,
                                            use_prefix_modeling);
    keys.push_back(sorted[static_cast<size_t>(key_rand % num_keys)]);
  }
  return keys;
}
