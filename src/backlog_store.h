#pragma once

#include <cstddef>
#include <string>
#include <vector>

/*
 * BacklogStore persists newline-delimited JSON records (NDJSON) on SPIFFS.
 *
 * Design goals:
 * - survive temporary network outages by buffering outbound payloads
 * - cap storage growth with a maximum line count
 * - keep API simple for append/requeue/dequeue workflow
 *
 * Queue behavior:
 * - appendLine() adds newest data to tail
 * - popOldestLine() returns head for FIFO replay
 * - prependLine() pushes a failed replay row back to head
 */
class BacklogStore {
 public:
  // Mount SPIFFS and prepare/create the backlog file.
  bool begin(const char* path, size_t max_lines);

  // Append to the tail (newest entry).
  bool appendLine(const std::string& line);

  // Insert at the head (used when publish fails and payload must be retried first).
  bool prependLine(const std::string& line);

  // Remove and return the oldest queued entry.
  bool popOldestLine(std::string& line);

  // Cached line count to avoid scanning on every call.
  size_t countLines() const { return line_count_; }

  // Keep only the newest N lines, dropping older cached rows.
  bool trimToNewest(size_t keep_lines);

 private:
  // Rewrite file from in-memory lines and refresh cached count.
  bool compactFromLines(const std::vector<std::string>& lines);

  // Physical file path under SPIFFS VFS mount.
  std::string path_;
  // Upper bound to prevent unbounded flash growth.
  size_t max_lines_ = 0;
  // Cached current number of non-empty lines in file.
  size_t line_count_ = 0;
};
