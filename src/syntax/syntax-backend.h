#ifndef LUMINE_DOCUMENT_ENGINE_SYNTAX_BACKEND_H_
#define LUMINE_DOCUMENT_ENGINE_SYNTAX_BACKEND_H_

#include "core-types.h"
#include "syntax/query-engine.h"

#include <tree_sitter/api.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace document_engine {

class SnapshotReader;

using SyntaxSnapshotCancellationFunction = bool (*)(void *payload);

// Immutable published syntax state shared by on-demand queries and dynamic
// injections. Tree-sitter copies only increment the tree's internal reference
// count. Candidate node/type handles are populated lazily off the owner thread
// and are not part of the parse hot path.
class PublishedSyntaxSnapshot {
public:
  static std::shared_ptr<const PublishedSyntaxSnapshot>
  build(const TSTree *tree,
        std::shared_ptr<const NativeQueryEngine> query_engine,
        uint64_t buffer_revision, uint64_t language_generation);

  ~PublishedSyntaxSnapshot();
  PublishedSyntaxSnapshot(const PublishedSyntaxSnapshot &) = delete;
  PublishedSyntaxSnapshot &operator=(const PublishedSyntaxSnapshot &) = delete;

  uint64_t buffer_revision() const;
  uint64_t language_generation() const;
  uint64_t identity() const;
  uint64_t node_count() const;
  const TSTree *tree() const;
  std::shared_ptr<const NativeQueryEngine> query_engine() const;

  TSNode node(uint32_t handle) const;
  uint32_t handle(TSNode node) const;
  bool collect_handles_for_types(
      const std::set<std::string> &types,
      std::map<std::string, std::vector<uint32_t>> &result,
      SyntaxSnapshotCancellationFunction cancellation = nullptr,
      void *cancellation_payload = nullptr) const;
  uint32_t handle_for_range(uint32_t start_index, uint32_t end_index,
                            uint32_t symbol) const;

private:
  struct Impl;
  explicit PublishedSyntaxSnapshot(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

constexpr uint64_t MAX_SYNTAX_UTF16_LENGTH = UINT64_C(2147483647);
constexpr uint64_t SYNTAX_GRAMMAR_CACHE_CAPACITY = UINT64_C(32);

struct SyntaxBackendCapabilities {
  bool native_wasm = true;
  bool native_queries = true;
  bool native_query_cache = true;
  bool native_grammar_cache = true;
  const char *backend = "tree-sitter-0.27.0+wasmtime-48.0.1";
  const char *reason = "";
};

struct SyntaxConfiguration {
  std::string language_id;
  std::string runtime;
  std::string wasm_path;
  std::string language_name;
  QueryConfiguration queries;
  struct IncludedRange {
    Point start;
    Point end;
    uint64_t start_index = 0;
    uint64_t end_index = 0;
  };
  std::vector<IncludedRange> included_ranges;
  uint64_t generation = 0;

  bool enabled() const { return runtime == "wasm" && !wasm_path.empty(); }
};

struct SyntaxEdit {
  Point old_start;
  Point old_end;
  Point new_start;
  Point new_end;
};

struct SyntaxCancellation {
  const std::atomic<bool> *destroyed = nullptr;
  const std::atomic<uint64_t> *latest_revision = nullptr;
  const std::atomic<uint64_t> *language_generation = nullptr;
  uint64_t revision = 0;
  uint64_t expected_language_generation = 0;

  bool requested() const;
};

struct SyntaxParseResult {
  bool attempted = false;
  bool parsed = false;
  bool incremental = false;
  bool parse_skipped = false;
  bool tree_projected = false;
  bool tree_projection_fallback = false;
  bool cancelled = false;
  bool grammar_cache_hit = false;
  bool root_has_error = false;
  uint64_t checksum = 0;
  uint64_t node_count = 0;
  uint64_t wasm_bytes = 0;
  std::string grammar_fingerprint;
  std::string resolved_language_name;
  double grammar_load_milliseconds = 0;
  double parse_milliseconds = 0;
  std::shared_ptr<const SyntaxQuerySnapshot> query_snapshot;
  std::shared_ptr<const PublishedSyntaxSnapshot> published_snapshot;
  QueryRunStatistics query_statistics;
  QueryErrorInfo query_error;
  std::string root_type;
  std::string error_code;
  std::string error_message;
};

struct SyntaxGrammarCacheDiagnostics {
  uint64_t entries = 0;
  uint64_t compilations = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t capacity = SYNTAX_GRAMMAR_CACHE_CAPACITY;
  uint64_t evictions = 0;
};

class SyntaxBackend {
public:
  SyntaxBackend();
  ~SyntaxBackend();

  SyntaxBackend(const SyntaxBackend &) = delete;
  SyntaxBackend &operator=(const SyntaxBackend &) = delete;

  bool parse(const SnapshotReader &reader,
             std::shared_ptr<const SnapshotAnalysis> analysis,
             const SyntaxConfiguration &configuration,
             const std::vector<SyntaxEdit> &edits, uint64_t revision,
             uint64_t maximum_utf16_length,
             const SyntaxCancellation &cancellation,
             SyntaxParseResult &result);
  void reset();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const SyntaxBackendCapabilities &syntax_backend_capabilities();
SyntaxGrammarCacheDiagnostics syntax_grammar_cache_diagnostics();

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_SYNTAX_BACKEND_H_
