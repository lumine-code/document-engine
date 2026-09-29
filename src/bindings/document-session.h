#ifndef LUMINE_DOCUMENT_ENGINE_DOCUMENT_SESSION_H_
#define LUMINE_DOCUMENT_ENGINE_DOCUMENT_SESSION_H_

#include "core-types.h"

#include <napi.h>

#include <atomic>
#include <memory>
#include <map>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

struct SuperstringSnapshotLease;

namespace document_engine {

class AddonData;
struct RevisionRequest;
struct DisplayViewState;
class DocumentSessionWrapper;
class InjectionEngine;
class NativeHighlightIndex;
class PublishedSyntaxSnapshot;
class QuerySnapshotCache;
class SyntaxBackend;
struct HighlightCandidateBatch;
struct HighlightCoverageRequest;
struct SyntaxQuerySnapshot;

class NativeJobControl {
public:
  explicit NativeJobControl(const SuperstringSnapshotLease *lease = nullptr);
  ~NativeJobControl();

  const SuperstringSnapshotLease *lease() const { return lease_; }
  const SuperstringSnapshotLease *take_lease();
  void release_lease_on_owner();
  void mark_worker_finished();
  bool worker_finished() const;

private:
  std::atomic<bool> worker_finished_{false};
  std::thread::id owner_thread_;
  const SuperstringSnapshotLease *lease_ = nullptr;
};

struct PendingFoldReset {
  std::shared_ptr<DisplayViewState> view;
  uint64_t from_generation = 0;
  uint64_t to_generation = 0;
  std::vector<uint32_t> ranges;
};

struct SessionState {
  explicit SessionState(AddonData *addon_data);
  std::mutex mutex;
  AddonData *addon_data = nullptr;
  bool destroyed = false;
  bool active = false;
  uint32_t active_injection_jobs = 0;
  uint32_t active_highlight_jobs = 0;
  bool async_highlight_mode = false;
  bool owner_referenced = false;
  DocumentSessionWrapper *owner = nullptr;
  uint32_t worker_delay_ms = 0;
  bool use_snapshot_line_index = true;
  uint64_t maximum_syntax_utf16_length = UINT64_C(2147483647);
  uint64_t latest_requested_revision = 0;
  uint64_t buffer_revision = 0;
  uint64_t syntax_revision = 0;
  const SuperstringSnapshotLease *current_lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  const SuperstringSnapshotLease *syntax_lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> syntax_analysis;
  std::vector<RevisionEditBatch> edits_since_syntax;
  std::shared_ptr<const SyntaxQuerySnapshot> query_snapshot;
  std::shared_ptr<const PublishedSyntaxSnapshot> published_syntax;
  std::unique_ptr<RevisionRequest> pending;
  std::vector<std::shared_ptr<NativeJobControl>> native_jobs;
  std::vector<Napi::Promise::Deferred> drain_waiters;
  SessionCounters counters;
  std::atomic<bool> destroyed_signal{false};
  std::atomic<bool> environment_cleanup_signal{false};
  std::atomic<uint64_t> latest_requested_signal{0};
  std::atomic<uint64_t> language_generation_signal{0};
  std::atomic<uint64_t> highlight_cancellation_signal{0};
  std::unique_ptr<SyntaxBackend> syntax_backend;
  std::unique_ptr<InjectionEngine> injection_engine;
  std::unique_ptr<QuerySnapshotCache> query_snapshot_cache;
  std::unique_ptr<NativeHighlightIndex> highlight_index;
  std::shared_ptr<HighlightCoverageRequest> active_highlight_request;
  std::shared_ptr<HighlightCoverageRequest> pending_highlight_request;
  std::map<uint64_t, std::shared_ptr<HighlightCandidateBatch>>
      staged_highlight_batches;
  uint64_t next_highlight_request_id = 1;
  std::vector<std::weak_ptr<DisplayViewState>> display_views;
  std::unordered_set<uint64_t> transient_node_lease_ids;

  std::string language_id;
  std::string runtime;
  std::string wasm_path;
  std::string language_name;
  std::string language_segment;
  std::map<std::string, std::vector<std::string>> query_paths;
  std::map<std::string, std::string> query_sources;
  uint64_t query_file_count = 0;
  uint64_t language_generation = 0;
  uint64_t syntax_checksum = 0;
  uint64_t syntax_node_count = 0;
  uint64_t syntax_wasm_bytes = 0;
  bool syntax_root_has_error = false;
  bool syntax_last_incremental = false;
  bool syntax_last_grammar_cache_hit = false;
  double syntax_grammar_load_milliseconds = 0;
  double syntax_parse_milliseconds = 0;
  double query_compile_milliseconds = 0;
  double query_execute_milliseconds = 0;
  double synchronous_analysis_milliseconds = 0;
  double highlight_queue_milliseconds = 0;
  double highlight_query_milliseconds = 0;
  double highlight_commit_milliseconds = 0;
  uint64_t query_pattern_count = 0;
  uint64_t query_capture_count = 0;
  uint64_t query_language_segment_substitutions = 0;
  uint64_t query_language_segment_warnings = 0;
  std::string query_last_error;
  std::string query_last_error_code;
  std::string query_last_error_type;
  std::string query_last_error_path;
  uint32_t query_last_error_line = 0;
  uint32_t query_last_error_column = 0;
  std::string syntax_root_type;
  std::string syntax_resolved_language_name;
  std::string syntax_grammar_fingerprint;
  std::string syntax_last_error;
  std::string syntax_last_error_code;
  std::string syntax_disabled_reason;

  ~SessionState();
};

void finish_injection_job(Napi::Env env,
                          const std::shared_ptr<SessionState> &state);
void finish_injection_job_without_js(
    const std::shared_ptr<SessionState> &state);
void finish_highlight_job(Napi::Env env,
                          const std::shared_ptr<SessionState> &state);
void resolve_session_drains_if_idle(
    Napi::Env env, const std::shared_ptr<SessionState> &state);
void begin_environment_cleanup(
    const std::shared_ptr<SessionState> &state);
bool environment_cleanup_complete(
    const std::shared_ptr<SessionState> &state);
void register_native_job_locked(
    SessionState &state, const std::shared_ptr<NativeJobControl> &job);

struct RevisionRequest {
  RevisionRequest(Napi::Env env, const SuperstringSnapshotLease *lease,
                  uint64_t revision);
  ~RevisionRequest();

  Napi::Promise::Deferred deferred;
  const SuperstringSnapshotLease *lease = nullptr;
  uint64_t revision = 0;
  uint64_t language_generation = 0;
  std::vector<uint32_t> edits;
  std::vector<uint32_t> syntax_edits;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  uint64_t chunks_read = 0;
  uint64_t lines_read = 0;
  std::string language_id;
  std::string runtime;
  std::string wasm_path;
  std::string language_name;
  std::string language_segment;
  std::map<std::string, std::vector<std::string>> query_paths;
  std::map<std::string, std::string> query_sources;
  std::vector<PendingFoldReset> fold_resets;
};

class DocumentSessionWrapper : public Napi::ObjectWrap<DocumentSessionWrapper> {
public:
  static Napi::Function init(Napi::Env env);

  explicit DocumentSessionWrapper(const Napi::CallbackInfo &info);
  ~DocumentSessionWrapper() override;

private:
  Napi::Value apply_revision(const Napi::CallbackInfo &info);
  Napi::Value create_display_view(const Napi::CallbackInfo &info);
  Napi::Value set_language(const Napi::CallbackInfo &info);
  Napi::Value configure_syntax(const Napi::CallbackInfo &info);
  Napi::Value get_query_captures(const Napi::CallbackInfo &info);
  Napi::Value get_query_requirements(const Napi::CallbackInfo &info);
  Napi::Value request_highlight_coverage(const Napi::CallbackInfo &info);
  Napi::Value commit_highlight_coverage(const Napi::CallbackInfo &info);
  Napi::Value abort_highlight_coverage(const Napi::CallbackInfo &info);
  Napi::Value invalidate_highlight_index(const Napi::CallbackInfo &info);
  Napi::Value use_synchronous_highlights(const Napi::CallbackInfo &info);
  Napi::Value get_injection_candidates(const Napi::CallbackInfo &info);
  Napi::Value resolve_injection_node(const Napi::CallbackInfo &info);
  Napi::Value resolve_query_node(const Napi::CallbackInfo &info);
  Napi::Value get_syntax_node_at_position(const Napi::CallbackInfo &info);
  Napi::Value get_syntax_node_containing_range(
      const Napi::CallbackInfo &info);
  Napi::Value apply_injection_result_batch(const Napi::CallbackInfo &info);
  Napi::Value apply_injection_results(const Napi::CallbackInfo &info);
  Napi::Value apply_injection_language_scopes(const Napi::CallbackInfo &info);
  Napi::Value apply_query_language_descriptors(
      const Napi::CallbackInfo &info);
  Napi::Value abort_injection_request(const Napi::CallbackInfo &info);
  Napi::Value get_diagnostics(const Napi::CallbackInfo &info);
  Napi::Value track_snapshot_leases(const Napi::CallbackInfo &info);
  Napi::Value release_snapshot_leases(const Napi::CallbackInfo &info);
  Napi::Value drain(const Napi::CallbackInfo &info);
  Napi::Value destroy(const Napi::CallbackInfo &info);

  void mark_destroyed(Napi::Env env);

  std::shared_ptr<SessionState> state_;
};

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_DOCUMENT_SESSION_H_
