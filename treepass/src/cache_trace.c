// Copyright 2024 VMware, Inc.
// SPDX-License-Identifier: Apache-2.0

/*
 * cache_trace.c --
 *
 *     Implementation of page-level cache event tracing.
 */

#include "cache_trace.h"

#ifdef CACHE_TRACE

#   include <errno.h>
#   include <fcntl.h>
#   include <stdio.h>
#   include <stdlib.h>
#   include <string.h>
#   include <sys/stat.h>
#   include <unistd.h>

static void
ensure_directory(const char *path)
{
   struct stat st;
   if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
      return;
   }
   mkdir(path, 0755);
}

void
cache_trace_init(cache_trace_state *ts, const char *output_dir)
{
   if (!ts) {
      return;
   }
   memset(ts, 0, sizeof(*ts));

   if (output_dir && output_dir[0]) {
      snprintf(ts->output_dir, sizeof(ts->output_dir), "%s", output_dir);
   } else {
      const char *env_dir = getenv("CACHE_TRACE_DIR");
      if (env_dir && env_dir[0]) {
         snprintf(ts->output_dir, sizeof(ts->output_dir), "%s", env_dir);
      } else {
         snprintf(ts->output_dir, sizeof(ts->output_dir), "/tmp/cache_trace");
      }
   }

   ensure_directory(ts->output_dir);

   for (int i = 0; i < MAX_THREADS; i++) {
      ts->per_thread[i].entries =
         (cache_trace_entry *)malloc(CACHE_TRACE_BUF_SIZE
                                     * sizeof(cache_trace_entry));
      ts->per_thread[i].count = 0;
      ts->per_thread[i].fd    = -1;
   }

   ts->initialized = true;
   platform_default_log("[CACHE_TRACE] Initialized, output_dir=%s\n",
                        ts->output_dir);
}

/*
 * Flush the buffer for a given thread to its output file.
 */
void
cache_trace_flush(cache_trace_state *ts, threadid tid)
{
   cache_trace_buffer *buf = &ts->per_thread[tid];
   if (buf->count == 0 || buf->entries == NULL) {
      return;
   }

   // Lazy open file
   if (buf->fd < 0) {
      char path[512];
      snprintf(path,
               sizeof(path),
               "%s/cache_trace_%02lu.bin",
               ts->output_dir,
               (unsigned long)tid);
      buf->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (buf->fd < 0) {
         platform_default_log(
            "[CACHE_TRACE] Failed to open %s: %s\n", path, strerror(errno));
         buf->count = 0;
         return;
      }
   }

   size_t bytes = buf->count * sizeof(cache_trace_entry);
   ssize_t written = write(buf->fd, buf->entries, bytes);
   if (written < 0) {
      platform_default_log("[CACHE_TRACE] Write error tid=%lu: %s\n",
                           (unsigned long)tid,
                           strerror(errno));
   }
   buf->count = 0;
}

void
cache_trace_deinit(cache_trace_state *ts)
{
   if (!ts || !ts->initialized) {
      return;
   }

   uint64 total_events = 0;
   for (int i = 0; i < MAX_THREADS; i++) {
      cache_trace_buffer *buf = &ts->per_thread[i];

      // Flush remaining
      if (buf->count > 0 && buf->entries != NULL) {
         cache_trace_flush(ts, i);
      }

      // Close file
      if (buf->fd >= 0) {
         // Count total events from file size
         off_t size = lseek(buf->fd, 0, SEEK_END);
         if (size > 0) {
            total_events += size / sizeof(cache_trace_entry);
         }
         close(buf->fd);
         buf->fd = -1;
      }

      // Free buffer
      if (buf->entries) {
         free(buf->entries);
         buf->entries = NULL;
      }
   }

   ts->initialized = false;
   platform_default_log("[CACHE_TRACE] Finished, total_events=%lu\n",
                        total_events);
}

#endif // CACHE_TRACE
