#include "bindings/document-session.h"

#include "bindings/addon-data.h"
#include "bindings/display-view.h"
#include "bindings/highlight-index-bindings.h"
#include "snapshot-lease.h"
#include "syntax/injection-engine.h"
#include "syntax/highlight-index.h"
#include "syntax/query-engine.h"
#include "syntax/layered-query.h"
#include "syntax/query-snapshot-cache.h"
#include "syntax/syntax-backend.h"
#include "text-bridge/snapshot-reader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <set>
#include <string_view>
#include <thread>

namespace document_engine {

namespace {

void set_error_code(Napi::Error &error, const char *code) {
  error.Value().Set("code", Napi::String::New(error.Env(), code));
}

Napi::Value throw_type_error(Napi::Env env, const char *message,
                             const char *code) {
  Napi::TypeError error = Napi::TypeError::New(env, message);
  set_error_code(error, code);
  error.ThrowAsJavaScriptException();
  return env.Undefined();
}

Napi::Value throw_error(Napi::Env env, const char *message, const char *code) {
  Napi::Error error = Napi::Error::New(env, message);
  set_error_code(error, code);
  error.ThrowAsJavaScriptException();
  return env.Undefined();
}

bool read_uint64(Napi::Value value, uint64_t *result) {
  if (value.IsBigInt()) {
    bool lossless = false;
    *result = value.As<Napi::BigInt>().Uint64Value(&lossless);
    return lossless;
  }
  if (!value.IsNumber())
    return false;
  const double number = value.As<Napi::Number>().DoubleValue();
  if (number < 0 || number > 9007199254740991.0 || number != number)
    return false;
  *result = static_cast<uint64_t>(number);
  return static_cast<double>(*result) == number;
}

bool is_uint32_array(Napi::Value value) {
  return value.IsTypedArray() &&
         value.As<Napi::TypedArray>().TypedArrayType() == napi_uint32_array;
}

void release_lease(const SuperstringSnapshotLease *lease) {
  if (lease != nullptr && lease->functions != nullptr)
    lease->functions->release(lease->context);
}

Napi::Error coded_error(Napi::Env env, const char *message, const char *code) {
  Napi::Error error = Napi::Error::New(env, message);
  set_error_code(error, code);
  return error;
}

std::string checksum_string(uint64_t checksum) {
  char buffer[17]{};
  std::snprintf(buffer, sizeof(buffer), "%016llx",
                static_cast<unsigned long long>(checksum));
  return buffer;
}

void resolve_revision_without_syntax(Napi::Env env,
                                     RevisionRequest &request) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("accepted", Napi::Boolean::New(env, true));
  result.Set("bufferRevision",
             Napi::Number::New(env, static_cast<double>(request.revision)));
  result.Set("syntaxRevision", Napi::Number::New(env, 0));
  result.Set("checksum",
             Napi::String::New(
                 env, checksum_string(request.analysis
                                          ? request.analysis->checksum
                                          : UINT64_C(0))));
  result.Set("syntaxParsed", Napi::Boolean::New(env, false));
  result.Set("syntaxIncremental", Napi::Boolean::New(env, false));
  result.Set("syntaxDisabledReason", Napi::String::New(env, ""));
  result.Set("syntaxErrorCode", Napi::String::New(env, ""));
  result.Set("syntaxChecksum", Napi::String::New(env, ""));
  result.Set("syntaxRootType", Napi::String::New(env, ""));
  result.Set("resolvedLanguageName", Napi::String::New(env, ""));
  result.Set("syntaxRootHasError", Napi::Boolean::New(env, false));
  result.Set("syntaxNodeCount", Napi::Number::New(env, 0));
  result.Set("grammarLoadMilliseconds", Napi::Number::New(env, 0));
  result.Set("grammarCacheHit", Napi::Boolean::New(env, false));
  result.Set("parseMilliseconds", Napi::Number::New(env, 0));
  result.Set("queryCaptureCount", Napi::Number::New(env, 0));
  result.Set("queryCompileMilliseconds", Napi::Number::New(env, 0));
  result.Set("queryExecuteMilliseconds", Napi::Number::New(env, 0));
  request.deferred.Resolve(result);
}

void clear_query_state(SessionState &state) {
  state.query_snapshot.reset();
  if (state.query_snapshot_cache)
    state.query_snapshot_cache->clear();
  state.query_compile_milliseconds = 0;
  state.query_execute_milliseconds = 0;
  state.query_pattern_count = 0;
  state.query_capture_count = 0;
  state.query_language_segment_substitutions = 0;
  state.query_language_segment_warnings = 0;
  state.query_last_error.clear();
  state.query_last_error_code.clear();
  state.query_last_error_type.clear();
  state.query_last_error_path.clear();
  state.query_last_error_line = 0;
  state.query_last_error_column = 0;
}

void clear_display_highlights(SessionState &state) {
  for (auto iterator = state.display_views.begin();
       iterator != state.display_views.end();) {
    if (std::shared_ptr<DisplayViewState> view = iterator->lock()) {
      view->highlight_ranges.clear();
      view->highlight_buffer_revision = 0;
      view->highlight_syntax_revision = 0;
      iterator++;
    } else {
      iterator = state.display_views.erase(iterator);
    }
  }
  if (state.highlight_index) {
    state.highlight_cancellation_signal.fetch_add(1,
                                                   std::memory_order_relaxed);
    state.highlight_index->invalidate();
    state.staged_highlight_batches.clear();
  }
}

void reject_request(Napi::Env env, std::unique_ptr<RevisionRequest> request,
                    const char *message, const char *code) {
  if (!request)
    return;
  request->deferred.Reject(coded_error(env, message, code).Value());
}

void resolve_drains_if_idle(Napi::Env env,
                            const std::shared_ptr<SessionState> &state) {
  if (state->environment_cleanup_signal.load(std::memory_order_acquire)) {
    if (state->addon_data != nullptr)
      state->addon_data->notify_cleanup_progress();
    return;
  }
  std::vector<Napi::Promise::Deferred> waiters;
  DocumentSessionWrapper *owner_to_release = nullptr;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active || state->pending || state->active_injection_jobs > 0 ||
        state->active_highlight_jobs > 0 ||
        state->pending_highlight_request ||
        !state->staged_highlight_batches.empty())
      return;
    waiters.swap(state->drain_waiters);
    if (state->owner_referenced) {
      state->owner_referenced = false;
      owner_to_release = state->owner;
    }
  }
  for (auto &waiter : waiters)
    waiter.Resolve(env.Undefined());
  if (owner_to_release != nullptr)
    owner_to_release->Unref();
}

void queue_request(Napi::Env env, const std::shared_ptr<SessionState> &state,
                   std::unique_ptr<RevisionRequest> request);

std::string language_name_for(const std::string &wasm_path,
                              const std::string &explicit_name) {
  if (!explicit_name.empty())
    return explicit_name;
  const size_t separator = wasm_path.find_last_of("/\\");
  const size_t start = separator == std::string::npos ? 0 : separator + 1;
  const size_t extension = wasm_path.find_last_of('.');
  const size_t length = extension == std::string::npos || extension < start
                            ? std::string::npos
                            : extension - start;
  std::string name = wasm_path.substr(start, length);
  constexpr std::string_view prefix = "tree-sitter-";
  if (name.starts_with(prefix))
    name.erase(0, prefix.size());
  for (char &character : name) {
    if (character == '-')
      character = '_';
  }
  return name;
}

bool syntax_checksum_cancelled(void *payload) {
  return payload != nullptr &&
         static_cast<const SyntaxCancellation *>(payload)->requested();
}

std::string syntax_fail_open_reason(const std::string &code) {
  if (code == "ERR_SYNTAX_INPUT_TOO_LARGE")
    return "input-too-large";
  if (code.starts_with("ERR_SYNTAX_QUERY_"))
    return "query-error";
  if (code.starts_with("ERR_SYNTAX_WASM_") ||
      code == "ERR_SYNTAX_RUNTIME_INIT_FAILED" ||
      code == "ERR_SYNTAX_LANGUAGE_FAILED")
    return "grammar-error";
  if (code == "ERR_SYNTAX_INCLUDED_RANGES")
    return "configuration-error";
  if (code == "ERR_SYNTAX_PARSE_FAILED")
    return "parse-error";
  return {};
}

class RevisionWorker final : public Napi::AsyncWorker {
public:
  RevisionWorker(Napi::Env env, std::shared_ptr<SessionState> state,
                 std::unique_ptr<RevisionRequest> request,
                 std::shared_ptr<NativeJobControl> job)
      : Napi::AsyncWorker(env, "DocumentSession.applyRevision"),
        state_(std::move(state)), request_(std::move(request)),
        job_(std::move(job)) {}

  void OnExecute(Napi::Env env) override {
    Napi::AsyncWorker::OnExecute(env);
    job_->mark_worker_finished();
    if (state_->addon_data != nullptr)
      state_->addon_data->notify_cleanup_progress();
  }

  void Execute() override {
    uint32_t delay = 0;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      delay = state_->worker_delay_ms;
    }
    const auto delay_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
    while (!state_->destroyed_signal.load(std::memory_order_relaxed)) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= delay_deadline)
        break;
      std::this_thread::sleep_until(
          std::min(delay_deadline, now + std::chrono::milliseconds(10)));
    }

    SnapshotReader reader(job_->lease());
    if (!reader.valid()) {
      error_code_ = "ERR_SNAPSHOT_ANALYSIS_FAILED";
      SetError(reader.error());
      return;
    }
    SyntaxConfiguration configuration;
    configuration.language_id = request_->language_id;
    configuration.runtime = request_->runtime;
    configuration.wasm_path = request_->wasm_path;
    configuration.language_name = request_->language_name;
    configuration.queries.paths = request_->query_paths;
    configuration.queries.sources = request_->query_sources;
    configuration.queries.language_segment = request_->language_segment;
    configuration.queries.grammar_identity =
        request_->wasm_path + '\n' + request_->language_name;
    configuration.queries.grammar_generation = request_->language_generation;
    configuration.generation = request_->language_generation;
    SyntaxCancellation cancellation{
        &state_->destroyed_signal, &state_->latest_requested_signal,
        &state_->language_generation_signal, request_->revision,
        request_->language_generation};
    if (!request_->analysis) {
      error_code_ = "ERR_SNAPSHOT_ANALYSIS_FAILED";
      SetError("Revision has no published snapshot analysis");
      return;
    }
    auto syntax_analysis =
        std::make_shared<SnapshotAnalysis>(*request_->analysis);
    // A content checksum is only used to recognize a no-edit revision. When
    // explicit edits are present Tree-sitter already receives the edit chain,
    // so scanning the complete buffer here duplicates its input walk and
    // makes latest-revision latency scale with document size.
    if (configuration.enabled() &&
        reader.size() <= state_->maximum_syntax_utf16_length &&
        request_->syntax_edits.empty() &&
        !syntax_analysis->checksum_complete) {
      uint64_t checksum = 0;
      if (!reader.content_checksum(checksum, syntax_checksum_cancelled,
                                   &cancellation)) {
        if (cancellation.requested()) {
          syntax_result_.attempted = configuration.enabled();
          syntax_result_.cancelled = true;
          return;
        }
        error_code_ = "ERR_SNAPSHOT_ANALYSIS_FAILED";
        SetError("Unable to checksum the syntax snapshot");
        return;
      }
      syntax_analysis->checksum = checksum;
      syntax_analysis->checksum_complete = true;
    }
    syntax_analysis_ = syntax_analysis;

    std::vector<SyntaxEdit> syntax_edits;
    syntax_edits.reserve(request_->syntax_edits.size() / 8);
    for (size_t index = 0; index < request_->syntax_edits.size(); index += 8) {
      syntax_edits.push_back(SyntaxEdit{
          Point{request_->syntax_edits[index],
                request_->syntax_edits[index + 1]},
          Point{request_->syntax_edits[index + 2],
                request_->syntax_edits[index + 3]},
          Point{request_->syntax_edits[index + 4],
                request_->syntax_edits[index + 5]},
          Point{request_->syntax_edits[index + 6],
                request_->syntax_edits[index + 7]}});
    }
    if (!state_->syntax_backend->parse(
            reader, syntax_analysis_, configuration, syntax_edits,
            request_->revision,
            state_->maximum_syntax_utf16_length, cancellation,
            syntax_result_)) {
      syntax_disabled_reason_ =
          syntax_fail_open_reason(syntax_result_.error_code);
      if (!syntax_disabled_reason_.empty()) {
        // Grammar/query failures and oversized inputs disable syntax for this
        // revision, but they are not text publication failures. Drop any old
        // tree so stale scopes can never survive the fail-open transition.
        state_->syntax_backend->reset();
        return;
      }
      error_code_ = syntax_result_.error_code;
      SetError(syntax_result_.error_message);
    }
  }

  void OnWorkComplete(Napi::Env env, napi_status status) override {
    if (status == napi_cancelled ||
        state_->environment_cleanup_signal.load(std::memory_order_acquire)) {
      std::unique_ptr<RevisionRequest> pending;
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->active = false;
        pending = std::move(state_->pending);
      }
      job_->release_lease_on_owner();
      request_.reset();
      pending.reset();
      if (state_->addon_data != nullptr)
        state_->addon_data->notify_cleanup_progress();
      Napi::AsyncWorker::OnWorkComplete(env, napi_cancelled);
      return;
    }
    Napi::AsyncWorker::OnWorkComplete(env, status);
  }

  void OnOK() override {
    Napi::Env env = Env();
    bool accepted = false;
    bool destroyed = false;
    const SuperstringSnapshotLease *old_syntax_lease = nullptr;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      destroyed = state_->destroyed;
      accepted = !destroyed && !syntax_result_.cancelled &&
                 request_->revision == state_->latest_requested_revision &&
                 request_->revision == state_->buffer_revision &&
                 request_->language_generation == state_->language_generation;
      if (syntax_result_.attempted)
        state_->counters.syntax_parses++;
      if (syntax_result_.incremental && syntax_result_.parsed)
        state_->counters.syntax_incremental_parses++;
      if (syntax_result_.cancelled)
        state_->counters.syntax_cancelled_parses++;
      if (!syntax_disabled_reason_.empty())
        state_->counters.syntax_fail_open++;
      if (syntax_disabled_reason_ == "input-too-large")
        state_->counters.syntax_input_too_large++;
      if (syntax_result_.wasm_bytes > 0) {
        if (syntax_result_.grammar_cache_hit)
          state_->counters.grammar_cache_hits++;
        else
          state_->counters.grammar_cache_misses++;
      }
      state_->counters.query_files_loaded +=
          syntax_result_.query_statistics.files_loaded;
      state_->counters.query_programs_compiled +=
          syntax_result_.query_statistics.programs_compiled;
      state_->counters.query_programs_executed +=
          syntax_result_.query_statistics.programs_executed;
      state_->counters.query_captures +=
          syntax_result_.query_statistics.captures;
      state_->counters.query_text_predicates +=
          syntax_result_.query_statistics.text_predicates;
      state_->counters.query_custom_predicates +=
          syntax_result_.query_statistics.custom_predicates;
      state_->counters.query_unresolved_predicates +=
          syntax_result_.query_statistics.unresolved_predicates;
      state_->counters.query_unresolved_regex_predicates +=
          syntax_result_.query_statistics.unresolved_regex_predicates;
      state_->counters.query_scope_predicates +=
          syntax_result_.query_statistics.scope_predicates;
      state_->counters.query_cache_hits +=
          syntax_result_.query_statistics.cache_hits;
      state_->counters.query_cache_misses +=
          syntax_result_.query_statistics.cache_misses;
      state_->counters.query_cache_evictions +=
          syntax_result_.query_statistics.cache_evictions;
      if (accepted) {
        old_syntax_lease = state_->syntax_lease;
        if (syntax_result_.parsed) {
          state_->syntax_lease = job_->take_lease();
          state_->syntax_analysis = syntax_analysis_;
          state_->syntax_revision = request_->revision;
          state_->syntax_checksum = syntax_result_.checksum;
          state_->syntax_node_count = syntax_result_.node_count;
          if (syntax_result_.wasm_bytes > 0) {
            state_->syntax_wasm_bytes = syntax_result_.wasm_bytes;
            state_->syntax_grammar_load_milliseconds =
                syntax_result_.grammar_load_milliseconds;
            state_->syntax_last_grammar_cache_hit =
                syntax_result_.grammar_cache_hit;
          }
          state_->syntax_root_has_error = syntax_result_.root_has_error;
          state_->syntax_last_incremental = syntax_result_.incremental;
          state_->syntax_parse_milliseconds = syntax_result_.parse_milliseconds;
          state_->syntax_root_type = syntax_result_.root_type;
          state_->syntax_resolved_language_name =
              syntax_result_.resolved_language_name;
          state_->syntax_grammar_fingerprint =
              syntax_result_.grammar_fingerprint;
          state_->query_snapshot = syntax_result_.query_snapshot;
          state_->published_syntax = syntax_result_.published_snapshot;
          state_->query_snapshot_cache->clear();
          state_->injection_engine->publish_root(
              state_->published_syntax, state_->query_snapshot,
              *state_->syntax_analysis, state_->language_id,
              state_->syntax_lease);
          state_->query_compile_milliseconds =
              syntax_result_.query_statistics.compile_milliseconds;
          state_->query_execute_milliseconds =
              syntax_result_.query_statistics.execute_milliseconds;
          state_->query_pattern_count =
              syntax_result_.query_statistics.patterns;
          state_->query_capture_count =
              syntax_result_.query_statistics.captures;
          state_->query_language_segment_substitutions =
              syntax_result_.query_statistics.language_segment_substitutions;
          state_->query_language_segment_warnings =
              syntax_result_.query_statistics.language_segment_warnings;
          state_->query_last_error.clear();
          state_->query_last_error_code.clear();
          state_->query_last_error_type.clear();
          state_->query_last_error_path.clear();
          state_->query_last_error_line = 0;
          state_->query_last_error_column = 0;
          state_->syntax_last_error.clear();
          state_->syntax_last_error_code.clear();
          state_->syntax_disabled_reason.clear();
        } else {
          state_->syntax_lease = nullptr;
          state_->syntax_analysis.reset();
          state_->syntax_revision = 0;
          state_->syntax_checksum = 0;
          state_->syntax_node_count = 0;
          state_->syntax_wasm_bytes = 0;
          state_->syntax_root_has_error = false;
          state_->syntax_last_incremental = false;
          state_->syntax_last_grammar_cache_hit = false;
          state_->syntax_grammar_load_milliseconds = 0;
          state_->syntax_parse_milliseconds = 0;
          state_->syntax_root_type.clear();
          state_->syntax_resolved_language_name.clear();
          state_->syntax_grammar_fingerprint.clear();
          state_->published_syntax.reset();
          clear_query_state(*state_);
          if (!syntax_result_.query_error.code.empty()) {
            state_->query_last_error = syntax_result_.query_error.message;
            state_->query_last_error_code = syntax_result_.query_error.code;
            state_->query_last_error_type =
                syntax_result_.query_error.query_type;
            state_->query_last_error_path =
                syntax_result_.query_error.file_path;
            state_->query_last_error_line = syntax_result_.query_error.line;
            state_->query_last_error_column =
                syntax_result_.query_error.column;
          }
          clear_display_highlights(*state_);
          state_->injection_engine->clear();
          state_->syntax_disabled_reason = syntax_disabled_reason_;
          if (!syntax_disabled_reason_.empty()) {
            state_->syntax_last_error = syntax_result_.error_message;
            state_->syntax_last_error_code = syntax_result_.error_code;
          }
        }
        state_->edits_since_syntax.clear();
      } else {
        state_->counters.revisions_stale++;
      }
    }
    release_lease(old_syntax_lease);
    job_->release_lease_on_owner();

    if (destroyed) {
      request_->deferred.Reject(
          coded_error(env, "Document session was destroyed",
                      "ERR_DOCUMENT_SESSION_DESTROYED")
              .Value());
    } else {
      Napi::Object result = Napi::Object::New(env);
      result.Set("accepted", Napi::Boolean::New(env, accepted));
      result.Set("bufferRevision",
                 Napi::Number::New(env,
                                   static_cast<double>(request_->revision)));
      result.Set(
          "syntaxRevision",
          Napi::Number::New(env, static_cast<double>(state_->syntax_revision)));
      result.Set("checksum",
                 Napi::String::New(
                     env, checksum_string(syntax_analysis_
                                              ? syntax_analysis_->checksum
                                              : request_->analysis->checksum)));
      result.Set("syntaxParsed", Napi::Boolean::New(env, syntax_result_.parsed));
      result.Set("syntaxDisabledReason",
                 Napi::String::New(env, syntax_disabled_reason_));
      result.Set("syntaxErrorCode",
                 Napi::String::New(env, syntax_result_.error_code));
      result.Set("syntaxIncremental",
                 Napi::Boolean::New(env, syntax_result_.incremental));
      result.Set("syntaxChecksum",
                 Napi::String::New(
                     env, syntax_result_.parsed
                              ? checksum_string(syntax_result_.checksum)
                              : std::string()));
      result.Set("syntaxRootType",
                 Napi::String::New(env, syntax_result_.root_type));
      result.Set("resolvedLanguageName",
                 Napi::String::New(
                     env, syntax_result_.resolved_language_name));
      result.Set("syntaxRootHasError",
                 Napi::Boolean::New(env, syntax_result_.root_has_error));
      result.Set("syntaxNodeCount",
                 Napi::Number::New(
                     env, static_cast<double>(syntax_result_.node_count)));
      result.Set("grammarLoadMilliseconds",
                 Napi::Number::New(
                     env, syntax_result_.grammar_load_milliseconds));
      result.Set("grammarCacheHit",
                 Napi::Boolean::New(env,
                                    syntax_result_.grammar_cache_hit));
      result.Set("parseMilliseconds",
                 Napi::Number::New(env, syntax_result_.parse_milliseconds));
      result.Set("queryCaptureCount",
                 Napi::Number::New(
                     env, static_cast<double>(
                              syntax_result_.query_statistics.captures)));
      result.Set("queryCompileMilliseconds",
                 Napi::Number::New(
                     env,
                     syntax_result_.query_statistics.compile_milliseconds));
      result.Set("queryExecuteMilliseconds",
                 Napi::Number::New(
                     env,
                     syntax_result_.query_statistics.execute_milliseconds));
      request_->deferred.Resolve(result);
    }

    std::unique_ptr<RevisionRequest> next;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->active = false;
      if (!state_->destroyed)
        next = std::move(state_->pending);
      else if (state_->pending)
        next = std::move(state_->pending);
    }
    if (state_->destroyed && next) {
      reject_request(env, std::move(next), "Document session was destroyed",
                     "ERR_DOCUMENT_SESSION_DESTROYED");
    } else if (next) {
      queue_request(env, state_, std::move(next));
    }
    resolve_drains_if_idle(env, state_);
  }

  void OnError(const Napi::Error &error) override {
    Napi::Env env = Env();
    const char *code = error_code_.empty() ? "ERR_SNAPSHOT_ANALYSIS_FAILED"
                                           : error_code_.c_str();
    Napi::Error output = coded_error(env, error.Message().c_str(), code);
    request_->deferred.Reject(output.Value());
    job_->release_lease_on_owner();

    std::unique_ptr<RevisionRequest> next;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->active = false;
      if (syntax_result_.attempted) {
        state_->counters.syntax_parses++;
        if (syntax_result_.wasm_bytes > 0) {
          if (syntax_result_.grammar_cache_hit)
            state_->counters.grammar_cache_hits++;
          else
            state_->counters.grammar_cache_misses++;
        }
        state_->syntax_last_error = error.Message();
        state_->syntax_last_error_code = code;
        state_->counters.query_files_loaded +=
            syntax_result_.query_statistics.files_loaded;
        state_->counters.query_programs_compiled +=
            syntax_result_.query_statistics.programs_compiled;
        state_->counters.query_programs_executed +=
            syntax_result_.query_statistics.programs_executed;
        state_->counters.query_captures +=
            syntax_result_.query_statistics.captures;
        state_->counters.query_text_predicates +=
            syntax_result_.query_statistics.text_predicates;
        state_->counters.query_custom_predicates +=
            syntax_result_.query_statistics.custom_predicates;
        state_->counters.query_unresolved_predicates +=
            syntax_result_.query_statistics.unresolved_predicates;
        state_->counters.query_unresolved_regex_predicates +=
            syntax_result_.query_statistics.unresolved_regex_predicates;
        state_->counters.query_scope_predicates +=
            syntax_result_.query_statistics.scope_predicates;
        state_->counters.query_cache_hits +=
            syntax_result_.query_statistics.cache_hits;
        state_->counters.query_cache_misses +=
            syntax_result_.query_statistics.cache_misses;
        state_->counters.query_cache_evictions +=
            syntax_result_.query_statistics.cache_evictions;
        state_->query_last_error = syntax_result_.query_error.message;
        state_->query_last_error_code = syntax_result_.query_error.code;
        state_->query_last_error_type =
            syntax_result_.query_error.query_type;
        state_->query_last_error_path =
            syntax_result_.query_error.file_path;
        state_->query_last_error_line = syntax_result_.query_error.line;
        state_->query_last_error_column = syntax_result_.query_error.column;
      }
      if (state_->pending)
        next = std::move(state_->pending);
    }
    if (state_->destroyed && next) {
      reject_request(env, std::move(next), "Document session was destroyed",
                     "ERR_DOCUMENT_SESSION_DESTROYED");
    } else if (next) {
      queue_request(env, state_, std::move(next));
    }
    resolve_drains_if_idle(env, state_);
  }

private:
  std::shared_ptr<SessionState> state_;
  std::unique_ptr<RevisionRequest> request_;
  std::shared_ptr<NativeJobControl> job_;
  std::shared_ptr<const SnapshotAnalysis> syntax_analysis_;
  SyntaxParseResult syntax_result_;
  std::string error_code_;
  std::string syntax_disabled_reason_;
};

void queue_request(Napi::Env env, const std::shared_ptr<SessionState> &state,
                   std::unique_ptr<RevisionRequest> request) {
  auto job = std::make_shared<NativeJobControl>(request->lease);
  request->lease = nullptr;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->active = true;
    register_native_job_locked(*state, job);
  }
  (new RevisionWorker(env, state, std::move(request), std::move(job)))->Queue();
}

bool parse_fold_resets(Napi::Env env, Napi::Value value, uint64_t revision,
                       const std::shared_ptr<SessionState> &expected_session,
                       std::vector<PendingFoldReset> *result) {
  if (value.IsUndefined() || value.IsNull())
    return true;
  if (!value.IsArray()) {
    throw_type_error(env, "foldUpdates must be an array",
                     "ERR_INVALID_FOLD_UPDATES");
    return false;
  }

  Napi::Array updates = value.As<Napi::Array>();
  result->reserve(updates.Length());
  for (uint32_t index = 0; index < updates.Length(); index++) {
    Napi::Value item_value = updates.Get(index);
    if (!item_value.IsObject()) {
      throw_type_error(env, "Each fold update must be an object",
                       "ERR_INVALID_FOLD_UPDATES");
      return false;
    }
    Napi::Object item = item_value.As<Napi::Object>();
    Napi::Value view_value = item.Get("view");
    if (!view_value.IsObject()) {
      throw_type_error(env, "Fold update view must be a DisplayView",
                       "ERR_INVALID_FOLD_UPDATES");
      return false;
    }
    DisplayViewWrapper *view =
        Napi::ObjectWrap<DisplayViewWrapper>::Unwrap(view_value.As<Napi::Object>());
    if (view == nullptr || !view->state()) {
      throw_type_error(env, "Fold update view must be a DisplayView",
                       "ERR_INVALID_FOLD_UPDATES");
      return false;
    }

    uint64_t from_generation = 0;
    uint64_t to_generation = 0;
    if (!read_uint64(item.Get("fromGeneration"), &from_generation) ||
        !read_uint64(item.Get("toGeneration"), &to_generation)) {
      throw_type_error(env, "Fold generations must be non-negative integers",
                       "ERR_INVALID_FOLD_UPDATES");
      return false;
    }
    Napi::Value ranges_value = item.Get("ranges");
    if (!is_uint32_array(ranges_value)) {
      throw_type_error(env, "Fold reset ranges must be a Uint32Array",
                       "ERR_INVALID_FOLD_UPDATES");
      return false;
    }
    Napi::Uint32Array ranges = ranges_value.As<Napi::Uint32Array>();
    if (ranges.ElementLength() % 5 != 0) {
      throw_type_error(env, "Fold reset ranges must use stride 5",
                       "ERR_INVALID_FOLD_UPDATES");
      return false;
    }
    PendingFoldReset reset;
    reset.view = view->state();
    if (reset.view->session.get() != expected_session.get()) {
      throw_type_error(env, "Fold update belongs to another session",
                       "ERR_INVALID_FOLD_UPDATES");
      return false;
    }
    if (reset.view->destroyed ||
        reset.view->fold_generation != from_generation ||
        to_generation <= from_generation) {
      throw_error(env, "Fold update generation does not match the DisplayView",
                  "ERR_FOLD_GENERATION_MISMATCH");
      return false;
    }
    reset.from_generation = from_generation;
    reset.to_generation = to_generation;
    reset.ranges.assign(ranges.Data(), ranges.Data() + ranges.ElementLength());
    result->push_back(std::move(reset));
  }
  (void)revision;
  return true;
}

} // namespace

void finish_injection_job(Napi::Env env,
                          const std::shared_ptr<SessionState> &state) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active_injection_jobs > 0)
      state->active_injection_jobs--;
  }
  resolve_drains_if_idle(env, state);
}

void finish_injection_job_without_js(
    const std::shared_ptr<SessionState> &state) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active_injection_jobs > 0)
      state->active_injection_jobs--;
  }
  if (state->addon_data != nullptr)
    state->addon_data->notify_cleanup_progress();
}

void finish_highlight_job(Napi::Env env,
                          const std::shared_ptr<SessionState> &state) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active_highlight_jobs > 0)
      state->active_highlight_jobs--;
  }
  resolve_drains_if_idle(env, state);
}

void resolve_session_drains_if_idle(
    Napi::Env env, const std::shared_ptr<SessionState> &state) {
  resolve_drains_if_idle(env, state);
}

NativeJobControl::NativeJobControl(
    const SuperstringSnapshotLease *lease)
    : owner_thread_(std::this_thread::get_id()), lease_(lease) {}

NativeJobControl::~NativeJobControl() { release_lease_on_owner(); }

const SuperstringSnapshotLease *NativeJobControl::take_lease() {
  if (std::this_thread::get_id() != owner_thread_)
    return nullptr;
  const SuperstringSnapshotLease *lease = lease_;
  lease_ = nullptr;
  return lease;
}

void NativeJobControl::release_lease_on_owner() {
  release_lease(take_lease());
}

void NativeJobControl::mark_worker_finished() {
  worker_finished_.store(true, std::memory_order_release);
}

bool NativeJobControl::worker_finished() const {
  return worker_finished_.load(std::memory_order_acquire);
}

void register_native_job_locked(
    SessionState &state, const std::shared_ptr<NativeJobControl> &job) {
  for (auto iterator = state.native_jobs.begin();
       iterator != state.native_jobs.end();) {
    if ((*iterator)->worker_finished()) {
      (*iterator)->release_lease_on_owner();
      iterator = state.native_jobs.erase(iterator);
    } else {
      iterator++;
    }
  }
  state.native_jobs.push_back(job);
}

void begin_environment_cleanup(
    const std::shared_ptr<SessionState> &state) {
  state->environment_cleanup_signal.store(true, std::memory_order_release);
  state->destroyed_signal.store(true, std::memory_order_release);
  std::unique_ptr<RevisionRequest> pending;
  const SuperstringSnapshotLease *current = nullptr;
  const SuperstringSnapshotLease *syntax = nullptr;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->destroyed = true;
    pending = std::move(state->pending);
    current = state->current_lease;
    state->current_lease = nullptr;
    syntax = state->syntax_lease;
    state->syntax_lease = nullptr;
    state->analysis.reset();
    state->syntax_analysis.reset();
    state->published_syntax.reset();
    state->highlight_cancellation_signal.fetch_add(
        1, std::memory_order_relaxed);
    state->highlight_index->invalidate();
    state->active_highlight_request.reset();
    state->pending_highlight_request.reset();
    state->staged_highlight_batches.clear();
    state->transient_node_lease_ids.clear();
    state->drain_waiters.clear();
    state->injection_engine->clear();
  }
  pending.reset();
  release_lease(current);
  release_lease(syntax);
}

bool environment_cleanup_complete(
    const std::shared_ptr<SessionState> &state) {
  std::vector<std::shared_ptr<NativeJobControl>> jobs;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->pending)
      return false;
    jobs = state->native_jobs;
  }
  for (const std::shared_ptr<NativeJobControl> &job : jobs) {
    if (!job->worker_finished())
      return false;
  }
  for (const std::shared_ptr<NativeJobControl> &job : jobs)
    job->release_lease_on_owner();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->native_jobs.clear();
    state->active = false;
    state->active_injection_jobs = 0;
    state->active_highlight_jobs = 0;
  }
  return true;
}

SessionState::SessionState(AddonData *data)
    : addon_data(data), syntax_backend(std::make_unique<SyntaxBackend>()),
      injection_engine(std::make_unique<InjectionEngine>()),
      query_snapshot_cache(std::make_unique<QuerySnapshotCache>()),
      highlight_index(std::make_unique<NativeHighlightIndex>()) {}

SessionState::~SessionState() {
  if (addon_data != nullptr) {
    for (uint64_t id : transient_node_lease_ids)
      addon_data->release_node_lease(id);
  }
  release_lease(current_lease);
  release_lease(syntax_lease);
}

RevisionRequest::RevisionRequest(Napi::Env env,
                                 const SuperstringSnapshotLease *lease_value,
                                 uint64_t revision_value)
    : deferred(Napi::Promise::Deferred::New(env)), lease(lease_value),
      revision(revision_value) {}

RevisionRequest::~RevisionRequest() { release_lease(lease); }

Napi::Function DocumentSessionWrapper::init(Napi::Env env) {
  return DefineClass(
      env, "DocumentSession",
      {InstanceMethod<&DocumentSessionWrapper::apply_revision>("applyRevision"),
       InstanceMethod<&DocumentSessionWrapper::create_display_view>(
           "createDisplayView"),
       InstanceMethod<&DocumentSessionWrapper::set_language>("setLanguage"),
       InstanceMethod<&DocumentSessionWrapper::configure_syntax>(
           "configureSyntax"),
       InstanceMethod<&DocumentSessionWrapper::get_query_captures>(
           "getQueryCaptures"),
       InstanceMethod<&DocumentSessionWrapper::get_query_requirements>(
           "getQueryRequirements"),
       InstanceMethod<&DocumentSessionWrapper::request_highlight_coverage>(
           "requestHighlightCoverage"),
       InstanceMethod<&DocumentSessionWrapper::commit_highlight_coverage>(
           "commitHighlightCoverage"),
       InstanceMethod<&DocumentSessionWrapper::abort_highlight_coverage>(
           "abortHighlightCoverage"),
       InstanceMethod<&DocumentSessionWrapper::invalidate_highlight_index>(
           "invalidateHighlightIndex"),
       InstanceMethod<&DocumentSessionWrapper::use_synchronous_highlights>(
           "useSynchronousHighlights"),
       InstanceMethod<&DocumentSessionWrapper::get_injection_candidates>(
           "getInjectionCandidates"),
       InstanceMethod<&DocumentSessionWrapper::resolve_injection_node>(
           "resolveInjectionNode"),
       InstanceMethod<&DocumentSessionWrapper::resolve_query_node>(
           "resolveQueryNode"),
       InstanceMethod<&DocumentSessionWrapper::get_syntax_node_at_position>(
           "getSyntaxNodeAtPosition"),
       InstanceMethod<
           &DocumentSessionWrapper::get_syntax_node_containing_range>(
           "getSyntaxNodeContainingRange"),
       InstanceMethod<&DocumentSessionWrapper::apply_injection_result_batch>(
           "applyInjectionResultBatch"),
       InstanceMethod<&DocumentSessionWrapper::apply_injection_results>(
           "applyInjectionResults"),
       InstanceMethod<
           &DocumentSessionWrapper::apply_injection_language_scopes>(
           "applyInjectionLanguageScopes"),
       InstanceMethod<&DocumentSessionWrapper::apply_query_language_descriptors>(
           "applyQueryLanguageDescriptors"),
       InstanceMethod<&DocumentSessionWrapper::abort_injection_request>(
           "abortInjectionRequest"),
        InstanceMethod<&DocumentSessionWrapper::get_diagnostics>(
            "getDiagnostics"),
        InstanceMethod<&DocumentSessionWrapper::track_snapshot_leases>(
            "_trackSnapshotLeases"),
        InstanceMethod<&DocumentSessionWrapper::release_snapshot_leases>(
            "_releaseSnapshotLeases"),
        InstanceMethod<&DocumentSessionWrapper::drain>("drain"),
       InstanceMethod<&DocumentSessionWrapper::destroy>("destroy")});
}

DocumentSessionWrapper::DocumentSessionWrapper(const Napi::CallbackInfo &info)
    : Napi::ObjectWrap<DocumentSessionWrapper>(info),
      state_(std::make_shared<SessionState>(
          info.Env().GetInstanceData<AddonData>())) {
  state_->owner = this;
  state_->addon_data->register_session(state_);
  if (info.Length() > 0 && !info[0].IsUndefined()) {
    if (!info[0].IsObject()) {
      throw_type_error(info.Env(), "DocumentSession options must be an object",
                       "ERR_INVALID_SESSION_OPTIONS");
      return;
    }
    Napi::Object options = info[0].As<Napi::Object>();
    Napi::Value delay_value = options.Get("workerDelayMs");
    if (!delay_value.IsUndefined()) {
      uint64_t delay = 0;
      if (!read_uint64(delay_value, &delay) || delay > 60000) {
        throw_type_error(info.Env(),
                         "workerDelayMs must be an integer from 0 to 60000",
                         "ERR_INVALID_SESSION_OPTIONS");
        return;
      }
      state_->worker_delay_ms = static_cast<uint32_t>(delay);
    }
    Napi::Value syntax_limit_value = options.Get("maxSyntaxUtf16Length");
    if (!syntax_limit_value.IsUndefined()) {
      uint64_t syntax_limit = 0;
      if (!read_uint64(syntax_limit_value, &syntax_limit) ||
          syntax_limit == 0 || syntax_limit > MAX_SYNTAX_UTF16_LENGTH) {
        throw_type_error(
            info.Env(),
            "maxSyntaxUtf16Length must be an integer from 1 to 2147483647",
            "ERR_INVALID_SESSION_OPTIONS");
        return;
      }
      state_->maximum_syntax_utf16_length = syntax_limit;
    }
    Napi::Value disable_line_index = options.Get("disableSnapshotLineIndex");
    if (!disable_line_index.IsUndefined()) {
      if (!disable_line_index.IsBoolean()) {
        throw_type_error(info.Env(),
                         "disableSnapshotLineIndex must be a boolean",
                         "ERR_INVALID_SESSION_OPTIONS");
        return;
      }
      state_->use_snapshot_line_index =
          !disable_line_index.As<Napi::Boolean>().Value();
    }
  }
}

DocumentSessionWrapper::~DocumentSessionWrapper() {
  if (!state_)
    return;
  std::unique_ptr<RevisionRequest> pending;
  const SuperstringSnapshotLease *current = nullptr;
  const SuperstringSnapshotLease *syntax = nullptr;
  std::vector<uint64_t> transient_node_lease_ids;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->destroyed = true;
    state_->destroyed_signal.store(true, std::memory_order_relaxed);
    state_->highlight_cancellation_signal.fetch_add(
        1, std::memory_order_relaxed);
    state_->owner = nullptr;
    pending = std::move(state_->pending);
    current = state_->current_lease;
    state_->current_lease = nullptr;
    syntax = state_->syntax_lease;
    state_->syntax_lease = nullptr;
    state_->analysis.reset();
    state_->syntax_analysis.reset();
    state_->published_syntax.reset();
    state_->highlight_index->invalidate();
    state_->pending_highlight_request.reset();
    state_->staged_highlight_batches.clear();
    transient_node_lease_ids.assign(state_->transient_node_lease_ids.begin(),
                                    state_->transient_node_lease_ids.end());
    state_->transient_node_lease_ids.clear();
    state_->injection_engine->clear();
  }
  if (state_->addon_data != nullptr) {
    for (uint64_t id : transient_node_lease_ids)
      state_->addon_data->release_node_lease(id);
  }
  release_lease(current);
  release_lease(syntax);
}

Napi::Value DocumentSessionWrapper::apply_revision(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[0].IsObject())
    return throw_type_error(env,
                            "applyRevision expects snapshot, edits and revision",
                            "ERR_INVALID_REVISION");

  if (!info[1].IsUndefined() && !info[1].IsNull()) {
    if (!is_uint32_array(info[1]) ||
        info[1].As<Napi::Uint32Array>().ElementLength() % 8 != 0) {
      return throw_type_error(env, "edits must be a Uint32Array with stride 8",
                              "ERR_INVALID_EDITS");
    }
  }

  std::vector<uint32_t> packed_edits;
  if (!info[1].IsUndefined() && !info[1].IsNull()) {
    Napi::Uint32Array edits = info[1].As<Napi::Uint32Array>();
    packed_edits.assign(edits.Data(), edits.Data() + edits.ElementLength());
  }

  uint64_t revision = 0;
  if (!read_uint64(info[2], &revision))
    return throw_type_error(env, "bufferRevision must be a non-negative integer",
                            "ERR_INVALID_REVISION");

  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->destroyed)
      return throw_error(env, "Document session was destroyed",
                         "ERR_DOCUMENT_SESSION_DESTROYED");
    if (revision <= state_->latest_requested_revision)
      return throw_error(env, "bufferRevision must increase monotonically",
                         "ERR_NON_MONOTONIC_REVISION");
  }

  const SuperstringSnapshotLease *lease = nullptr;
  const SuperstringSnapshotLeaseStatus acquire_status =
      superstring_snapshot_lease_acquire(env, info[0], &lease);
  if (acquire_status != SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK) {
    if (env.IsExceptionPending())
      return env.Undefined();
    if (acquire_status == SUPERSTRING_SNAPSHOT_LEASE_STATUS_TYPE_MISMATCH)
      return throw_type_error(env, "snapshot is not a native Superstring snapshot",
                              "ERR_SNAPSHOT_TYPE_MISMATCH");
    if (acquire_status == SUPERSTRING_SNAPSHOT_LEASE_STATUS_ABI_MISMATCH)
      return throw_error(env, "Superstring SnapshotLease ABI is incompatible",
                         "ERR_SNAPSHOT_ABI_MISMATCH");
    return throw_error(env, "Unable to acquire the Superstring snapshot lease",
                       "ERR_SNAPSHOT_LEASE");
  }

  auto request = std::make_unique<RevisionRequest>(env, lease, revision);
  request->edits = std::move(packed_edits);
  Napi::Promise promise = request->deferred.Promise();
  if (info.Length() > 3 &&
      !parse_fold_resets(env, info[3], revision, state_,
                         &request->fold_resets)) {
    return env.Undefined();
  }

  SnapshotReader reader(lease);
  if (!reader.valid())
    return throw_error(env, reader.error().c_str(),
                       "ERR_SNAPSHOT_ANALYSIS_FAILED");
  std::shared_ptr<const SnapshotAnalysis> previous_analysis;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    previous_analysis = state_->analysis;
  }
  auto analysis = std::make_shared<SnapshotAnalysis>();
  const auto analysis_started_at = std::chrono::steady_clock::now();
  bool analyzed = false;
  bool incremental_analysis = false;
  bool line_index_analysis = false;
  if (previous_analysis != nullptr && request->edits.empty() &&
      previous_analysis->utf16_length == reader.size()) {
    *analysis = *previous_analysis;
    analyzed = true;
    incremental_analysis = true;
  } else if (previous_analysis != nullptr && request->edits.size() == 8) {
    const Point old_start{request->edits[0], request->edits[1]};
    const Point old_end{request->edits[2], request->edits[3]};
    const Point new_start{request->edits[4], request->edits[5]};
    const Point new_end{request->edits[6], request->edits[7]};
    if (state_->use_snapshot_line_index && reader.has_line_index()) {
      analyzed = reader.analyze_line_index_incremental(
          *previous_analysis, old_start, old_end, new_start, new_end,
          *analysis, request->lines_read);
    }
    if (!analyzed) {
      analyzed = reader.analyze_incremental(
          *previous_analysis, old_start, old_end, new_start, new_end,
          *analysis, request->chunks_read);
    }
    incremental_analysis = analyzed;
  } else if (previous_analysis != nullptr && request->edits.size() > 8 &&
             state_->use_snapshot_line_index && reader.has_line_index()) {
    analyzed = reader.analyze_line_index_incremental_batch(
        *previous_analysis, request->edits, *analysis, request->lines_read);
    incremental_analysis = analyzed;
  }
  if (!analyzed && state_->use_snapshot_line_index &&
      reader.has_line_index()) {
    analyzed = reader.analyze_line_index(*analysis, request->lines_read);
    line_index_analysis = analyzed;
  }
  if (!analyzed)
    analyzed = reader.analyze(*analysis, request->chunks_read);
  if (!analyzed)
    return throw_error(env, "Unable to analyze the snapshot lease",
                       "ERR_SNAPSHOT_ANALYSIS_FAILED");
  const double analysis_milliseconds =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - analysis_started_at)
          .count();
  request->analysis = analysis;

  const SuperstringSnapshotLeaseStatus retain_status =
      lease->functions->retain(lease->context);
  if (retain_status != SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK)
    return throw_error(env, "Unable to retain the display snapshot lease",
                       "ERR_SNAPSHOT_LEASE");

  std::unique_ptr<RevisionRequest> superseded;
  bool start_now = false;
  bool complete_without_syntax = false;
  bool retain_owner = false;
  const SuperstringSnapshotLease *old_display_lease = nullptr;
  const SuperstringSnapshotLease *old_syntax_lease_without_syntax = nullptr;
  uint64_t previous_buffer_revision = 0;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    previous_buffer_revision = state_->buffer_revision;
    old_display_lease = state_->current_lease;
    state_->current_lease = lease;
    state_->analysis = analysis;
    state_->buffer_revision = revision;
    request->language_id = state_->language_id;
    request->runtime = state_->runtime;
    request->wasm_path = state_->wasm_path;
    request->language_name = state_->language_name.empty()
                                 ? language_name_for(state_->wasm_path, "")
                                 : state_->language_name;
    request->language_segment = state_->language_segment;
    request->query_paths = state_->query_paths;
    request->query_sources = state_->query_sources;
    request->language_generation = state_->language_generation;
    const bool syntax_enabled = request->runtime == "wasm" &&
                                !request->wasm_path.empty();
    complete_without_syntax = !syntax_enabled && state_->worker_delay_ms == 0;
    if (syntax_enabled && !request->edits.empty()) {
      state_->edits_since_syntax.push_back(RevisionEditBatch{
          previous_buffer_revision, revision, request->edits,
          previous_analysis, analysis});
    }
    if (syntax_enabled) {
      for (const RevisionEditBatch &batch : state_->edits_since_syntax) {
        request->syntax_edits.insert(request->syntax_edits.end(),
                                     batch.edits.begin(), batch.edits.end());
      }
    } else {
      state_->edits_since_syntax.clear();
    }
    state_->latest_requested_revision = revision;
    state_->latest_requested_signal.store(revision, std::memory_order_relaxed);
    state_->highlight_cancellation_signal.fetch_add(
        1, std::memory_order_relaxed);
    state_->highlight_index->invalidate();
    state_->staged_highlight_batches.clear();
    state_->counters.revisions_requested++;
    state_->counters.revisions_published++;
    state_->counters.snapshot_chunks_read += request->chunks_read;
    state_->counters.snapshot_lines_read += request->lines_read;
    state_->counters.synchronous_analyses++;
    if (incremental_analysis)
      state_->counters.synchronous_incremental_analyses++;
    else if (line_index_analysis)
      state_->counters.synchronous_line_index_analyses++;
    else
      state_->counters.synchronous_full_analyses++;
    state_->synchronous_analysis_milliseconds += analysis_milliseconds;

    for (auto iterator = state_->display_views.begin();
         iterator != state_->display_views.end();) {
      if (std::shared_ptr<DisplayViewState> view = iterator->lock()) {
        if (!view->destroyed) {
          display_view_accept_revision(view, revision, request->edits,
                                       previous_analysis, analysis);
        }
        iterator++;
      } else {
        iterator = state_->display_views.erase(iterator);
      }
    }
    for (PendingFoldReset &reset : request->fold_resets) {
      if (!reset.view || reset.view->destroyed)
        continue;
      reset.view->folds.clear();
      for (size_t index = 0; index < reset.ranges.size(); index += 5) {
        const uint32_t id = reset.ranges[index];
        reset.view->folds[id] = Fold{
            id,
            Range{Point{reset.ranges[index + 1], reset.ranges[index + 2]},
                  Point{reset.ranges[index + 3], reset.ranges[index + 4]}}};
      }
      reset.view->fold_generation = reset.to_generation;
      reset.view->target_buffer_revision = revision;
      reset.view->cached_buffer_revision = UINT64_MAX;
      reset.view->display_revision++;
    }
    if (complete_without_syntax) {
      superseded = std::move(state_->pending);
      if (superseded)
        state_->counters.revisions_superseded++;
      state_->syntax_revision = 0;
      state_->syntax_checksum = 0;
      state_->syntax_node_count = 0;
      state_->syntax_wasm_bytes = 0;
      state_->syntax_root_has_error = false;
      state_->syntax_last_incremental = false;
      state_->syntax_last_grammar_cache_hit = false;
      state_->syntax_grammar_load_milliseconds = 0;
      state_->syntax_parse_milliseconds = 0;
      state_->syntax_root_type.clear();
      state_->syntax_resolved_language_name.clear();
      state_->syntax_grammar_fingerprint.clear();
      state_->syntax_last_error.clear();
      state_->syntax_last_error_code.clear();
      state_->syntax_disabled_reason.clear();
      old_syntax_lease_without_syntax = state_->syntax_lease;
      state_->syntax_lease = nullptr;
      state_->syntax_analysis.reset();
      state_->published_syntax.reset();
      clear_query_state(*state_);
      clear_display_highlights(*state_);
      state_->injection_engine->clear();
    } else if (state_->active) {
      superseded = std::move(state_->pending);
      state_->pending = std::move(request);
      if (superseded)
        state_->counters.revisions_superseded++;
    } else {
      start_now = true;
      if (!state_->owner_referenced) {
        state_->owner_referenced = true;
        retain_owner = true;
      }
    }
  }
  release_lease(old_display_lease);
  release_lease(old_syntax_lease_without_syntax);

  if (superseded)
    reject_request(env, std::move(superseded),
                   "Revision was replaced by a newer pending revision",
                   "ERR_REVISION_SUPERSEDED");
  if (retain_owner)
    Ref();
  if (complete_without_syntax) {
    resolve_revision_without_syntax(env, *request);
  } else if (start_now) {
    queue_request(env, state_, std::move(request));
  }
  resolve_drains_if_idle(env, state_);
  return promise;
}

Napi::Value DocumentSessionWrapper::create_display_view(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->destroyed)
      return throw_error(env, "Document session was destroyed",
                         "ERR_DOCUMENT_SESSION_DESTROYED");
  }
  Napi::Value options = info.Length() > 0 ? info[0] : env.Undefined();
  Napi::Object instance =
      DisplayViewWrapper::new_instance(env, state_, options);
  DisplayViewWrapper *wrapper =
      Napi::ObjectWrap<DisplayViewWrapper>::Unwrap(instance);
  if (wrapper != nullptr) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->display_views.push_back(wrapper->state());
  }
  return instance;
}

Napi::Value DocumentSessionWrapper::set_language(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || info[0].IsNull()) {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->language_id.clear();
    state_->runtime.clear();
    state_->wasm_path.clear();
    state_->language_name.clear();
    state_->language_segment.clear();
    state_->query_paths.clear();
    state_->query_sources.clear();
    state_->query_file_count = 0;
    state_->syntax_revision = 0;
    release_lease(state_->syntax_lease);
    state_->syntax_lease = nullptr;
    state_->syntax_analysis.reset();
    state_->edits_since_syntax.clear();
    clear_query_state(*state_);
    clear_display_highlights(*state_);
    state_->published_syntax.reset();
    state_->injection_engine->clear();
    state_->language_generation++;
    state_->language_generation_signal.store(state_->language_generation,
                                               std::memory_order_relaxed);
    state_->syntax_checksum = 0;
    state_->syntax_node_count = 0;
    state_->syntax_root_type.clear();
    state_->syntax_resolved_language_name.clear();
    state_->syntax_grammar_fingerprint.clear();
    state_->syntax_last_error.clear();
    state_->syntax_last_error_code.clear();
      state_->syntax_disabled_reason.clear();
    }
    resolve_drains_if_idle(env, state_);
    return env.Undefined();
  }
  if (!info[0].IsObject())
    return throw_type_error(env, "Language descriptor must be an object or null",
                            "ERR_INVALID_LANGUAGE_DESCRIPTOR");

  Napi::Object descriptor = info[0].As<Napi::Object>();
  const char *required[] = {"languageId", "runtime", "wasmPath"};
  for (const char *key : required) {
    if (!descriptor.Get(key).IsString())
      return throw_type_error(env, "Language descriptor fields are invalid",
                              "ERR_INVALID_LANGUAGE_DESCRIPTOR");
  }
  uint64_t query_count = 0;
  std::map<std::string, std::vector<std::string>> parsed_query_paths;
  Napi::Value query_paths_value = descriptor.Get("queryPaths");
  if (!query_paths_value.IsUndefined()) {
    if (!query_paths_value.IsObject())
      return throw_type_error(env, "queryPaths must be an object of arrays",
                              "ERR_INVALID_LANGUAGE_DESCRIPTOR");
    Napi::Object query_paths = query_paths_value.As<Napi::Object>();
    Napi::Array keys = query_paths.GetPropertyNames();
    for (uint32_t index = 0; index < keys.Length(); index++) {
      Napi::Value key_value = keys.Get(index);
      if (!key_value.IsString())
        return throw_type_error(env, "queryPaths keys must be strings",
                                "ERR_INVALID_LANGUAGE_DESCRIPTOR");
      const std::string raw_query_type =
          key_value.As<Napi::String>().Utf8Value();
      const char *canonical = canonical_query_type(raw_query_type);
      Napi::Value paths_value = query_paths.Get(key_value);
      if (!paths_value.IsArray())
        return throw_type_error(env, "Each queryPaths value must be an array",
                                "ERR_INVALID_LANGUAGE_DESCRIPTOR");
      Napi::Array paths = paths_value.As<Napi::Array>();
      std::vector<std::string> parsed_paths;
      parsed_paths.reserve(paths.Length());
      for (uint32_t path_index = 0; path_index < paths.Length(); path_index++) {
        if (!paths.Get(path_index).IsString())
          return throw_type_error(env, "Query paths must be strings",
                                  "ERR_INVALID_LANGUAGE_DESCRIPTOR");
        parsed_paths.push_back(
            paths.Get(path_index).As<Napi::String>().Utf8Value());
      }
      query_count += paths.Length();
      if (canonical != nullptr) {
        auto &destination = parsed_query_paths[canonical];
        destination.insert(destination.end(), parsed_paths.begin(),
                           parsed_paths.end());
      }
    }
  }
  Napi::Value language_name_value = descriptor.Get("languageName");
  if (!language_name_value.IsUndefined() && !language_name_value.IsString())
    return throw_type_error(env, "languageName must be a string when present",
                            "ERR_INVALID_LANGUAGE_DESCRIPTOR");
  Napi::Value segment_value = descriptor.Get("languageSegment");
  if (!segment_value.IsUndefined() && !segment_value.IsNull() &&
      !segment_value.IsString())
    return throw_type_error(env,
                            "languageSegment must be a string when present",
                            "ERR_INVALID_LANGUAGE_DESCRIPTOR");
  const std::string wasm_path =
      descriptor.Get("wasmPath").As<Napi::String>().Utf8Value();
  const std::string language_name =
      language_name_value.IsString()
          ? language_name_value.As<Napi::String>().Utf8Value()
          : language_name_for(wasm_path, "");
  const std::string language_segment =
      segment_value.IsString()
          ? segment_value.As<Napi::String>().Utf8Value()
          : "";

  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->language_id = descriptor.Get("languageId").As<Napi::String>();
  state_->runtime = descriptor.Get("runtime").As<Napi::String>();
  state_->wasm_path = wasm_path;
  state_->language_name = language_name;
  state_->language_segment = language_segment;
  state_->query_paths = std::move(parsed_query_paths);
  state_->query_sources.clear();
  state_->query_file_count = query_count;
  state_->syntax_revision = 0;
  release_lease(state_->syntax_lease);
  state_->syntax_lease = nullptr;
  state_->syntax_analysis.reset();
  state_->edits_since_syntax.clear();
  clear_query_state(*state_);
  clear_display_highlights(*state_);
  state_->published_syntax.reset();
  state_->injection_engine->clear();
  state_->language_generation++;
  state_->language_generation_signal.store(state_->language_generation,
                                             std::memory_order_relaxed);
  state_->syntax_checksum = 0;
  state_->syntax_node_count = 0;
  state_->syntax_root_type.clear();
  state_->syntax_resolved_language_name.clear();
  state_->syntax_grammar_fingerprint.clear();
  state_->syntax_last_error.clear();
  state_->syntax_last_error_code.clear();
    state_->syntax_disabled_reason.clear();
  }
  resolve_drains_if_idle(env, state_);
  return env.Undefined();
}

Napi::Value DocumentSessionWrapper::configure_syntax(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !info[0].IsObject())
    return throw_type_error(env, "configureSyntax expects an options object",
                            "ERR_INVALID_LANGUAGE_DESCRIPTOR");
  Napi::Object options = info[0].As<Napi::Object>();
  if (!options.Get("languageId").IsString() ||
      !options.Get("wasmPath").IsString())
    return throw_type_error(env, "configureSyntax requires languageId and wasmPath",
                            "ERR_INVALID_LANGUAGE_DESCRIPTOR");
  Napi::Value language_name_value = options.Get("languageName");
  if (!language_name_value.IsUndefined() && !language_name_value.IsString())
    return throw_type_error(env, "languageName must be a string when present",
                            "ERR_INVALID_LANGUAGE_DESCRIPTOR");
  const std::string wasm_path =
      options.Get("wasmPath").As<Napi::String>().Utf8Value();
  const std::string language_name =
      language_name_value.IsString()
          ? language_name_value.As<Napi::String>().Utf8Value()
          : language_name_for(wasm_path, "");
  Napi::Value queries = options.Get("queries");
  std::map<std::string, std::string> query_sources;
  uint64_t query_file_count = 0;
  if (!queries.IsUndefined()) {
    if (!queries.IsObject())
      return throw_type_error(env, "queries must be an object of strings",
                              "ERR_INVALID_LANGUAGE_DESCRIPTOR");
    Napi::Object query_object = queries.As<Napi::Object>();
    Napi::Array keys = query_object.GetPropertyNames();
    for (uint32_t index = 0; index < keys.Length(); index++) {
      Napi::Value key = keys.Get(index);
      Napi::Value source = query_object.Get(key);
      if (!key.IsString() || !source.IsString())
        return throw_type_error(env, "queries must be an object of strings",
                                "ERR_INVALID_LANGUAGE_DESCRIPTOR");
      const std::string raw_type = key.As<Napi::String>().Utf8Value();
      if (const char *canonical = canonical_query_type(raw_type))
        query_sources[canonical] = source.As<Napi::String>().Utf8Value();
    }
    query_file_count = keys.Length();
  }
  Napi::Value segment_value = options.Get("languageSegment");
  if (!segment_value.IsUndefined() && !segment_value.IsNull() &&
      !segment_value.IsString())
    return throw_type_error(env, "languageSegment must be a string",
                            "ERR_INVALID_LANGUAGE_DESCRIPTOR");
  const std::string language_segment =
      segment_value.IsString()
          ? segment_value.As<Napi::String>().Utf8Value()
          : std::string();

  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->language_id = options.Get("languageId").As<Napi::String>();
  state_->runtime = "wasm";
  state_->wasm_path = wasm_path;
  state_->language_name = language_name;
  state_->language_segment = language_segment;
  state_->query_file_count = query_file_count;
  state_->syntax_revision = 0;
  release_lease(state_->syntax_lease);
  state_->syntax_lease = nullptr;
  state_->syntax_analysis.reset();
  state_->edits_since_syntax.clear();
  state_->query_paths.clear();
  state_->query_sources = std::move(query_sources);
  clear_query_state(*state_);
  clear_display_highlights(*state_);
  state_->published_syntax.reset();
  state_->injection_engine->clear();
  state_->language_generation++;
  state_->language_generation_signal.store(state_->language_generation,
                                             std::memory_order_relaxed);
  state_->syntax_checksum = 0;
  state_->syntax_node_count = 0;
  state_->syntax_root_type.clear();
  state_->syntax_resolved_language_name.clear();
  state_->syntax_grammar_fingerprint.clear();
  state_->syntax_last_error.clear();
  state_->syntax_last_error_code.clear();
    state_->syntax_disabled_reason.clear();
  }
  resolve_drains_if_idle(env, state_);
  return env.Undefined();
}

Napi::Value DocumentSessionWrapper::get_query_captures(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !info[0].IsString())
    return throw_type_error(env, "getQueryCaptures expects a query type",
                            "ERR_INVALID_QUERY_RANGE");
  const std::string requested_type =
      info[0].As<Napi::String>().Utf8Value();
  const char *canonical = canonical_query_type(requested_type);
  if (canonical == nullptr)
    return throw_type_error(env, "Unsupported native query type",
                            "ERR_INVALID_QUERY_TYPE");

  uint64_t start_row = 0;
  uint64_t end_row = std::numeric_limits<uint32_t>::max();
  uint64_t start_column = 0;
  uint64_t end_column = std::numeric_limits<uint32_t>::max();
  if (info.Length() > 1 && !info[1].IsUndefined() &&
      !read_uint64(info[1], &start_row))
    return throw_type_error(env, "startRow must be a non-negative integer",
                            "ERR_INVALID_QUERY_RANGE");
  if (info.Length() > 2 && !info[2].IsUndefined() &&
      !read_uint64(info[2], &end_row))
    return throw_type_error(env, "endRow must be a non-negative integer",
                            "ERR_INVALID_QUERY_RANGE");
  if (start_row > end_row ||
      end_row > std::numeric_limits<uint32_t>::max())
    return throw_type_error(
        env, "Query row range must be ordered and fit uint32",
        "ERR_INVALID_QUERY_RANGE");

  if (end_row != std::numeric_limits<uint32_t>::max())
    end_column = 0;
  QueryResolutionContext resolution;
  std::map<std::string, QueryResolutionContext> resolutions_by_grammar;
  bool include_language_scopes = false;
  bool compact_highlights = false;
  resolution.resolve_scopes = std::string_view(canonical) != "injectionsQuery";
  if (info.Length() > 3 && !info[3].IsUndefined()) {
    if (!info[3].IsObject())
      return throw_type_error(env, "query options must be an object",
                              "ERR_INVALID_QUERY_CONTEXT");
    Napi::Object options = info[3].As<Napi::Object>();
    Napi::Value start_column_value = options.Get("startColumn");
    if (!start_column_value.IsUndefined() &&
        (!read_uint64(start_column_value, &start_column) ||
         start_column > std::numeric_limits<uint32_t>::max()))
      return throw_type_error(env, "startColumn must fit uint32",
                              "ERR_INVALID_QUERY_RANGE");
    Napi::Value end_column_value = options.Get("endColumn");
    if (!end_column_value.IsUndefined() &&
        (!read_uint64(end_column_value, &end_column) ||
         end_column > std::numeric_limits<uint32_t>::max()))
      return throw_type_error(env, "endColumn must fit uint32",
                              "ERR_INVALID_QUERY_RANGE");
    Napi::Value resolve_scopes = options.Get("resolveScopes");
    if (!resolve_scopes.IsUndefined()) {
      if (!resolve_scopes.IsBoolean())
        return throw_type_error(env, "resolveScopes must be a boolean",
                                "ERR_INVALID_QUERY_CONTEXT");
      resolution.resolve_scopes = resolve_scopes.As<Napi::Boolean>().Value();
    }
    Napi::Value interpolate_names = options.Get("interpolateNames");
    if (!interpolate_names.IsUndefined()) {
      if (!interpolate_names.IsBoolean())
        return throw_type_error(env, "interpolateNames must be a boolean",
                                "ERR_INVALID_QUERY_CONTEXT");
      resolution.interpolate_names =
          interpolate_names.As<Napi::Boolean>().Value();
    }
    Napi::Value include_scopes = options.Get("includeLanguageScopes");
    if (!include_scopes.IsUndefined()) {
      if (!include_scopes.IsBoolean())
        return throw_type_error(env,
                                "includeLanguageScopes must be a boolean",
                                "ERR_INVALID_QUERY_CONTEXT");
      include_language_scopes = include_scopes.As<Napi::Boolean>().Value();
    }
    Napi::Value compact = options.Get("compactHighlights");
    if (!compact.IsUndefined()) {
      if (!compact.IsBoolean())
        return throw_type_error(env, "compactHighlights must be a boolean",
                                "ERR_INVALID_QUERY_CONTEXT");
      compact_highlights = compact.As<Napi::Boolean>().Value();
      if (compact_highlights && std::string_view(canonical) != "highlightsQuery")
        return throw_type_error(
            env, "compactHighlights is only valid for highlightsQuery",
            "ERR_INVALID_QUERY_CONTEXT");
    }
    uint64_t injection_depth = 0;
    Napi::Value depth = options.Get("injectionDepth");
    if (!depth.IsUndefined() &&
        (!read_uint64(depth, &injection_depth) ||
         injection_depth > std::numeric_limits<uint32_t>::max()))
      return throw_type_error(env, "injectionDepth must fit uint32",
                              "ERR_INVALID_QUERY_CONTEXT");
    resolution.injection_depth = static_cast<uint32_t>(injection_depth);

    Napi::Value scope_config = options.Get("scopeConfig");
    if (!scope_config.IsUndefined()) {
      if (!scope_config.IsObject() || scope_config.IsArray())
        return throw_type_error(env, "scopeConfig must be an object",
                                "ERR_INVALID_QUERY_CONTEXT");
      Napi::Object config = scope_config.As<Napi::Object>();
      Napi::Array keys = config.GetPropertyNames();
      for (uint32_t index = 0; index < keys.Length(); index++) {
        Napi::Value key = keys.Get(index);
        if (!key.IsString())
          continue;
        Napi::Value value = config.Get(key);
        const std::string name = key.As<Napi::String>().Utf8Value();
        if (value.IsNull() || value.IsUndefined()) {
          resolution.config[name] = std::monostate{};
        } else if (value.IsBoolean()) {
          resolution.config[name] = value.As<Napi::Boolean>().Value();
        } else if (value.IsNumber()) {
          resolution.config[name] = value.As<Napi::Number>().DoubleValue();
        } else if (value.IsString()) {
          resolution.config[name] = value.As<Napi::String>().Utf8Value();
        } else {
          return throw_type_error(
              env, "scopeConfig values must be null, boolean, number or string",
              "ERR_INVALID_QUERY_CONTEXT");
        }
      }
    }

    Napi::Value local_ranges = options.Get("localRanges");
    if (!local_ranges.IsUndefined()) {
      if (!is_uint32_array(local_ranges) ||
          local_ranges.As<Napi::Uint32Array>().ElementLength() % 2 != 0)
        return throw_type_error(
            env, "localRanges must be a Uint32Array with stride 2",
            "ERR_INVALID_QUERY_CONTEXT");
      const Napi::Uint32Array ranges = local_ranges.As<Napi::Uint32Array>();
      resolution.local_ranges_enabled = true;
      for (size_t index = 0; index < ranges.ElementLength(); index += 2) {
        if (ranges[index] > ranges[index + 1])
          return throw_type_error(env, "localRanges must be ordered",
                                  "ERR_INVALID_QUERY_CONTEXT");
        resolution.local_ranges.emplace_back(ranges[index], ranges[index + 1]);
      }
    }

    Napi::Value configs_by_language = options.Get("scopeConfigsByLanguage");
    if (!configs_by_language.IsUndefined()) {
      if (!configs_by_language.IsObject() || configs_by_language.IsArray())
        return throw_type_error(
            env, "scopeConfigsByLanguage must be an object",
            "ERR_INVALID_QUERY_CONTEXT");
      Napi::Object configs = configs_by_language.As<Napi::Object>();
      Napi::Array grammar_ids = configs.GetPropertyNames();
      for (uint32_t grammar_index = 0;
           grammar_index < grammar_ids.Length(); grammar_index++) {
        Napi::Value grammar_id_value = grammar_ids.Get(grammar_index);
        if (!grammar_id_value.IsString())
          continue;
        Napi::Value raw_config = configs.Get(grammar_id_value);
        if (!raw_config.IsObject() || raw_config.IsArray())
          return throw_type_error(
              env, "Each scopeConfigsByLanguage value must be an object",
              "ERR_INVALID_QUERY_CONTEXT");
        QueryResolutionContext layer_resolution = resolution;
        layer_resolution.config.clear();
        Napi::Object config = raw_config.As<Napi::Object>();
        Napi::Array keys = config.GetPropertyNames();
        for (uint32_t key_index = 0; key_index < keys.Length(); key_index++) {
          Napi::Value key = keys.Get(key_index);
          if (!key.IsString())
            continue;
          Napi::Value value = config.Get(key);
          const std::string name = key.As<Napi::String>().Utf8Value();
          if (value.IsNull() || value.IsUndefined()) {
            layer_resolution.config[name] = std::monostate{};
          } else if (value.IsBoolean()) {
            layer_resolution.config[name] =
                value.As<Napi::Boolean>().Value();
          } else if (value.IsNumber()) {
            layer_resolution.config[name] =
                value.As<Napi::Number>().DoubleValue();
          } else if (value.IsString()) {
            layer_resolution.config[name] =
                value.As<Napi::String>().Utf8Value();
          } else {
            return throw_type_error(
                env,
                "scopeConfigsByLanguage values must be null, boolean, number or string",
                "ERR_INVALID_QUERY_CONTEXT");
          }
        }
        resolutions_by_grammar.emplace(
            grammar_id_value.As<Napi::String>().Utf8Value(),
            std::move(layer_resolution));
      }
    }
  }

  if (start_row == end_row && start_column > end_column)
    return throw_type_error(env, "Query point range must be ordered",
                            "ERR_INVALID_QUERY_RANGE");

  QueryRange query_range;
  query_range.start_row = static_cast<uint32_t>(start_row);
  query_range.start_column = static_cast<uint32_t>(start_column);
  query_range.end_row = static_cast<uint32_t>(end_row);
  query_range.end_column = static_cast<uint32_t>(end_column);

  std::shared_ptr<const PublishedSyntaxSnapshot> published_syntax;
  std::shared_ptr<const SnapshotAnalysis> syntax_analysis;
  std::shared_ptr<const SnapshotAnalysis> output_analysis;
  std::vector<RevisionEditBatch> projection;
  std::vector<InjectionQuerySource> injection_sources;
  std::string root_grammar_id;
  const SuperstringSnapshotLease *lease = nullptr;
  uint64_t buffer_revision = 0;
  uint64_t syntax_revision = 0;
  uint64_t language_generation = 0;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->destroyed)
      return throw_error(env, "Document session was destroyed",
                         "ERR_DOCUMENT_SESSION_DESTROYED");
    published_syntax = state_->published_syntax;
    syntax_analysis = state_->syntax_analysis;
    output_analysis = state_->analysis;
    projection = state_->edits_since_syntax;
    lease = state_->syntax_lease;
    buffer_revision = state_->buffer_revision;
    syntax_revision = state_->syntax_revision;
    language_generation = state_->language_generation;
    root_grammar_id = state_->language_id;
    injection_sources = state_->injection_engine->query_sources(
        projection.empty() ? query_range : QueryRange{});
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("queryType", Napi::String::New(env, canonical));
  result.Set("bufferRevision",
             Napi::Number::New(env, static_cast<double>(buffer_revision)));
  result.Set("syntaxRevision",
             Napi::Number::New(env, static_cast<double>(syntax_revision)));
  result.Set("languageGeneration",
             Napi::Number::New(env,
                               static_cast<double>(language_generation)));
  result.Set("captureStride", Napi::Number::New(env, 9));
  result.Set("resolutionComplete", Napi::Boolean::New(env, true));
  result.Set("scopeCapturesTested", Napi::Number::New(env, 0));
  result.Set("scopeCapturesRejected", Napi::Number::New(env, 0));
  result.Set("scopeCapturesAdjusted", Napi::Number::New(env, 0));
  result.Set("projectedCaptureCount", Napi::Number::New(env, 0));
  result.Set("staleCaptureCount", Napi::Number::New(env, 0));
  result.Set("coveredCaptureCount", Napi::Number::New(env, 0));
  result.Set("captureNames", Napi::Array::New(env));
  result.Set("propertySets", Napi::Array::New(env));
  result.Set("layers", Napi::Array::New(env));
  result.Set("captureLayerIndices", Napi::Uint32Array::New(env, 0));
  result.Set("captureDepths", Napi::Uint32Array::New(env, 0));
  result.Set("captureOrders", Napi::Uint32Array::New(env, 0));
  result.Set("captureFlags", Napi::Uint32Array::New(env, 0));
  result.Set("captureNodeHandles", Napi::Uint32Array::New(env, 0));
  result.Set("highlightRangeStride", Napi::Number::New(env, 5));
  result.Set("highlightRanges", Napi::Uint32Array::New(env, 0));
  result.Set("highlightLayerIndices", Napi::Uint32Array::New(env, 0));
  result.Set("highlightFlags", Napi::Uint32Array::New(env, 0));
  result.Set("highlightNodeHandles", Napi::Uint32Array::New(env, 0));
  result.Set("highlightGrammarIds", Napi::Array::New(env));

  std::shared_ptr<const QueryIndexSnapshot> index_snapshot;
  if (published_syntax && syntax_analysis && output_analysis && lease) {
    SnapshotReader reader(lease);
    if (!reader.valid())
      return throw_error(env, reader.error().c_str(),
                         "ERR_SNAPSHOT_ANALYSIS_FAILED");
    QueryErrorInfo query_error;
    QueryRunStatistics statistics;
    LayeredQueryContext layered_context;
    layered_context.root_grammar_id = root_grammar_id;
    layered_context.defaults = resolution;
    layered_context.by_grammar = std::move(resolutions_by_grammar);
    layered_context.include_language_scopes = include_language_scopes;
    layered_context.include_node_handles = !compact_highlights;
    if (!execute_layered_query(
            canonical, published_syntax, injection_sources, reader,
            *syntax_analysis, *output_analysis, projection, buffer_revision,
            language_generation, query_range,
            layered_context, state_->query_snapshot_cache.get(), nullptr,
            nullptr,
            index_snapshot, query_error, statistics)) {
      const std::string message = query_error.message.empty()
                                      ? "Unable to execute native query"
                                      : query_error.message;
      return throw_error(
          env, message.c_str(),
          query_error.code.empty() ? "ERR_SYNTAX_QUERY_EXECUTION_FAILED"
                                   : query_error.code.c_str());
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->counters.query_programs_executed +=
        statistics.programs_executed;
    state_->counters.query_captures += statistics.captures;
    state_->query_execute_milliseconds += statistics.execute_milliseconds;
    state_->query_pattern_count = statistics.patterns;
    state_->query_capture_count = index_snapshot->accepted_capture_count;
    state_->counters.layered_query_executions++;
    state_->counters.layered_query_layers += index_snapshot->layers.size();
    state_->counters.query_projected_captures +=
        index_snapshot->projected_capture_count;
    state_->counters.query_stale_captures +=
        index_snapshot->stale_capture_count;
    state_->counters.query_covered_captures +=
        index_snapshot->covered_capture_count;
  }

  if (!index_snapshot) {
    result.Set("captures", Napi::Uint32Array::New(env, 0));
    result.Set("rawCaptureCount", Napi::Number::New(env, 0));
    result.Set("acceptedCaptureCount", Napi::Number::New(env, 0));
    result.Set("didExceedMatchLimit", Napi::Boolean::New(env, false));
    return result;
  }
  const QueryIndexSnapshot &index = *index_snapshot;
  result.Set("bufferRevision",
             Napi::Number::New(env,
                               static_cast<double>(index.buffer_revision)));
  result.Set("languageGeneration",
             Napi::Number::New(
                 env, static_cast<double>(index.language_generation)));

  Napi::Array capture_names = Napi::Array::New(env, index.capture_names.size());
  for (size_t name_index = 0; name_index < index.capture_names.size();
       name_index++) {
    capture_names.Set(static_cast<uint32_t>(name_index),
                      Napi::String::New(env,
                                        index.capture_names[name_index]));
  }
  result.Set("captureNames", capture_names);

  Napi::Array highlight_grammar_ids =
      Napi::Array::New(env, index.layers.size());
  for (uint32_t layer_index = 0; layer_index < index.layers.size();
       layer_index++)
    highlight_grammar_ids.Set(
        layer_index, Napi::String::New(env, index.layers[layer_index].grammar_id));
  result.Set("highlightGrammarIds", highlight_grammar_ids);

  if (!compact_highlights) {
    auto properties_to_object = [&](const std::vector<QueryProperty> &values) {
    Napi::Object output = Napi::Object::New(env);
    for (const QueryProperty &property : values) {
      output.Set(property.name,
                 property.has_value
                     ? Napi::Value(Napi::String::New(env, property.value))
                     : Napi::Value(env.Null()));
    }
    return output;
  };
  Napi::Array property_sets = Napi::Array::New(env, index.patterns.size());
  for (size_t pattern_index = 0; pattern_index < index.patterns.size();
       pattern_index++) {
    const QueryPatternMetadata &pattern = index.patterns[pattern_index];
    Napi::Object metadata = Napi::Object::New(env);
    metadata.Set("set", properties_to_object(pattern.set_properties));
    metadata.Set("asserted",
                 properties_to_object(pattern.asserted_properties));
    metadata.Set("refuted",
                 properties_to_object(pattern.refuted_properties));
    Napi::Array predicates =
        Napi::Array::New(env, pattern.custom_predicates.size());
    for (size_t predicate_index = 0;
         predicate_index < pattern.custom_predicates.size();
         predicate_index++) {
      const QueryPredicate &predicate =
          pattern.custom_predicates[predicate_index];
      Napi::Object predicate_value = Napi::Object::New(env);
      predicate_value.Set("operator",
                          Napi::String::New(env, predicate.operator_name));
      Napi::Array operands =
          Napi::Array::New(env, predicate.operands.size());
      for (size_t operand_index = 0;
           operand_index < predicate.operands.size(); operand_index++) {
        const QueryOperand &operand = predicate.operands[operand_index];
        Napi::Object operand_value = Napi::Object::New(env);
        operand_value.Set("type", Napi::String::New(
                                      env, operand.capture ? "capture"
                                                           : "string"));
        operand_value.Set("value", Napi::String::New(env, operand.value));
        operands.Set(static_cast<uint32_t>(operand_index), operand_value);
      }
      predicate_value.Set("operands", operands);
      predicates.Set(static_cast<uint32_t>(predicate_index), predicate_value);
    }
    metadata.Set("predicates", predicates);
    Napi::Array unresolved =
        Napi::Array::New(env, pattern.unresolved_predicates.size());
    for (size_t predicate_index = 0;
         predicate_index < pattern.unresolved_predicates.size();
         predicate_index++) {
      const QueryPredicate &predicate =
          pattern.unresolved_predicates[predicate_index];
      Napi::Object predicate_value = Napi::Object::New(env);
      predicate_value.Set("operator",
                          Napi::String::New(env, predicate.operator_name));
      Napi::Array operands =
          Napi::Array::New(env, predicate.operands.size());
      for (size_t operand_index = 0;
           operand_index < predicate.operands.size(); operand_index++) {
        const QueryOperand &operand = predicate.operands[operand_index];
        Napi::Object operand_value = Napi::Object::New(env);
        operand_value.Set("type", Napi::String::New(
                                      env, operand.capture ? "capture"
                                                           : "string"));
        operand_value.Set("value", Napi::String::New(env, operand.value));
        operands.Set(static_cast<uint32_t>(operand_index), operand_value);
      }
      predicate_value.Set("operands", operands);
      unresolved.Set(static_cast<uint32_t>(predicate_index), predicate_value);
    }
    metadata.Set("unresolvedPredicates", unresolved);
    metadata.Set("scopeRegexComplete",
                 Napi::Boolean::New(env, pattern.scope_regex_complete));
    property_sets.Set(static_cast<uint32_t>(pattern_index), metadata);
  }
  result.Set("propertySets", property_sets);

  Napi::Array layers = Napi::Array::New(env, index.layers.size());
  for (uint32_t layer_index = 0; layer_index < index.layers.size();
       layer_index++) {
    const QueryLayerRecord &layer = index.layers[layer_index];
    Napi::Object layer_value = Napi::Object::New(env);
    layer_value.Set("layerId", Napi::Number::New(
                                   env, static_cast<double>(layer.layer_id)));
    layer_value.Set("parentLayerId",
                    Napi::Number::New(
                        env, static_cast<double>(layer.parent_layer_id)));
    layer_value.Set("depth", Napi::Number::New(env, layer.depth));
    layer_value.Set("grammarId", Napi::String::New(env, layer.grammar_id));
    layer_value.Set("coverShallowerScopes",
                    Napi::Boolean::New(env,
                                       layer.cover_shallower_scopes));
    Napi::Uint32Array ranges =
        Napi::Uint32Array::New(env, layer.ranges.size() * 6);
    Napi::Array range_scopes = Napi::Array::New(env, layer.ranges.size());
    size_t range_offset = 0;
    for (uint32_t range_index = 0; range_index < layer.ranges.size();
         range_index++) {
      const QueryLayerRangeRecord &range = layer.ranges[range_index];
      const uint32_t values[] = {
          range.start_row, range.start_column, range.end_row,
          range.end_column, range.start_index, range.end_index};
      for (uint32_t value : values)
        ranges[range_offset++] = value;
      Napi::Array scopes = Napi::Array::New(env, range.scopes.size());
      for (uint32_t scope_index = 0; scope_index < range.scopes.size();
           scope_index++)
        scopes.Set(scope_index,
                   Napi::String::New(env, range.scopes[scope_index]));
      range_scopes.Set(range_index, scopes);
    }
    layer_value.Set("rangeStride", Napi::Number::New(env, 6));
    layer_value.Set("ranges", ranges);
    layer_value.Set("rangeScopes", range_scopes);
    layers.Set(layer_index, layer_value);
  }
    result.Set("layers", layers);
  }

  const size_t selected_count = index.captures.size();
  Napi::Uint32Array captures =
      Napi::Uint32Array::New(env, compact_highlights ? 0 : selected_count * 9);
  Napi::Uint32Array capture_layer_indices =
      Napi::Uint32Array::New(env, compact_highlights ? 0 : selected_count);
  Napi::Uint32Array capture_depths =
      Napi::Uint32Array::New(env, compact_highlights ? 0 : selected_count);
  Napi::Uint32Array capture_orders =
      Napi::Uint32Array::New(env, compact_highlights ? 0 : selected_count);
  Napi::Uint32Array capture_flags =
      Napi::Uint32Array::New(env, compact_highlights ? 0 : selected_count);
  Napi::Uint32Array capture_node_handles =
      Napi::Uint32Array::New(env, compact_highlights ? 0 : selected_count);
  Napi::Uint32Array highlight_ranges =
      Napi::Uint32Array::New(env, selected_count * 5);
  Napi::Uint32Array highlight_layer_indices =
      Napi::Uint32Array::New(env, selected_count);
  Napi::Uint32Array highlight_flags =
      Napi::Uint32Array::New(env, compact_highlights ? 0 : selected_count);
  Napi::Uint32Array highlight_node_handles =
      Napi::Uint32Array::New(env, compact_highlights ? 0 : selected_count);
  size_t output_index = 0;
  size_t capture_output_index = 0;
  size_t highlight_output_index = 0;
  for (const QueryCaptureRecord &capture : index.captures) {
    if (!compact_highlights) {
      const uint32_t values[] = {
          capture.name_id,      capture.pattern_index,
          capture.property_set_id, capture.start_row,
          capture.start_column, capture.end_row,
          capture.end_column,   capture.start_index,
          capture.end_index};
      for (uint32_t value : values)
        captures[output_index++] = value;
      capture_layer_indices[capture_output_index] = capture.layer_index;
      capture_depths[capture_output_index] = capture.depth;
      capture_orders[capture_output_index] = capture.order;
      capture_flags[capture_output_index] = capture.flags;
      capture_node_handles[capture_output_index] = capture.node_handle;
    }
    const uint32_t highlight_values[] = {
        capture.name_id, capture.start_index, capture.end_index,
        capture.order, capture.depth};
    for (uint32_t value : highlight_values)
      highlight_ranges[highlight_output_index++] = value;
    highlight_layer_indices[capture_output_index] = capture.layer_index;
    if (!compact_highlights) {
      highlight_flags[capture_output_index] = capture.flags;
      highlight_node_handles[capture_output_index] = capture.node_handle;
    }
    capture_output_index++;
  }
  result.Set("captures", captures);
  result.Set("captureLayerIndices", capture_layer_indices);
  result.Set("captureDepths", capture_depths);
  result.Set("captureOrders", capture_orders);
  result.Set("captureFlags", capture_flags);
  result.Set("captureNodeHandles", capture_node_handles);
  result.Set("highlightRangeStride", Napi::Number::New(env, 5));
  result.Set("highlightRanges", highlight_ranges);
  result.Set("highlightLayerIndices", highlight_layer_indices);
  result.Set("highlightFlags", highlight_flags);
  result.Set("highlightNodeHandles", highlight_node_handles);
  result.Set("rawCaptureCount",
             Napi::Number::New(
                 env, static_cast<double>(index.raw_capture_count)));
  result.Set("acceptedCaptureCount",
             Napi::Number::New(
                 env, static_cast<double>(index.accepted_capture_count)));
  result.Set("projectedCaptureCount",
             Napi::Number::New(
                 env, static_cast<double>(index.projected_capture_count)));
  result.Set("staleCaptureCount",
             Napi::Number::New(
                 env, static_cast<double>(index.stale_capture_count)));
  result.Set("coveredCaptureCount",
             Napi::Number::New(
                 env, static_cast<double>(index.covered_capture_count)));
  result.Set("resolutionComplete",
             Napi::Boolean::New(env, index.resolution_complete));
  result.Set("scopeCapturesTested",
             Napi::Number::New(
                 env, static_cast<double>(index.scope_statistics.tested)));
  result.Set("scopeCapturesRejected",
             Napi::Number::New(
                 env, static_cast<double>(index.scope_statistics.rejected)));
  result.Set("scopeCapturesAdjusted",
             Napi::Number::New(
                 env, static_cast<double>(index.scope_statistics.adjusted)));
  result.Set("didExceedMatchLimit",
             Napi::Boolean::New(env, index.exceeded_match_limit));
  return result;
}

Napi::Value DocumentSessionWrapper::get_query_requirements(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !info[0].IsString())
    return throw_type_error(env, "getQueryRequirements expects a query type",
                            "ERR_INVALID_QUERY_TYPE");
  const std::string requested = info[0].As<Napi::String>().Utf8Value();
  const char *canonical = canonical_query_type(requested);
  if (canonical == nullptr)
    return throw_type_error(env, "Unsupported native query type",
                            "ERR_INVALID_QUERY_TYPE");
  std::shared_ptr<const PublishedSyntaxSnapshot> syntax;
  std::string root_grammar_id;
  std::vector<InjectionQuerySource> injection_sources;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->destroyed)
      return throw_error(env, "Document session was destroyed",
                         "ERR_DOCUMENT_SESSION_DESTROYED");
    syntax = state_->published_syntax;
    root_grammar_id = state_->language_id;
    injection_sources = state_->injection_engine->query_sources(QueryRange{});
  }
  std::vector<std::string> keys;
  if (syntax && syntax->query_engine())
    keys = syntax->query_engine()->scope_config_keys(canonical);
  Napi::Object result = Napi::Object::New(env);
  result.Set("queryType", Napi::String::New(env, canonical));
  Napi::Array config_keys = Napi::Array::New(env, keys.size());
  for (uint32_t index = 0; index < keys.size(); index++)
    config_keys.Set(index, Napi::String::New(env, keys[index]));
  result.Set("scopeConfigKeys", config_keys);
  std::map<std::string, std::set<std::string>> keys_by_grammar;
  keys_by_grammar[root_grammar_id].insert(keys.begin(), keys.end());
  for (const InjectionQuerySource &source : injection_sources) {
    if (!source.syntax || !source.syntax->query_engine())
      continue;
    const std::vector<std::string> child_keys =
        source.syntax->query_engine()->scope_config_keys(canonical);
    keys_by_grammar[source.grammar_id].insert(child_keys.begin(),
                                              child_keys.end());
  }
  Napi::Object by_grammar = Napi::Object::New(env);
  for (const auto &[grammar_id, grammar_keys] : keys_by_grammar) {
    if (grammar_id.empty())
      continue;
    Napi::Array values = Napi::Array::New(env, grammar_keys.size());
    uint32_t value_index = 0;
    for (const std::string &key : grammar_keys)
      values.Set(value_index++, Napi::String::New(env, key));
    by_grammar.Set(grammar_id, values);
  }
  result.Set("scopeConfigKeysByLanguage", by_grammar);
  return result;
}

Napi::Value DocumentSessionWrapper::request_highlight_coverage(
    const Napi::CallbackInfo &info) {
  return document_engine::request_highlight_coverage(info, state_);
}

Napi::Value DocumentSessionWrapper::commit_highlight_coverage(
    const Napi::CallbackInfo &info) {
  return document_engine::commit_highlight_coverage(info, state_);
}

Napi::Value DocumentSessionWrapper::abort_highlight_coverage(
    const Napi::CallbackInfo &info) {
  return document_engine::abort_highlight_coverage(info, state_);
}

Napi::Value DocumentSessionWrapper::invalidate_highlight_index(
    const Napi::CallbackInfo &info) {
  return document_engine::invalidate_highlight_index(info, state_);
}

Napi::Value DocumentSessionWrapper::use_synchronous_highlights(
    const Napi::CallbackInfo &info) {
  return document_engine::use_synchronous_highlights(info, state_);
}

Napi::Value DocumentSessionWrapper::get_injection_candidates(
    const Napi::CallbackInfo &info) {
  return state_->injection_engine->get_candidates(info, state_);
}

Napi::Value DocumentSessionWrapper::resolve_injection_node(
    const Napi::CallbackInfo &info) {
  return state_->injection_engine->resolve_node(info, state_);
}

Napi::Value DocumentSessionWrapper::resolve_query_node(
    const Napi::CallbackInfo &info) {
  return state_->injection_engine->resolve_query_node(info, state_);
}

Napi::Value DocumentSessionWrapper::get_syntax_node_at_position(
    const Napi::CallbackInfo &info) {
  return state_->injection_engine->get_syntax_node_at_position(info, state_);
}

Napi::Value DocumentSessionWrapper::get_syntax_node_containing_range(
    const Napi::CallbackInfo &info) {
  return state_->injection_engine->get_syntax_node_containing_range(info,
                                                                    state_);
}

Napi::Value DocumentSessionWrapper::apply_injection_result_batch(
    const Napi::CallbackInfo &info) {
  return state_->injection_engine->apply_result_batch(info, state_);
}

Napi::Value DocumentSessionWrapper::apply_injection_results(
    const Napi::CallbackInfo &info) {
  return state_->injection_engine->apply_results(info, state_);
}

Napi::Value DocumentSessionWrapper::apply_injection_language_scopes(
    const Napi::CallbackInfo &info) {
  return state_->injection_engine->apply_language_scopes(info, state_);
}

Napi::Value DocumentSessionWrapper::apply_query_language_descriptors(
    const Napi::CallbackInfo &info) {
  return state_->injection_engine->apply_query_language_descriptors(info,
                                                                     state_);
}

Napi::Value DocumentSessionWrapper::abort_injection_request(
    const Napi::CallbackInfo &info) {
  return state_->injection_engine->abort_request(info, state_);
}

Napi::Value DocumentSessionWrapper::get_diagnostics(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Object result = Napi::Object::New(env);
  std::lock_guard<std::mutex> lock(state_->mutex);
  result.Set("bufferRevision", Napi::Number::New(env, state_->buffer_revision));
  result.Set("syntaxRevision", Napi::Number::New(env, state_->syntax_revision));
  result.Set("languageGeneration",
             Napi::Number::New(env, state_->language_generation));
  result.Set("latestRequestedRevision",
             Napi::Number::New(env, state_->latest_requested_revision));
  result.Set("activeJobs", Napi::Number::New(env, state_->active ? 1 : 0));
  result.Set("pendingJobs", Napi::Number::New(env, state_->pending ? 1 : 0));
  result.Set("activeInjectionJobs",
             Napi::Number::New(env, state_->active_injection_jobs));
  result.Set("activeHighlightJobs",
             Napi::Number::New(env, state_->active_highlight_jobs));
  result.Set("pendingHighlightJobs",
             Napi::Number::New(
                 env, state_->pending_highlight_request ? 1 : 0));
  result.Set("stagedHighlightCommits",
             Napi::Number::New(env,
                               state_->staged_highlight_batches.size()));
  uint64_t staged_highlight_bytes = 0;
  for (const auto &[_, candidate] : state_->staged_highlight_batches) {
    if (candidate)
      staged_highlight_bytes += candidate->retained_bytes;
  }
  result.Set("stagedHighlightBytes",
             Napi::Number::New(env, staged_highlight_bytes));
  result.Set("revisionsRequested",
             Napi::Number::New(env, state_->counters.revisions_requested));
  result.Set("revisionsPublished",
             Napi::Number::New(env, state_->counters.revisions_published));
  result.Set("revisionsStale",
             Napi::Number::New(env, state_->counters.revisions_stale));
  result.Set("revisionsSuperseded",
             Napi::Number::New(env, state_->counters.revisions_superseded));
  result.Set("snapshotChunksRead",
             Napi::Number::New(env, state_->counters.snapshot_chunks_read));
  result.Set("snapshotLinesRead",
             Napi::Number::New(env, state_->counters.snapshot_lines_read));
  result.Set("synchronousAnalyses",
             Napi::Number::New(env,
                               state_->counters.synchronous_analyses));
  result.Set("synchronousIncrementalAnalyses",
             Napi::Number::New(
                 env, state_->counters.synchronous_incremental_analyses));
  result.Set("synchronousLineIndexAnalyses",
             Napi::Number::New(
                 env, state_->counters.synchronous_line_index_analyses));
  result.Set("synchronousFullAnalyses",
             Napi::Number::New(
                 env, state_->counters.synchronous_full_analyses));
  result.Set("synchronousAnalysisMilliseconds",
             Napi::Number::New(env,
                               state_->synchronous_analysis_milliseconds));
  result.Set("snapshotInputBytesCopied",
             Napi::Number::New(env,
                               state_->counters.snapshot_input_bytes_copied));
  result.Set("fullBufferMaterializations",
             Napi::Number::New(env,
                               state_->counters.full_buffer_materializations));
  result.Set("viewportUtf16Copied",
             Napi::Number::New(env, state_->counters.viewport_utf16_copied));
  result.Set("nativeSyntaxWasm",
             Napi::Boolean::New(env,
                                syntax_backend_capabilities().native_wasm));
  result.Set("nativeQueries",
             Napi::Boolean::New(env,
                                syntax_backend_capabilities().native_queries));
  result.Set("nativeGrammarCache",
             Napi::Boolean::New(
                 env, syntax_backend_capabilities().native_grammar_cache));
  result.Set("syntaxBackend",
             Napi::String::New(env, syntax_backend_capabilities().backend));
  result.Set("syntaxUnavailableReason",
             Napi::String::New(
                 env, state_->syntax_disabled_reason.empty()
                          ? syntax_backend_capabilities().reason
                          : state_->syntax_disabled_reason));
  result.Set("languageId", Napi::String::New(env, state_->language_id));
  result.Set("runtime", Napi::String::New(env, state_->runtime));
  result.Set("wasmPath", Napi::String::New(env, state_->wasm_path));
  result.Set("languageSegment",
             Napi::String::New(env, state_->language_segment));
  result.Set("languageName", Napi::String::New(env, state_->language_name));
  result.Set("queryFileCount",
             Napi::Number::New(env, state_->query_file_count));
  result.Set("maxSyntaxUtf16Length",
             Napi::Number::New(
                 env, static_cast<double>(state_->maximum_syntax_utf16_length)));
  result.Set("syntaxParses",
             Napi::Number::New(env, state_->counters.syntax_parses));
  result.Set("syntaxIncrementalParses",
             Napi::Number::New(env,
                               state_->counters.syntax_incremental_parses));
  result.Set("syntaxCancelledParses",
             Napi::Number::New(env, state_->counters.syntax_cancelled_parses));
  result.Set("syntaxInputTooLarge",
             Napi::Number::New(env,
                               state_->counters.syntax_input_too_large));
  result.Set("syntaxFailOpenCount",
             Napi::Number::New(env,
                               state_->counters.syntax_fail_open));
  result.Set("grammarCacheHits",
             Napi::Number::New(env, state_->counters.grammar_cache_hits));
  result.Set("grammarCacheMisses",
             Napi::Number::New(env, state_->counters.grammar_cache_misses));
  result.Set("syntaxChecksum",
             Napi::String::New(
                 env, state_->syntax_revision == 0
                          ? std::string()
                          : checksum_string(state_->syntax_checksum)));
  result.Set("syntaxNodeCount",
             Napi::Number::New(env,
                               static_cast<double>(state_->syntax_node_count)));
  result.Set("syntaxWasmBytes",
             Napi::Number::New(env,
                               static_cast<double>(state_->syntax_wasm_bytes)));
  result.Set("syntaxRootType",
             Napi::String::New(env, state_->syntax_root_type));
  result.Set("resolvedLanguageName",
             Napi::String::New(env,
                               state_->syntax_resolved_language_name));
  result.Set("grammarFingerprint",
             Napi::String::New(env,
                               state_->syntax_grammar_fingerprint));
  result.Set("syntaxRootHasError",
             Napi::Boolean::New(env, state_->syntax_root_has_error));
  result.Set("syntaxLastIncremental",
             Napi::Boolean::New(env, state_->syntax_last_incremental));
  result.Set("grammarLoadMilliseconds",
             Napi::Number::New(
                 env, state_->syntax_grammar_load_milliseconds));
  result.Set("grammarCacheHit",
             Napi::Boolean::New(env,
                                state_->syntax_last_grammar_cache_hit));
  const SyntaxGrammarCacheDiagnostics cache =
      syntax_grammar_cache_diagnostics();
  result.Set("grammarCacheEntries",
             Napi::Number::New(env, static_cast<double>(cache.entries)));
  result.Set("grammarCacheCompilations",
             Napi::Number::New(env,
                               static_cast<double>(cache.compilations)));
  result.Set("grammarCacheProcessHits",
             Napi::Number::New(env, static_cast<double>(cache.hits)));
  result.Set("grammarCacheProcessMisses",
             Napi::Number::New(env, static_cast<double>(cache.misses)));
  result.Set("grammarCacheCapacity",
             Napi::Number::New(env, static_cast<double>(cache.capacity)));
  result.Set("grammarCacheEvictions",
             Napi::Number::New(env, static_cast<double>(cache.evictions)));
  result.Set("parseMilliseconds",
             Napi::Number::New(env, state_->syntax_parse_milliseconds));
  result.Set("queryFilesLoaded",
             Napi::Number::New(env, state_->counters.query_files_loaded));
  result.Set("queryProgramsCompiled",
             Napi::Number::New(env,
                               state_->counters.query_programs_compiled));
  result.Set("queryProgramsExecuted",
             Napi::Number::New(env,
                               state_->counters.query_programs_executed));
  result.Set("queryCaptureCount",
             Napi::Number::New(env,
                               static_cast<double>(state_->query_capture_count)));
  result.Set("queryCapturesTotal",
             Napi::Number::New(env, state_->counters.query_captures));
  result.Set("queryPatternCount",
             Napi::Number::New(env,
                               static_cast<double>(state_->query_pattern_count)));
  result.Set("queryTextPredicates",
             Napi::Number::New(env,
                               state_->counters.query_text_predicates));
  result.Set("queryCustomPredicates",
             Napi::Number::New(env,
                               state_->counters.query_custom_predicates));
  result.Set("queryUnresolvedPredicates",
             Napi::Number::New(
                 env, state_->counters.query_unresolved_predicates));
  result.Set("queryUnresolvedRegexPredicates",
             Napi::Number::New(
                 env, state_->counters.query_unresolved_regex_predicates));
  result.Set("queryScopePredicates",
             Napi::Number::New(env,
                               state_->counters.query_scope_predicates));
  result.Set("queryCacheHits",
             Napi::Number::New(env, state_->counters.query_cache_hits));
  result.Set("queryCacheMisses",
             Napi::Number::New(env, state_->counters.query_cache_misses));
  result.Set("queryCacheEvictions",
             Napi::Number::New(env, state_->counters.query_cache_evictions));
  result.Set("queryCacheCapacity",
             Napi::Number::New(
                  env, static_cast<double>(NATIVE_QUERY_CACHE_CAPACITY)));
  result.Set("layeredQueryExecutions",
             Napi::Number::New(
                 env, state_->counters.layered_query_executions));
  result.Set("layeredQueryLayers",
             Napi::Number::New(env,
                               state_->counters.layered_query_layers));
  result.Set("queryProjectedCaptures",
             Napi::Number::New(
                 env, state_->counters.query_projected_captures));
  result.Set("queryStaleCaptures",
             Napi::Number::New(env,
                               state_->counters.query_stale_captures));
  result.Set("queryCoveredCaptures",
             Napi::Number::New(env,
                               state_->counters.query_covered_captures));
  const QuerySnapshotCacheDiagnostics snapshot_cache =
      state_->query_snapshot_cache->diagnostics();
  result.Set("queryResultCacheEntries",
             Napi::Number::New(env,
                               static_cast<double>(snapshot_cache.entries)));
  result.Set("queryResultCacheHits",
             Napi::Number::New(env,
                               static_cast<double>(snapshot_cache.hits)));
  result.Set("queryResultCacheMisses",
             Napi::Number::New(env,
                               static_cast<double>(snapshot_cache.misses)));
  result.Set("queryResultCacheEvictions",
             Napi::Number::New(env,
                               static_cast<double>(snapshot_cache.evictions)));
  result.Set("queryResultCacheCapacity",
             Napi::Number::New(
                 env, static_cast<double>(QUERY_SNAPSHOT_CACHE_CAPACITY)));
  result.Set("queryCompileMilliseconds",
             Napi::Number::New(env, state_->query_compile_milliseconds));
  result.Set("queryExecuteMilliseconds",
             Napi::Number::New(env, state_->query_execute_milliseconds));
  result.Set("queryLanguageSegmentSubstitutions",
             Napi::Number::New(
                 env, static_cast<double>(
                          state_->query_language_segment_substitutions)));
  result.Set("queryLanguageSegmentWarnings",
             Napi::Number::New(
                 env, static_cast<double>(
                          state_->query_language_segment_warnings)));
  result.Set("queryLastError",
             Napi::String::New(env, state_->query_last_error));
  result.Set("queryLastErrorCode",
             Napi::String::New(env, state_->query_last_error_code));
  result.Set("queryLastErrorType",
             Napi::String::New(env, state_->query_last_error_type));
  result.Set("queryLastErrorPath",
             Napi::String::New(env, state_->query_last_error_path));
  result.Set("queryLastErrorLine",
             Napi::Number::New(env, state_->query_last_error_line));
  result.Set("queryLastErrorColumn",
             Napi::Number::New(env, state_->query_last_error_column));
  result.Set("syntaxLastError",
             Napi::String::New(env, state_->syntax_last_error));
  result.Set("syntaxLastErrorCode",
             Napi::String::New(env, state_->syntax_last_error_code));
  const InjectionEngineDiagnostics injections =
      state_->injection_engine->diagnostics();
  result.Set("injectionActiveRequests",
             Napi::Number::New(env, injections.active_request_count));
  result.Set("injectionCandidateCount",
             Napi::Number::New(env, injections.candidate_count));
  result.Set("injectionLayerCount",
             Napi::Number::New(env, injections.active_layer_count));
  result.Set("dynamicInjectionLayerCount",
             Napi::Number::New(env, injections.dynamic_layer_count));
  result.Set("queryInjectionLayerCount",
             Napi::Number::New(env, injections.query_layer_count));
  result.Set("injectionRangeCount",
             Napi::Number::New(env, injections.range_count));
  result.Set("parsedInjectionLayerCount",
             Napi::Number::New(env,
                               injections.parsed_child_layer_count));
  result.Set("failedInjectionLayerCount",
             Napi::Number::New(env,
                               injections.failed_child_layer_count));
  result.Set("maximumInjectionDepth",
             Napi::Number::New(env, injections.maximum_depth));
  result.Set("unresolvedQueryInjectionLanguages",
             Napi::Number::New(
                 env, injections.unresolved_query_language_count));
  result.Set("queryInjectionLanguageResolutions",
             Napi::Number::New(
                 env, injections.query_language_resolution_count));
  result.Set("queryInjectionLanguageRejections",
             Napi::Number::New(
                 env, injections.query_language_rejection_count));
  result.Set("injectionStaleRequests",
             Napi::Number::New(env, injections.stale_request_count));
  result.Set("injectionAbortedRequests",
             Napi::Number::New(env, injections.aborted_request_count));
  result.Set("injectionPublishedGeneration",
             Napi::Number::New(env, injections.published_generation));
  result.Set("injectionTopologyGeneration",
             Napi::Number::New(env, injections.topology_generation));
  const HighlightIndexDiagnostics highlights =
      state_->highlight_index->diagnostics();
  result.Set("highlightGeneration",
             Napi::Number::New(env, highlights.generation));
  result.Set("highlightShardCount",
             Napi::Number::New(env, highlights.shard_count));
  result.Set("highlightRetainedBytes",
             Napi::Number::New(env, highlights.retained_bytes));
  result.Set("highlightEvictions",
             Napi::Number::New(env, highlights.evictions));
  result.Set("highlightRangeCount",
             Napi::Number::New(env, highlights.range_count));
  result.Set("highlightZeroLengthRangeCount",
             Napi::Number::New(env,
                               highlights.zero_length_range_count));
  result.Set("highlightRequests",
             Napi::Number::New(env, state_->counters.highlight_requests));
  result.Set("highlightCacheHits",
             Napi::Number::New(env, state_->counters.highlight_cache_hits));
  result.Set("highlightCacheMisses",
             Napi::Number::New(env, state_->counters.highlight_cache_misses));
  result.Set("highlightRequestsCoalesced",
             Napi::Number::New(
                 env, state_->counters.highlight_requests_coalesced));
  result.Set("highlightJobsQueued",
             Napi::Number::New(env, state_->counters.highlight_jobs_queued));
  result.Set("highlightJobsCompleted",
             Napi::Number::New(
                 env, state_->counters.highlight_jobs_completed));
  result.Set("highlightJobsCancelled",
             Napi::Number::New(
                 env, state_->counters.highlight_jobs_cancelled));
  result.Set("highlightRequestsSuperseded",
             Napi::Number::New(
                 env, state_->counters.highlight_requests_superseded));
  result.Set("highlightStaleResults",
             Napi::Number::New(
                 env, state_->counters.highlight_stale_results));
  result.Set("highlightShardsPublished",
             Napi::Number::New(
                 env, state_->counters.highlight_shards_published));
  result.Set("highlightCaptureCount",
             Napi::Number::New(
                 env, state_->counters.highlight_capture_count));
  result.Set("highlightFallbackSync",
             Napi::Number::New(
                 env, state_->counters.highlight_fallback_sync));
  result.Set("highlightFailOpen",
             Napi::Number::New(
                 env, state_->counters.highlight_fail_open));
  result.Set("highlightQueueMilliseconds",
             Napi::Number::New(env, state_->highlight_queue_milliseconds));
  result.Set("highlightQueryMilliseconds",
             Napi::Number::New(env, state_->highlight_query_milliseconds));
  result.Set("highlightCommitMilliseconds",
             Napi::Number::New(env, state_->highlight_commit_milliseconds));
  return result;
}

Napi::Value DocumentSessionWrapper::drain(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
  Napi::Promise promise = deferred.Promise();
  bool idle = false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    idle = !state_->active && !state_->pending &&
           state_->active_injection_jobs == 0 &&
           state_->active_highlight_jobs == 0 &&
           !state_->pending_highlight_request &&
           state_->staged_highlight_batches.empty();
    if (!idle)
      state_->drain_waiters.push_back(deferred);
  }
  if (idle)
    deferred.Resolve(env.Undefined());
  return promise;
}

Napi::Value DocumentSessionWrapper::release_snapshot_leases(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !info[0].IsArray())
    return throw_type_error(env, "_releaseSnapshotLeases expects an array",
                            "ERR_INVALID_SYNTAX_NODE_LEASES");
  Napi::Array ids = info[0].As<Napi::Array>();
  for (uint32_t index = 0; index < ids.Length(); index++) {
    uint64_t id = 0;
    if (!read_uint64(ids.Get(index), &id))
      return throw_type_error(env, "Snapshot lease ids must be integers",
                              "ERR_INVALID_SYNTAX_NODE_LEASES");
    bool tracked = false;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      tracked = state_->transient_node_lease_ids.erase(id) > 0;
    }
    if (tracked && id != 0 && state_->addon_data != nullptr)
      state_->addon_data->release_node_lease(id);
  }
  return env.Undefined();
}

Napi::Value DocumentSessionWrapper::track_snapshot_leases(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !info[0].IsArray())
    return throw_type_error(env, "_trackSnapshotLeases expects an array",
                            "ERR_INVALID_SYNTAX_NODE_LEASES");
  Napi::Array ids = info[0].As<Napi::Array>();
  std::vector<uint64_t> release_immediately;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (uint32_t index = 0; index < ids.Length(); index++) {
      uint64_t id = 0;
      if (!read_uint64(ids.Get(index), &id))
        return throw_type_error(env, "Snapshot lease ids must be integers",
                                "ERR_INVALID_SYNTAX_NODE_LEASES");
      if (id == 0)
        continue;
      if (state_->destroyed)
        release_immediately.push_back(id);
      else
        state_->transient_node_lease_ids.insert(id);
    }
  }
  if (state_->addon_data != nullptr) {
    for (uint64_t id : release_immediately)
      state_->addon_data->release_node_lease(id);
  }
  return env.Undefined();
}

void DocumentSessionWrapper::mark_destroyed(Napi::Env env) {
  state_->destroyed_signal.store(true, std::memory_order_relaxed);
  state_->highlight_cancellation_signal.fetch_add(
      1, std::memory_order_relaxed);
  std::unique_ptr<RevisionRequest> pending;
  const SuperstringSnapshotLease *current = nullptr;
  const SuperstringSnapshotLease *syntax = nullptr;
  std::vector<uint64_t> transient_node_lease_ids;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->destroyed)
      return;
    state_->destroyed = true;
    pending = std::move(state_->pending);
    current = state_->current_lease;
    state_->current_lease = nullptr;
    syntax = state_->syntax_lease;
    state_->syntax_lease = nullptr;
    state_->analysis.reset();
    state_->syntax_analysis.reset();
    state_->published_syntax.reset();
    state_->highlight_index->invalidate();
    transient_node_lease_ids.assign(state_->transient_node_lease_ids.begin(),
                                    state_->transient_node_lease_ids.end());
    state_->transient_node_lease_ids.clear();
    state_->injection_engine->clear();
  }
  cancel_highlight_requests(env, state_, "destroyed");
  if (state_->addon_data != nullptr) {
    for (uint64_t id : transient_node_lease_ids)
      state_->addon_data->release_node_lease(id);
  }
  release_lease(current);
  release_lease(syntax);
  reject_request(env, std::move(pending), "Document session was destroyed",
                 "ERR_DOCUMENT_SESSION_DESTROYED");
  resolve_drains_if_idle(env, state_);
}

Napi::Value DocumentSessionWrapper::destroy(const Napi::CallbackInfo &info) {
  mark_destroyed(info.Env());
  return drain(info);
}

} // namespace document_engine
