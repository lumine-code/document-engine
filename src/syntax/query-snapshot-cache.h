#ifndef LUMINE_DOCUMENT_ENGINE_QUERY_SNAPSHOT_CACHE_H_
#define LUMINE_DOCUMENT_ENGINE_QUERY_SNAPSHOT_CACHE_H_

#include "syntax/query-engine.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace document_engine {

constexpr size_t QUERY_SNAPSHOT_CACHE_CAPACITY = 32;

struct QuerySnapshotCacheDiagnostics {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t evictions = 0;
  uint64_t entries = 0;
};

class QuerySnapshotCache {
public:
  std::shared_ptr<const QueryIndexSnapshot>
  get(uint64_t buffer_revision, uint64_t language_generation,
      uint64_t source_identity,
      const std::string &query_type, const QueryRange &range,
      const QueryResolutionContext &resolution);
  void put(uint64_t buffer_revision, uint64_t language_generation,
           uint64_t source_identity,
           const std::string &query_type, const QueryRange &range,
           const QueryResolutionContext &resolution,
           std::shared_ptr<const QueryIndexSnapshot> snapshot);
  void clear();
  QuerySnapshotCacheDiagnostics diagnostics() const;

private:
  struct Entry {
    std::shared_ptr<const QueryIndexSnapshot> snapshot;
    uint64_t last_used = 0;
  };

  static std::string key_for(uint64_t buffer_revision,
                             uint64_t language_generation,
                             uint64_t source_identity,
                             const std::string &query_type,
                             const QueryRange &range,
                             const QueryResolutionContext &resolution);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  uint64_t clock_ = 0;
  QuerySnapshotCacheDiagnostics diagnostics_;
};

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_QUERY_SNAPSHOT_CACHE_H_
