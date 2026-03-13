#include "backlog_store.h"

#include <cstdio>

#include "esp_log.h"
#include "esp_spiffs.h"

/*
 * Backlog storage strategy:
 * - persist one JSON object per line
 * - keep operations simple and deterministic on embedded flash
 * - trade write efficiency for implementation clarity/reliability
 */

namespace {
constexpr const char* kTag = "BacklogStore";
constexpr const char* kBasePath = "/spiffs";

// Convert logical path (for example "/backlog.ndjson") into mounted VFS path.
std::string makeFsPath(const std::string& logical) {
  if (logical.empty()) return std::string(kBasePath) + "/backlog.ndjson";
  if (logical.front() == '/') return std::string(kBasePath) + logical;
  return std::string(kBasePath) + "/" + logical;
}
}  // namespace

bool BacklogStore::begin(const char* path, size_t max_lines) {
  path_ = makeFsPath(path ? path : "/backlog.ndjson");
  max_lines_ = max_lines;
  ESP_LOGI(kTag, "Mounting SPIFFS backlog store at %s (max lines: %u)",
           path_.c_str(), static_cast<unsigned>(max_lines_));

  // Mount SPIFFS once for this process. format_if_mount_failed=true prioritizes resilience.
  esp_vfs_spiffs_conf_t conf = {};
  conf.base_path = kBasePath;
  conf.partition_label = nullptr;
  conf.max_files = 8;
  conf.format_if_mount_failed = true;
  if (esp_vfs_spiffs_register(&conf) != ESP_OK) {
    ESP_LOGE(kTag, "SPIFFS mount failed");
    return false;
  }

  // Ensure file exists so later read/write calls have a valid target.
  FILE* f = fopen(path_.c_str(), "a+");
  if (!f) {
    ESP_LOGE(kTag, "Failed to create/open backlog file: %s", path_.c_str());
    return false;
  }
  fclose(f);

  // Build initial cached count from current file content.
  line_count_ = 0;
  f = fopen(path_.c_str(), "r");
  if (!f) {
    ESP_LOGE(kTag, "Failed to read backlog file: %s", path_.c_str());
    return false;
  }
  char buf[512];
  while (fgets(buf, sizeof(buf), f) != nullptr) {
    if (buf[0] != '\n' && buf[0] != '\0') ++line_count_;
  }
  fclose(f);
  ESP_LOGI(kTag, "Backlog init: %u lines", static_cast<unsigned>(line_count_));
  return true;
}

bool BacklogStore::appendLine(const std::string& line) {
  // Fast path: append row to tail.
  FILE* f = fopen(path_.c_str(), "a");
  if (!f) return false;
  const int rc = fprintf(f, "%s\n", line.c_str());
  fclose(f);
  if (rc < 0) return false;
  ++line_count_;

  if (line_count_ <= max_lines_) return true;

  // Compaction path: keep only newest max_lines_ rows.
  std::vector<std::string> keep;
  keep.reserve(max_lines_);
  f = fopen(path_.c_str(), "r");
  if (!f) return false;
  char buf[768];
  while (fgets(buf, sizeof(buf), f) != nullptr) {
    std::string row(buf);
    while (!row.empty() && (row.back() == '\n' || row.back() == '\r')) row.pop_back();
    if (row.empty()) continue;
    keep.push_back(row);
    if (keep.size() > max_lines_) keep.erase(keep.begin());
  }
  fclose(f);
  return compactFromLines(keep);
}

bool BacklogStore::prependLine(const std::string& line) {
  // Build new queue with failed line first, then existing rows (up to limit).
  std::vector<std::string> rows;
  rows.reserve(line_count_ + 1);
  rows.push_back(line);

  FILE* f = fopen(path_.c_str(), "r");
  if (!f) return false;
  char buf[768];
  while (fgets(buf, sizeof(buf), f) != nullptr) {
    std::string row(buf);
    while (!row.empty() && (row.back() == '\n' || row.back() == '\r')) row.pop_back();
    if (row.empty()) continue;
    rows.push_back(row);
    if (rows.size() > max_lines_) rows.pop_back();
  }
  fclose(f);
  return compactFromLines(rows);
}

bool BacklogStore::popOldestLine(std::string& line) {
  FILE* f = fopen(path_.c_str(), "r");
  if (!f) return false;

  // Read entire file once, return first data line, rewrite remaining lines.
  std::vector<std::string> remaining;
  remaining.reserve(line_count_ > 0 ? line_count_ - 1 : 0);

  bool first_found = false;
  char buf[768];
  while (fgets(buf, sizeof(buf), f) != nullptr) {
    std::string row(buf);
    while (!row.empty() && (row.back() == '\n' || row.back() == '\r')) row.pop_back();
    if (row.empty()) continue;
    if (!first_found) {
      line = row;
      first_found = true;
    } else {
      remaining.push_back(row);
    }
  }
  fclose(f);

  if (!first_found) return false;
  return compactFromLines(remaining);
}

bool BacklogStore::compactFromLines(const std::vector<std::string>& lines) {
  // Full rewrite keeps file format deterministic and line_count_ authoritative.
  FILE* f = fopen(path_.c_str(), "w");
  if (!f) return false;
  for (const auto& row : lines) {
    if (fprintf(f, "%s\n", row.c_str()) < 0) {
      fclose(f);
      return false;
    }
  }
  fclose(f);
  line_count_ = lines.size();
  return true;
}

bool BacklogStore::trimToNewest(size_t keep_lines) {
  if (line_count_ <= keep_lines) return true;

  std::vector<std::string> keep;
  keep.reserve(keep_lines);
  FILE* f = fopen(path_.c_str(), "r");
  if (!f) return false;

  char buf[768];
  while (fgets(buf, sizeof(buf), f) != nullptr) {
    std::string row(buf);
    while (!row.empty() && (row.back() == '\n' || row.back() == '\r')) row.pop_back();
    if (row.empty()) continue;
    keep.push_back(row);
    if (keep.size() > keep_lines) {
      keep.erase(keep.begin());
    }
  }
  fclose(f);
  return compactFromLines(keep);
}
