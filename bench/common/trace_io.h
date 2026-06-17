#pragma once

#include "key_generator.h"
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

struct TraceFileHeader {
  char     magic[4];          // "QKTR"
  uint32_t version;           // 1
  uint32_t dist_type;         // 0=unif, 1=zipf, 2=prefix, 3=latest
  uint32_t zipf_const_x1k;   // zipf_const * 1000
  uint32_t load_num;          // FLAGS_num
  uint32_t warmup_num;
  uint32_t phase_op_num;
  uint32_t num_repeats;
  char     reserved[32];      // zero-filled
};
static_assert(sizeof(TraceFileHeader) == 64, "Header must be 64 bytes");

static uint32_t dist_to_uint(const std::string& dist) {
  if (dist == "unif")   return 0;
  if (dist == "zipf")   return 1;
  if (dist == "prefix") return 2;
  if (dist == "latest") return 3;
  return 0xFF;
}

// Extract basename without extension from a file path.
// e.g. "/home/yang/trace_data/cluster052_index.txt" -> "cluster052_index"
static std::string path_stem(const std::string& path) {
  size_t slash = path.rfind('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  size_t dot = base.rfind('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  return base;
}

static std::string build_trace_path(const std::string& dir,
                                    const std::string& workload_name,
                                    const std::string& query_dist,
                                    double zipf_const,
                                    int load_num,
                                    int warmup_num,
                                    int phase_op_num,
                                    int num_repeats,
                                    int keyrange_num = 0,
                                    const std::string& trace_workload_file = "") {
  char buf[1024];
  if (query_dist == "zipf") {
    snprintf(buf, sizeof(buf), "%s/%s_%s_%.2f_%dM_%dM_%dM_r%d.bin",
             dir.c_str(),
             workload_name.c_str(),
             query_dist.c_str(),
             zipf_const,
             load_num / 1000000,
             warmup_num / 1000000,
             phase_op_num / 1000000,
             num_repeats);
  } else if (query_dist == "prefix" && keyrange_num > 1 && keyrange_num != 30) {
    snprintf(buf, sizeof(buf), "%s/%s_%s_kr%d_%dM_%dM_%dM_r%d.bin",
             dir.c_str(),
             workload_name.c_str(),
             query_dist.c_str(),
             keyrange_num,
             load_num / 1000000,
             warmup_num / 1000000,
             phase_op_num / 1000000,
             num_repeats);
  } else if (query_dist == "trace" && !trace_workload_file.empty()) {
    std::string stem = path_stem(trace_workload_file);
    snprintf(buf, sizeof(buf), "%s/%s_%s_%s_%dM_%dM_%dM_r%d.bin",
             dir.c_str(),
             workload_name.c_str(),
             query_dist.c_str(),
             stem.c_str(),
             load_num / 1000000,
             warmup_num / 1000000,
             phase_op_num / 1000000,
             num_repeats);
  } else {
    snprintf(buf, sizeof(buf), "%s/%s_%s_%dM_%dM_%dM_r%d.bin",
             dir.c_str(),
             workload_name.c_str(),
             query_dist.c_str(),
             load_num / 1000000,
             warmup_num / 1000000,
             phase_op_num / 1000000,
             num_repeats);
  }
  return std::string(buf);
}

static bool write_vector(FILE* fp, const key_vector& vec) {
  uint64_t num_keys = vec.size();
  uint32_t key_len = 0;
  uint32_t padding = 0;

  // Determine key_len: max length across all keys
  if (num_keys > 0) {
    for (const auto& k : vec) {
      if (k.size() > key_len)
        key_len = static_cast<uint32_t>(k.size());
    }
  }

  if (fwrite(&num_keys, sizeof(num_keys), 1, fp) != 1) return false;
  if (fwrite(&key_len,  sizeof(key_len),  1, fp) != 1) return false;
  if (fwrite(&padding,  sizeof(padding),  1, fp) != 1) return false;

  if (num_keys == 0 || key_len == 0)
    return true;

  // Write keys in a bulk buffer for efficiency
  const size_t chunk_size = 65536;  // keys per chunk
  std::vector<char> buf(chunk_size * key_len, 0);

  for (size_t i = 0; i < num_keys; ) {
    size_t batch = std::min(chunk_size, num_keys - i);
    std::memset(buf.data(), 0, batch * key_len);
    for (size_t j = 0; j < batch; ++j) {
      std::memcpy(buf.data() + j * key_len, vec[i + j].data(), vec[i + j].size());
    }
    if (fwrite(buf.data(), key_len, batch, fp) != batch) return false;
    i += batch;
  }
  return true;
}

static bool read_vector(FILE* fp, key_vector& vec) {
  uint64_t num_keys = 0;
  uint32_t key_len = 0;
  uint32_t padding = 0;

  if (fread(&num_keys, sizeof(num_keys), 1, fp) != 1) return false;
  if (fread(&key_len,  sizeof(key_len),  1, fp) != 1) return false;
  if (fread(&padding,  sizeof(padding),  1, fp) != 1) return false;

  if (num_keys == 0) {
    vec.clear();
    return true;
  }

  // Read all key data in a single fread
  std::vector<char> buf(static_cast<size_t>(num_keys) * key_len);
  if (fread(buf.data(), key_len, num_keys, fp) != num_keys) return false;

  vec.resize(num_keys);
  for (size_t i = 0; i < num_keys; ++i) {
    // Find actual string length (strip null padding)
    const char* start = buf.data() + i * key_len;
    size_t actual_len = strnlen(start, key_len);
    vec[i].assign(start, actual_len);
  }
  return true;
}

// ============================================================
// Load dataset binary cache (CSV → bin auto-conversion)
// ============================================================

struct LoadTraceHeader {
  char     magic[4];       // "LDTR"
  uint32_t version;        // 2 (v1: no val_sizes; v2: val_sizes appended after keys)
  uint64_t num_keys;
  uint32_t key_len;        // max key length in dataset
  uint32_t avg_val_size;   // average value size across loadable keys (0 for v1)
  char     reserved[8];    // zero-filled
};
static_assert(sizeof(LoadTraceHeader) == 32, "LoadTraceHeader must be 32 bytes");

// ============================================================
// MmapKeyArray: zero-copy mmap-based key array for load traces
// ============================================================

class MmapKeyArray {
  void*       mapped_    = nullptr;
  size_t      file_size_ = 0;
  size_t      num_keys_  = 0;
  uint32_t    key_len_   = 0;
  uint32_t    avg_val_   = 0;
  const char* data_      = nullptr;  // points to first key after header
  const uint32_t* val_sizes_ = nullptr;  // per-key val_size array (v2+), null for v1
  int         fd_        = -1;

public:
  MmapKeyArray() = default;
  ~MmapKeyArray() { close(); }

  // Non-copyable, movable
  MmapKeyArray(const MmapKeyArray&) = delete;
  MmapKeyArray& operator=(const MmapKeyArray&) = delete;
  MmapKeyArray(MmapKeyArray&& o) noexcept
    : mapped_(o.mapped_), file_size_(o.file_size_), num_keys_(o.num_keys_),
      key_len_(o.key_len_), avg_val_(o.avg_val_), data_(o.data_),
      val_sizes_(o.val_sizes_), fd_(o.fd_) {
    o.mapped_ = nullptr; o.fd_ = -1;
  }
  MmapKeyArray& operator=(MmapKeyArray&& o) noexcept {
    if (this != &o) { close(); mapped_ = o.mapped_; file_size_ = o.file_size_;
      num_keys_ = o.num_keys_; key_len_ = o.key_len_; avg_val_ = o.avg_val_;
      data_ = o.data_; val_sizes_ = o.val_sizes_; fd_ = o.fd_;
      o.mapped_ = nullptr; o.fd_ = -1; }
    return *this;
  }

  bool open(const std::string& path, size_t expected_num) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return false;

    struct stat st;
    if (fstat(fd_, &st) != 0) { ::close(fd_); fd_ = -1; return false; }
    file_size_ = st.st_size;

    mapped_ = mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd_, 0);
    if (mapped_ == MAP_FAILED) { mapped_ = nullptr; ::close(fd_); fd_ = -1; return false; }

    auto* hdr = reinterpret_cast<const LoadTraceHeader*>(mapped_);
    if (std::memcmp(hdr->magic, "LDTR", 4) != 0 || (hdr->version != 1 && hdr->version != 2)) {
      
      close(); return false;
    }
    if (hdr->num_keys < static_cast<uint64_t>(expected_num)) {
      
      close(); return false;
    }

    key_len_  = hdr->key_len;
    avg_val_  = hdr->avg_val_size;
    num_keys_ = expected_num;
    data_     = reinterpret_cast<const char*>(mapped_) + sizeof(LoadTraceHeader);

    // v2: per-key val_size array follows the key data
    if (hdr->version >= 2) {
      size_t keys_end = sizeof(LoadTraceHeader) + static_cast<size_t>(hdr->num_keys) * key_len_;
      if (keys_end + hdr->num_keys * sizeof(uint32_t) <= file_size_) {
        val_sizes_ = reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const char*>(mapped_) + keys_end);
        
      } else {
        
        val_sizes_ = nullptr;
      }
    }

    // Pin in memory so cgroup pressure won't evict trace pages
    if (mlock(mapped_, file_size_) != 0) {
      
    } else {
      
    }

    return true;
  }

  void close() {
    if (mapped_) { munmap(mapped_, file_size_); mapped_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    num_keys_ = 0;
  }

  size_t size() const { return num_keys_; }
  bool   empty() const { return num_keys_ == 0; }
  uint32_t key_len() const { return key_len_; }
  uint32_t avg_val_size() const { return avg_val_; }
  bool has_val_sizes() const { return val_sizes_ != nullptr; }
  uint32_t val_size(size_t i) const { return val_sizes_ ? val_sizes_[i] : avg_val_; }

  // Returns a std::string (24-byte copy, no heap alloc with SSO for short keys)
  std::string operator[](size_t i) const {
    const char* start = data_ + i * key_len_;
    size_t len = strnlen(start, key_len_);
    return std::string(start, len);
  }
};

// Build bin path from CSV path: /data/dry_run_673M/dry_run_673M.csv → .bin
static std::string build_load_bin_path(const std::string& csv_path) {
  std::string bin_path = csv_path;
  size_t dot = bin_path.rfind('.');
  if (dot != std::string::npos) {
    bin_path = bin_path.substr(0, dot);
  }
  bin_path += ".bin";
  return bin_path;
}

static bool save_load_trace(const std::string& path, const key_vector& keys,
                            uint32_t target_key_size = 0,
                            const std::vector<uint32_t>& val_sizes = {}) {
  FILE* fp = fopen(path.c_str(), "wb");
  if (!fp) {
    fprintf(stderr, "[LOAD-TRACE] Failed to open for writing: %s\n", path.c_str());
    return false;
  }

  uint32_t key_len = target_key_size;
  if (key_len == 0) {
    for (const auto& k : keys) {
      if (k.size() > key_len)
        key_len = static_cast<uint32_t>(k.size());
    }
  }

  bool has_vals = !val_sizes.empty() && val_sizes.size() == keys.size();

  uint32_t avg_val = 0;
  if (has_vals) {
    uint64_t sum = 0;
    for (auto v : val_sizes) sum += v;
    avg_val = static_cast<uint32_t>(sum / val_sizes.size());
  }

  LoadTraceHeader hdr;
  std::memset(&hdr, 0, sizeof(hdr));
  std::memcpy(hdr.magic, "LDTR", 4);
  hdr.version      = has_vals ? 2 : 1;
  hdr.num_keys     = keys.size();
  hdr.key_len      = key_len;
  hdr.avg_val_size = avg_val;

  if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) { fclose(fp); return false; }

  // Write keys in chunks
  const size_t chunk_size = 65536;
  std::vector<char> buf(chunk_size * key_len, 0);
  uint64_t num_keys = keys.size();

  for (size_t i = 0; i < num_keys; ) {
    size_t batch = std::min(chunk_size, num_keys - i);
    std::memset(buf.data(), 0, batch * key_len);
    for (size_t j = 0; j < batch; ++j) {
      std::memcpy(buf.data() + j * key_len, keys[i + j].data(), keys[i + j].size());
    }
    if (fwrite(buf.data(), key_len, batch, fp) != batch) { fclose(fp); return false; }
    i += batch;
  }

  // v2: write per-key val_sizes after key data
  if (has_vals) {
    if (fwrite(val_sizes.data(), sizeof(uint32_t), num_keys, fp) != num_keys) {
      fclose(fp); return false;
    }
    fprintf(stderr, "[LOAD-TRACE] v2: wrote %lu val_sizes, avg=%u\n",
            (unsigned long)num_keys, avg_val);
  }

  fclose(fp);
  return true;
}

static bool load_load_trace(const std::string& path, int expected_num, key_vector& keys) {
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp) return false;

  LoadTraceHeader hdr;
  if (fread(&hdr, sizeof(hdr), 1, fp) != 1) { fclose(fp); return false; }

  if (std::memcmp(hdr.magic, "LDTR", 4) != 0 || hdr.version != 1) {
    fprintf(stderr, "[LOAD-TRACE] Invalid magic/version in: %s\n", path.c_str());
    fclose(fp);
    return false;
  }

  if (hdr.num_keys < static_cast<uint64_t>(expected_num)) {
    fprintf(stderr, "[LOAD-TRACE] Not enough keys: file has %lu, need %d\n",
            (unsigned long)hdr.num_keys, expected_num);
    fclose(fp);
    return false;
  }

  // Read only expected_num keys (file may contain more for insert-heavy workloads)
  uint64_t num_keys = static_cast<uint64_t>(expected_num);
  uint32_t key_len = hdr.key_len;
  std::vector<char> buf(static_cast<size_t>(num_keys) * key_len);
  if (fread(buf.data(), key_len, num_keys, fp) != num_keys) {
    fclose(fp);
    return false;
  }
  fclose(fp);

  keys.resize(num_keys);
  for (size_t i = 0; i < num_keys; ++i) {
    const char* start = buf.data() + i * key_len;
    size_t actual_len = strnlen(start, key_len);
    keys[i].assign(start, actual_len);
  }
  return true;
}

