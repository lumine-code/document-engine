#ifndef LUMINE_DOCUMENT_ENGINE_CORE_TYPES_H_
#define LUMINE_DOCUMENT_ENGINE_CORE_TYPES_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace document_engine {

struct Point {
  uint64_t row = 0;
  uint64_t column = 0;
};

inline bool operator==(const Point &left, const Point &right) {
  return left.row == right.row && left.column == right.column;
}

inline bool operator!=(const Point &left, const Point &right) {
  return !(left == right);
}

inline bool operator<(const Point &left, const Point &right) {
  return left.row < right.row ||
         (left.row == right.row && left.column < right.column);
}

inline bool operator<=(const Point &left, const Point &right) {
  return left < right || left == right;
}

struct Range {
  Point start;
  Point end;
};

struct Fold {
  uint32_t id = 0;
  Range range;
};

struct SnapshotAnalysis {
  uint64_t utf16_length = 0;
  uint64_t checksum = 0;
  bool checksum_complete = false;
  std::vector<uint64_t> line_starts;
  std::vector<uint64_t> line_ends;
};

// One accepted text transaction. Every packed edit uses stride 8
// [oldStart, oldEnd, newStart, newEnd]. Edits inside a batch share the batch's
// before/after coordinate spaces; successive batches form a projection chain.
struct RevisionEditBatch {
  uint64_t from_revision = 0;
  uint64_t to_revision = 0;
  std::vector<uint32_t> edits;
  std::shared_ptr<const SnapshotAnalysis> before;
  std::shared_ptr<const SnapshotAnalysis> after;
};

struct SessionCounters {
  uint64_t revisions_requested = 0;
  uint64_t revisions_published = 0;
  uint64_t revisions_stale = 0;
  uint64_t revisions_superseded = 0;
  uint64_t snapshot_chunks_read = 0;
  uint64_t snapshot_lines_read = 0;
  uint64_t synchronous_analyses = 0;
  uint64_t synchronous_incremental_analyses = 0;
  uint64_t synchronous_line_index_analyses = 0;
  uint64_t synchronous_full_analyses = 0;
  uint64_t snapshot_input_bytes_copied = 0;
  uint64_t full_buffer_materializations = 0;
  uint64_t viewport_utf16_copied = 0;
  uint64_t syntax_parses = 0;
  uint64_t syntax_incremental_parses = 0;
  uint64_t syntax_cancelled_parses = 0;
  uint64_t syntax_input_too_large = 0;
  uint64_t syntax_fail_open = 0;
  uint64_t grammar_cache_hits = 0;
  uint64_t grammar_cache_misses = 0;
  uint64_t query_files_loaded = 0;
  uint64_t query_programs_compiled = 0;
  uint64_t query_programs_executed = 0;
  uint64_t query_captures = 0;
  uint64_t query_text_predicates = 0;
  uint64_t query_custom_predicates = 0;
  uint64_t query_unresolved_predicates = 0;
  uint64_t query_unresolved_regex_predicates = 0;
  uint64_t query_scope_predicates = 0;
  uint64_t query_cache_hits = 0;
  uint64_t query_cache_misses = 0;
  uint64_t query_cache_evictions = 0;
  uint64_t layered_query_executions = 0;
  uint64_t layered_query_layers = 0;
  uint64_t query_projected_captures = 0;
  uint64_t query_stale_captures = 0;
  uint64_t query_covered_captures = 0;
};

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_CORE_TYPES_H_
