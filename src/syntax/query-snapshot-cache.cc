#include "syntax/query-snapshot-cache.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace document_engine {

namespace {

void append_string(std::ostringstream &output, const std::string &value) {
  output << value.size() << ':' << value;
}

} // namespace

std::string QuerySnapshotCache::key_for(
    uint64_t buffer_revision, uint64_t language_generation,
    uint64_t source_identity,
    const std::string &query_type, const QueryRange &range,
    const QueryResolutionContext &resolution) {
  std::ostringstream output;
  output << buffer_revision << ':' << language_generation << ':'
         << source_identity << ':';
  append_string(output, query_type);
  output << ':' << range.start_row << ':' << range.start_column << ':'
         << range.end_row << ':' << range.end_column << ':'
         << resolution.resolve_scopes << ':' << resolution.interpolate_names
         << ':' << resolution.injection_depth << ':' << resolution.capture_limit
         << ':'
         << resolution.local_ranges_enabled << ':';
  for (const auto &[name, value] : resolution.config) {
    append_string(output, name);
    output << '=' << value.index() << ':';
    if (const bool *boolean = std::get_if<bool>(&value)) {
      output << *boolean;
    } else if (const double *number = std::get_if<double>(&value)) {
      uint64_t bits = 0;
      static_assert(sizeof(bits) == sizeof(*number));
      std::memcpy(&bits, number, sizeof(bits));
      output << bits;
    } else if (const std::string *text = std::get_if<std::string>(&value)) {
      append_string(output, *text);
    }
    output << ';';
  }
  output << '|';
  for (const auto &[start, end] : resolution.local_ranges)
    output << start << ':' << end << ';';
  return output.str();
}

std::shared_ptr<const QueryIndexSnapshot> QuerySnapshotCache::get(
    uint64_t buffer_revision, uint64_t language_generation,
    uint64_t source_identity,
    const std::string &query_type, const QueryRange &range,
    const QueryResolutionContext &resolution) {
  const std::string key = key_for(buffer_revision, language_generation,
                                  source_identity, query_type, range,
                                  resolution);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iterator = entries_.find(key);
  if (iterator == entries_.end()) {
    diagnostics_.misses++;
    return nullptr;
  }
  iterator->second.last_used = ++clock_;
  diagnostics_.hits++;
  return iterator->second.snapshot;
}

void QuerySnapshotCache::put(
    uint64_t buffer_revision, uint64_t language_generation,
    uint64_t source_identity,
    const std::string &query_type, const QueryRange &range,
    const QueryResolutionContext &resolution,
    std::shared_ptr<const QueryIndexSnapshot> snapshot) {
  if (!snapshot)
    return;
  const std::string key = key_for(buffer_revision, language_generation,
                                  source_identity, query_type, range,
                                  resolution);
  std::lock_guard<std::mutex> lock(mutex_);
  auto existing = entries_.find(key);
  if (existing != entries_.end()) {
    existing->second = Entry{std::move(snapshot), ++clock_};
    return;
  }
  if (entries_.size() >= QUERY_SNAPSHOT_CACHE_CAPACITY) {
    const auto oldest = std::min_element(
        entries_.begin(), entries_.end(),
        [](const auto &left, const auto &right) {
          return left.second.last_used < right.second.last_used;
        });
    if (oldest != entries_.end()) {
      entries_.erase(oldest);
      diagnostics_.evictions++;
    }
  }
  entries_.emplace(key, Entry{std::move(snapshot), ++clock_});
  diagnostics_.entries = entries_.size();
}

void QuerySnapshotCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  diagnostics_.entries = 0;
}

QuerySnapshotCacheDiagnostics QuerySnapshotCache::diagnostics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  QuerySnapshotCacheDiagnostics result = diagnostics_;
  result.entries = entries_.size();
  return result;
}

} // namespace document_engine
