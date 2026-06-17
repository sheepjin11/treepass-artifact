// Copyright 2024 VMware, Inc.
// SPDX-License-Identifier: Apache-2.0

/*
 * cache_trace.h --
 *
 *     Page-level cache event tracing for offline analysis.
 *     Shared between clockcache and s3fifo.
 *
 *     Enable at compile time with -DCACHE_TRACE.
 *     Set CACHE_TRACE_DIR env var to control output directory
 *     (default: /tmp/cache_trace).
 *
 *     Output: per-thread binary files (cache_trace_XX.bin).
 *     Each record is 24 bytes.
 */

#pragma once

#include "platform.h"

/*
 * Event types
 */
typedef enum cache_trace_event {
   CACHE_TRACE_ALLOC     = 0, // New page allocated (starts dirty)
   CACHE_TRACE_ACCESS    = 1, // Cache hit (page already in cache)
   CACHE_TRACE_DISCARD   = 2, // Page leaves cache (eviction or forced discard)
   CACHE_TRACE_WRITEBACK = 3, // Dirty page written back by cleaner
   CACHE_TRACE_LOAD      = 4, // Page loaded from disk (cache miss, starts clean)
   CACHE_TRACE_SHUTDOWN_WB = 5, // Dirty page written back during shutdown
} cache_trace_event;

/*
 * Flag bits for the flags field
 */
#define CACHE_TRACE_FLAG_DIRTY   (1 << 0) // Page is dirty at event time
#define CACHE_TRACE_FLAG_WRITE   (1 << 1) // This access is a write
#define CACHE_TRACE_FLAG_EVICT   (1 << 2) // Discard by clock sweep (vs forced)

/*
 * Binary trace record (24 bytes, naturally aligned)
 */
typedef struct cache_trace_entry {
   uint64 timestamp;   // nanosecond monotonic (platform_get_timestamp)
   uint64 page_addr;   // disk address of the page
   uint8  event_type;  // cache_trace_event
   uint8  page_type;   // page_type enum (trunk, branch, memtable, ...)
   uint8  flags;       // CACHE_TRACE_FLAG_*
   uint8  node_class;  // node_class enum (COLD=0, WARM=1, HOT=2, META=3)
   uint8  _pad[4];     // padding to 24 bytes
} cache_trace_entry;

_Static_assert(sizeof(cache_trace_entry) == 24,
               "cache_trace_entry must be 24 bytes");

#ifdef CACHE_TRACE

#   define CACHE_TRACE_BUF_SIZE (16 * 1024) // 16K entries per thread

typedef struct cache_trace_buffer {
   cache_trace_entry *entries; // allocated at init time
   uint32             count;
   int                fd; // output file descriptor, -1 = not opened
} cache_trace_buffer;

typedef struct cache_trace_state {
   cache_trace_buffer per_thread[MAX_THREADS];
   char               output_dir[256];
   bool               initialized;
} cache_trace_state;

void
cache_trace_init(cache_trace_state *ts, const char *output_dir);

void
cache_trace_deinit(cache_trace_state *ts);

extern void
cache_trace_flush(cache_trace_state *ts, threadid tid);

static inline void
cache_trace_record(cache_trace_state *ts,
                   uint8              event_type,
                   uint64             page_addr,
                   uint8              page_type,
                   uint8              flags,
                   uint8              nclass)
{
   if (!ts || !ts->initialized) {
      return;
   }

   threadid tid = platform_get_tid();
   if (UNLIKELY(tid >= MAX_THREADS)) {
      return; // IO callback thread without valid tid
   }
   cache_trace_buffer *buf = &ts->per_thread[tid];

   if (UNLIKELY(buf->entries == NULL)) {
      return; // this thread's buffer was not allocated
   }

   cache_trace_entry *e = &buf->entries[buf->count];
   e->timestamp   = platform_get_timestamp();
   e->page_addr   = page_addr;
   e->event_type  = event_type;
   e->page_type   = page_type;
   e->flags       = flags;
   e->node_class  = nclass;
   buf->count++;

   if (UNLIKELY(buf->count >= CACHE_TRACE_BUF_SIZE)) {
      cache_trace_flush(ts, tid);
   }
}

#   define CACHE_TRACE_RECORD(ts, event, addr, ptype, fl, nc)                  \
      cache_trace_record((ts), (event), (addr), (ptype), (fl), (nc))

#else // !CACHE_TRACE

typedef struct cache_trace_state {
   char _unused;
} cache_trace_state;

#   define CACHE_TRACE_RECORD(ts, event, addr, ptype, fl, nc) ((void)0)

static inline void
cache_trace_init(cache_trace_state *ts, const char *output_dir)
{
   (void)ts;
   (void)output_dir;
}

static inline void
cache_trace_deinit(cache_trace_state *ts)
{
   (void)ts;
}

#endif // CACHE_TRACE
