#ifndef LUMINE_DOCUMENT_ENGINE_QUERY_ENGINE_H_
#define LUMINE_DOCUMENT_ENGINE_QUERY_ENGINE_H_

#include "core-types.h"
#include "syntax/scope-resolver.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct TSLanguage;
struct TSTree;

namespace document_engine {

class SnapshotReader;

using QueryCancellationFunction = bool (*)(void *payload);

constexpr uint64_t NATIVE_QUERY_CACHE_CAPACITY = UINT64_C(32);

struct QueryConfiguration {
  std::map<std::string, std::vector<std::string>> paths;
  std::map<std::string, std::string> sources;
  std::string language_segment;
  std::string grammar_identity;
  uint64_t grammar_generation = 0;
};

struct QueryOperand {
  bool capture = false;
  std::string value;
};

struct QueryPredicate {
  std::string operator_name;
  std::vector<QueryOperand> operands;
};

struct QueryProperty {
  std::string name;
  bool has_value = false;
  std::string value;
};

struct QueryPatternMetadata {
  std::vector<QueryProperty> set_properties;
  std::vector<QueryProperty> asserted_properties;
  std::vector<QueryProperty> refuted_properties;
  std::vector<QueryPredicate> custom_predicates;
  std::vector<QueryPredicate> unresolved_predicates;
  bool scope_regex_complete = true;
};

struct QueryCaptureRecord {
  uint32_t name_id = 0;
  uint32_t pattern_index = 0;
  uint32_t property_set_id = 0;
  uint32_t start_row = 0;
  uint32_t start_column = 0;
  uint32_t end_row = 0;
  uint32_t end_column = 0;
  uint32_t start_index = 0;
  uint32_t end_index = 0;
  // Native-only grouping and identity metadata. The public packed capture
  // contract intentionally remains the nine fields above.
  uint32_t match_id = 0;
  uint32_t node_symbol = 0;
  uint32_t layer_index = 0;
  uint32_t depth = 0;
  uint32_t order = 0;
  uint32_t flags = 0;
  uint32_t node_start_index = 0;
  uint32_t node_end_index = 0;
  uint32_t node_handle = 0;
  uint32_t layer_range_index = 0;
};

constexpr uint32_t QUERY_CAPTURE_LANGUAGE_SCOPE = UINT32_C(1);
constexpr uint32_t QUERY_CAPTURE_NAME_INTERPOLATED = UINT32_C(1) << 1u;

struct QueryLayerRangeRecord {
  uint32_t start_row = 0;
  uint32_t start_column = 0;
  uint32_t end_row = 0;
  uint32_t end_column = 0;
  uint32_t start_index = 0;
  uint32_t end_index = 0;
  std::vector<std::string> scopes;
};

struct QueryLayerRecord {
  uint64_t layer_id = 0;
  uint64_t parent_layer_id = 0;
  uint32_t depth = 0;
  std::string grammar_id;
  bool cover_shallower_scopes = false;
  std::vector<QueryLayerRangeRecord> ranges;
};

struct QueryIndexSnapshot {
  std::string query_type;
  uint64_t buffer_revision = 0;
  uint64_t language_generation = 0;
  std::vector<std::string> capture_names;
  std::vector<QueryPatternMetadata> patterns;
  std::vector<QueryLayerRecord> layers;
  std::vector<QueryCaptureRecord> captures;
  uint64_t raw_capture_count = 0;
  uint64_t accepted_capture_count = 0;
  uint64_t projected_capture_count = 0;
  uint64_t stale_capture_count = 0;
  uint64_t covered_capture_count = 0;
  ScopeResolutionStatistics scope_statistics;
  bool resolution_complete = true;
  bool exceeded_match_limit = false;
};

struct QueryRange {
  uint32_t start_row = 0;
  uint32_t start_column = 0;
  uint32_t end_row = UINT32_MAX;
  uint32_t end_column = UINT32_MAX;
};

// These indices intentionally share one packed representation. Their aliases
// make ownership explicit without coupling public MarkerLayer state to syntax.
using HighlightIndex = QueryIndexSnapshot;
using SyntaxFoldIndex = QueryIndexSnapshot;
using IndentIndex = QueryIndexSnapshot;
using LocalIndex = QueryIndexSnapshot;
using TagIndex = QueryIndexSnapshot;
using InjectionCaptureIndex = QueryIndexSnapshot;

struct SyntaxQuerySnapshot {
  uint64_t buffer_revision = 0;
  uint64_t language_generation = 0;
  std::map<std::string, QueryIndexSnapshot> indices;
};

struct QueryErrorInfo {
  std::string code;
  std::string message;
  std::string query_type;
  std::string file_path;
  uint32_t line = 0;
  uint32_t column = 0;
  uint32_t byte_offset = 0;
};

struct QueryRunStatistics {
  uint64_t files_loaded = 0;
  uint64_t programs_compiled = 0;
  uint64_t programs_executed = 0;
  uint64_t patterns = 0;
  uint64_t captures = 0;
  uint64_t text_predicates = 0;
  uint64_t custom_predicates = 0;
  uint64_t unresolved_predicates = 0;
  uint64_t unresolved_regex_predicates = 0;
  uint64_t scope_predicates = 0;
  uint64_t scope_captures_tested = 0;
  uint64_t scope_captures_rejected = 0;
  uint64_t scope_captures_adjusted = 0;
  uint64_t language_segment_substitutions = 0;
  uint64_t language_segment_warnings = 0;
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  uint64_t cache_evictions = 0;
  double compile_milliseconds = 0;
  double execute_milliseconds = 0;
};

class NativeQueryEngine {
public:
  static std::unique_ptr<NativeQueryEngine>
  compile(const TSLanguage *language, const QueryConfiguration &configuration,
          QueryErrorInfo &error, QueryRunStatistics &statistics);

  ~NativeQueryEngine();

  NativeQueryEngine(const NativeQueryEngine &) = delete;
  NativeQueryEngine &operator=(const NativeQueryEngine &) = delete;

  bool execute(const TSTree *tree, const SnapshotReader &reader,
               const SnapshotAnalysis &analysis,
               uint64_t buffer_revision, uint64_t language_generation,
               QueryCancellationFunction cancellation,
               void *cancellation_payload,
               std::shared_ptr<const SyntaxQuerySnapshot> &snapshot,
               QueryErrorInfo &error, QueryRunStatistics &statistics) const;

  bool execute_one(const std::string &query_type, const TSTree *tree,
                   const SnapshotReader &reader,
                   const SnapshotAnalysis &analysis,
                   uint64_t buffer_revision, uint64_t language_generation,
                   const QueryRange &range,
                   const QueryResolutionContext &resolution,
                   QueryCancellationFunction cancellation,
                   void *cancellation_payload,
                   std::shared_ptr<const QueryIndexSnapshot> &snapshot,
                   QueryErrorInfo &error,
                   QueryRunStatistics &statistics) const;

  std::vector<std::string>
  scope_config_keys(const std::string &query_type) const;

private:
  struct Impl;
  explicit NativeQueryEngine(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
};

const char *canonical_query_type(const std::string &query_type);

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_QUERY_ENGINE_H_
