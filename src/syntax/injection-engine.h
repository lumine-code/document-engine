#ifndef LUMINE_DOCUMENT_ENGINE_INJECTION_ENGINE_H_
#define LUMINE_DOCUMENT_ENGINE_INJECTION_ENGINE_H_

#include "core-types.h"

#include <napi.h>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct SuperstringSnapshotLease;

namespace document_engine {

struct SessionState;
struct QueryRange;
struct SyntaxQuerySnapshot;
class PublishedSyntaxSnapshot;
class SnapshotLeaseReference;
class SyntaxBackend;
class InjectionCandidateWorker;
class InjectionLayerParseWorker;

void release_snapshot_lease_reference(
    const std::shared_ptr<SnapshotLeaseReference> &lease);

struct InjectionRevisionTags {
  uint64_t buffer_revision = 0;
  uint64_t syntax_revision = 0;
  uint64_t language_generation = 0;
};

struct InjectionRangeRecord {
  uint64_t start_index = 0;
  uint64_t end_index = 0;
  Point start;
  Point end;
  std::vector<std::string> scopes;
};

struct InjectionLayerRecord {
  uint64_t layer_id = 0;
  uint64_t parent_layer_id = 0;
  uint32_t depth = 0;
  uint32_t candidate_id = 0;
  uint32_t injection_point_id = 0;
  std::string parent_grammar_id;
  std::string language_name;
  std::string language_id;
  std::string runtime;
  std::string wasm_path;
  std::string language_name_export;
  std::string language_segment;
  std::map<std::string, std::vector<std::string>> query_paths;
  bool include_children = false;
  bool include_adjacent_whitespace = false;
  bool newlines_between = false;
  bool cover_shallower_scopes = false;
  bool query_defined = false;
  bool syntax_parsed = false;
  bool syntax_root_has_error = false;
  std::string syntax_error_code;
  std::string syntax_error_message;
  std::shared_ptr<const PublishedSyntaxSnapshot> syntax;
  std::shared_ptr<const SyntaxQuerySnapshot> queries;
  std::vector<InjectionRangeRecord> ranges;
};

struct InjectionRangeIndex {
  InjectionRevisionTags tags;
  uint64_t injection_point_generation = 0;
  std::vector<InjectionLayerRecord> layers;
  uint64_t dynamic_layer_count = 0;
  uint64_t query_layer_count = 0;
  uint64_t range_count = 0;
  uint64_t parsed_child_layer_count = 0;
  uint64_t failed_child_layer_count = 0;
  uint64_t maximum_depth = 0;
};

struct InjectionEngineDiagnostics {
  uint64_t active_request_count = 0;
  uint64_t candidate_count = 0;
  uint64_t active_layer_count = 0;
  uint64_t dynamic_layer_count = 0;
  uint64_t query_layer_count = 0;
  uint64_t range_count = 0;
  uint64_t parsed_child_layer_count = 0;
  uint64_t failed_child_layer_count = 0;
  uint64_t maximum_depth = 0;
  uint64_t unresolved_query_language_count = 0;
  uint64_t query_language_resolution_count = 0;
  uint64_t query_language_rejection_count = 0;
  uint64_t stale_request_count = 0;
  uint64_t aborted_request_count = 0;
  uint64_t published_generation = 0;
};

struct InjectionQuerySource {
  uint64_t layer_id = 0;
  uint64_t parent_layer_id = 0;
  uint32_t depth = 0;
  std::string grammar_id;
  std::shared_ptr<const PublishedSyntaxSnapshot> syntax;
  struct IncludedRange {
    Range range;
    std::vector<std::string> scopes;
  };
  std::vector<IncludedRange> included_ranges;
  bool cover_shallower_scopes = false;
};

class InjectionEngine {
public:
  InjectionEngine();
  ~InjectionEngine();

  InjectionEngine(const InjectionEngine &) = delete;
  InjectionEngine &operator=(const InjectionEngine &) = delete;

  void publish_root(
      std::shared_ptr<const PublishedSyntaxSnapshot> syntax,
      std::shared_ptr<const SyntaxQuerySnapshot> queries,
      const SnapshotAnalysis &analysis, const std::string &grammar_id,
      const void *snapshot_lease);
  void clear();

  std::shared_ptr<const InjectionRangeIndex> index_snapshot() const;
  InjectionEngineDiagnostics diagnostics() const;
  bool owns_syntax_snapshot(
      const std::shared_ptr<const PublishedSyntaxSnapshot> &syntax,
      const InjectionRevisionTags &tags) const;
  std::vector<InjectionQuerySource>
  query_sources(const QueryRange &range) const;

  Napi::Value get_candidates(const Napi::CallbackInfo &info,
                             const std::shared_ptr<SessionState> &session);
  Napi::Value resolve_node(const Napi::CallbackInfo &info,
                           const std::shared_ptr<SessionState> &session);
  Napi::Value resolve_query_node(
      const Napi::CallbackInfo &info,
      const std::shared_ptr<SessionState> &session);
  Napi::Value get_syntax_node_at_position(
      const Napi::CallbackInfo &info,
      const std::shared_ptr<SessionState> &session);
  Napi::Value get_syntax_node_containing_range(
      const Napi::CallbackInfo &info,
      const std::shared_ptr<SessionState> &session);
  Napi::Value apply_result_batch(const Napi::CallbackInfo &info,
                                 const std::shared_ptr<SessionState> &session);
  Napi::Value apply_results(const Napi::CallbackInfo &info,
                            const std::shared_ptr<SessionState> &session);
  Napi::Value apply_language_scopes(
      const Napi::CallbackInfo &info,
      const std::shared_ptr<SessionState> &session);
  Napi::Value apply_query_language_descriptors(
      const Napi::CallbackInfo &info,
      const std::shared_ptr<SessionState> &session);
  Napi::Value abort_request(const Napi::CallbackInfo &info,
                            const std::shared_ptr<SessionState> &session);

private:
  Napi::Value apply_result_object(
      Napi::Env env, Napi::Object batch,
      const std::shared_ptr<SessionState> &session);
  Napi::Value queue_child_parse(
      Napi::Env env, uint64_t request_id,
      const std::shared_ptr<SessionState> &session);
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend class InjectionCandidateWorker;
  friend class InjectionLayerParseWorker;
};

class InjectionNodeWrapper : public Napi::ObjectWrap<InjectionNodeWrapper> {
public:
  static Napi::Function init(Napi::Env env);
  static Napi::Object new_instance(
      Napi::Env env, const std::weak_ptr<SessionState> &session,
      std::shared_ptr<const PublishedSyntaxSnapshot> tree, uint32_t node_handle,
      const InjectionRevisionTags &tags,
      const SuperstringSnapshotLease *lease);
  static Napi::Object new_instance(
      Napi::Env env, const std::weak_ptr<SessionState> &session,
      std::shared_ptr<const PublishedSyntaxSnapshot> tree, uint32_t node_handle,
      const InjectionRevisionTags &tags,
      std::shared_ptr<SnapshotLeaseReference> lease);

  explicit InjectionNodeWrapper(const Napi::CallbackInfo &info);

private:
  Napi::Value get_id(const Napi::CallbackInfo &info);
  Napi::Value get_type(const Napi::CallbackInfo &info);
  Napi::Value get_text(const Napi::CallbackInfo &info);
  Napi::Value get_start_index(const Napi::CallbackInfo &info);
  Napi::Value get_end_index(const Napi::CallbackInfo &info);
  Napi::Value get_start_position(const Napi::CallbackInfo &info);
  Napi::Value get_end_position(const Napi::CallbackInfo &info);
  Napi::Value get_range(const Napi::CallbackInfo &info);
  Napi::Value get_parent(const Napi::CallbackInfo &info);
  Napi::Value get_children(const Napi::CallbackInfo &info);
  Napi::Value get_named_children(const Napi::CallbackInfo &info);
  Napi::Value get_child_count(const Napi::CallbackInfo &info);
  Napi::Value get_named_child_count(const Napi::CallbackInfo &info);
  Napi::Value get_first_child(const Napi::CallbackInfo &info);
  Napi::Value get_last_child(const Napi::CallbackInfo &info);
  Napi::Value get_first_named_child(const Napi::CallbackInfo &info);
  Napi::Value get_last_named_child(const Napi::CallbackInfo &info);
  Napi::Value get_previous_sibling(const Napi::CallbackInfo &info);
  Napi::Value get_next_sibling(const Napi::CallbackInfo &info);
  Napi::Value get_previous_named_sibling(const Napi::CallbackInfo &info);
  Napi::Value get_next_named_sibling(const Napi::CallbackInfo &info);
  Napi::Value get_is_named(const Napi::CallbackInfo &info);
  Napi::Value get_is_missing(const Napi::CallbackInfo &info);
  Napi::Value get_is_error(const Napi::CallbackInfo &info);
  Napi::Value get_has_error(const Napi::CallbackInfo &info);
  Napi::Value is_current(const Napi::CallbackInfo &info);
  Napi::Value assert_current(const Napi::CallbackInfo &info);
  Napi::Value child(const Napi::CallbackInfo &info);
  Napi::Value named_child(const Napi::CallbackInfo &info);
  Napi::Value child_for_field_name(const Napi::CallbackInfo &info);
  Napi::Value descendants_of_type(const Napi::CallbackInfo &info);
  Napi::Value get_snapshot_lease_id(const Napi::CallbackInfo &info);
  Napi::Value release_snapshot_lease(const Napi::CallbackInfo &info);

  std::weak_ptr<SessionState> session_;
  std::shared_ptr<const PublishedSyntaxSnapshot> tree_;
  uint32_t node_handle_ = 0;
  InjectionRevisionTags tags_;
  std::shared_ptr<SnapshotLeaseReference> lease_;
};

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_INJECTION_ENGINE_H_
