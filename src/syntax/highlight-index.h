#ifndef LUMINE_DOCUMENT_ENGINE_HIGHLIGHT_INDEX_H_
#define LUMINE_DOCUMENT_ENGINE_HIGHLIGHT_INDEX_H_

#include "display/render-plan.h"
#include "syntax/query-engine.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace document_engine {

constexpr uint32_t HIGHLIGHT_SHARD_ROWS = 128;
constexpr size_t HIGHLIGHT_SHARD_CAPACITY = 32;
constexpr size_t HIGHLIGHT_SHARD_BYTE_CAPACITY = 32 * 1024 * 1024;
constexpr uint32_t HIGHLIGHT_MAX_REQUEST_SHARDS = 8;
constexpr uint32_t HIGHLIGHT_CAPTURE_CAPACITY = 131072;
constexpr uint32_t HIGHLIGHT_MAX_COHORT_ENVELOPE_ROWS = 8192;
constexpr uint32_t HIGHLIGHT_MAX_COHORT_ENVELOPE_UTF16 = 8 * 1024 * 1024;

struct HighlightIndexTags {
  uint64_t buffer_revision = 0;
  uint64_t syntax_revision = 0;
  uint64_t language_generation = 0;
  uint64_t injection_generation = 0;
  uint64_t context_generation = 0;
};

inline bool operator==(const HighlightIndexTags &left,
                       const HighlightIndexTags &right) {
  return left.buffer_revision == right.buffer_revision &&
         left.syntax_revision == right.syntax_revision &&
         left.language_generation == right.language_generation &&
         left.injection_generation == right.injection_generation &&
         left.context_generation == right.context_generation;
}

inline bool operator!=(const HighlightIndexTags &left,
                       const HighlightIndexTags &right) {
  return !(left == right);
}

struct HighlightQueryShard {
  uint32_t start_row = 0;
  uint32_t end_row = 0;
  uint32_t start_offset = 0;
  uint32_t end_offset = 0;
  bool includes_end = false;
  std::shared_ptr<const QueryIndexSnapshot> query;
};

struct HighlightShardBounds {
  uint32_t start_row = 0;
  uint32_t end_row = 0;
  uint32_t start_offset = 0;
  uint32_t end_offset = 0;
  bool includes_end = false;
};

struct HighlightCandidateBatch {
  uint64_t request_id = 0;
  HighlightIndexTags tags;
  uint32_t coverage_start_row = 0;
  uint32_t coverage_end_row = 0;
  std::vector<uint32_t> requested_shards;
  std::vector<HighlightShardBounds> requested_shard_bounds;
  std::vector<HighlightQueryShard> shards;
  bool partition_by_final_range = false;
  std::vector<std::string> capture_names;
  std::vector<std::string> capture_grammar_ids;
  size_t retained_bytes = 0;
};

struct HighlightIndexDiagnostics {
  uint64_t generation = 0;
  uint64_t shard_count = 0;
  uint64_t retained_bytes = 0;
  uint64_t evictions = 0;
  uint64_t range_count = 0;
  uint64_t zero_length_range_count = 0;
};

class NativeHighlightIndex {
public:
  NativeHighlightIndex() = default;

  void invalidate();
  void activate(const HighlightIndexTags &tags);
  void prepare_request(const HighlightIndexTags &tags,
                       const std::vector<uint32_t> &requested_shards,
                       bool exclusive_cohort);
  bool active_for(const HighlightIndexTags &tags) const;
  const HighlightIndexTags &tags() const;
  uint64_t generation() const;

  bool covers(uint32_t start_row, uint32_t end_row,
              const HighlightIndexTags &tags);
  bool covers_shards(const std::vector<uint32_t> &shard_starts,
                     const HighlightIndexTags &tags);
  bool collect(uint32_t start_row, uint32_t end_row,
               const HighlightIndexTags &tags,
               std::vector<ScopedRange> &ranges);
  bool collect_shards(const std::vector<uint32_t> &shard_starts,
                      const HighlightIndexTags &tags,
                      std::vector<ScopedRange> &ranges);
  std::vector<uint32_t> missing_shard_starts(
      uint32_t start_row, uint32_t end_row,
      const HighlightIndexTags &tags) const;

  bool commit(const HighlightCandidateBatch &candidate,
              const std::vector<uint32_t> &scope_ids);
  HighlightIndexDiagnostics diagnostics() const;

private:
  struct IndexedRange {
    ScopedRange range;
    uint64_t layer_id = 0;
    uint32_t pattern_id = 0;
    uint32_t node_start_offset = 0;
    uint32_t node_end_offset = 0;
  };

  using RangeIdentityKey =
      std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint64_t, uint32_t,
                 uint32_t, uint32_t, uint32_t>;

  struct Shard {
    uint32_t start_row = 0;
    uint32_t end_row = 0;
    std::vector<IndexedRange> ranges;
    size_t retained_bytes = 0;
    uint64_t last_used = 0;
  };

  void clear_shards();
  bool has_shard(uint32_t start_row) const;
  void append_shard_ranges(const Shard &shard,
                           std::set<RangeIdentityKey> &seen,
                           std::vector<ScopedRange> &ranges) const;

  HighlightIndexTags tags_;
  bool tags_initialized_ = false;
  uint64_t generation_ = 0;
  uint64_t clock_ = 0;
  uint64_t evictions_ = 0;
  size_t retained_bytes_ = 0;
  size_t scope_id_bytes_ = 0;
  std::vector<uint32_t> cohort_shards_;
  std::map<uint32_t, Shard> shards_;
  std::map<std::pair<std::string, std::string>, uint32_t> scope_ids_;
};

uint32_t highlight_shard_start(uint32_t row);
uint32_t highlight_shard_end(uint32_t start_row);

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_HIGHLIGHT_INDEX_H_
