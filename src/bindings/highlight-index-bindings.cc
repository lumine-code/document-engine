#include "bindings/highlight-index-bindings.h"

#include "bindings/addon-data.h"
#include "bindings/document-session.h"
#include "bindings/display-view.h"
#include "snapshot-lease.h"
#include "syntax/highlight-index.h"
#include "syntax/injection-engine.h"
#include "syntax/layered-query.h"
#include "syntax/query-snapshot-cache.h"
#include "syntax/syntax-backend.h"
#include "text-bridge/snapshot-reader.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace document_engine {

namespace {

using HighlightClock = std::chrono::steady_clock;

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

Napi::Value throw_error(Napi::Env env, const char *message,
                        const char *code) {
  Napi::Error error = Napi::Error::New(env, message);
  set_error_code(error, code);
  error.ThrowAsJavaScriptException();
  return env.Undefined();
}

void release_lease(const SuperstringSnapshotLease *lease) {
  if (lease != nullptr && lease->functions != nullptr)
    lease->functions->release(lease->context);
}

bool read_config_value(Napi::Value value, QueryConfigValue *result) {
  if (value.IsNull() || value.IsUndefined()) {
    *result = std::monostate{};
    return true;
  }
  if (value.IsBoolean()) {
    *result = value.As<Napi::Boolean>().Value();
    return true;
  }
  if (value.IsNumber()) {
    *result = value.As<Napi::Number>().DoubleValue();
    return true;
  }
  if (value.IsString()) {
    *result = value.As<Napi::String>().Utf8Value();
    return true;
  }
  return false;
}

bool read_config_object(Napi::Env env, Napi::Value value,
                        QueryResolutionContext *resolution) {
  if (value.IsUndefined())
    return true;
  if (!value.IsObject() || value.IsArray()) {
    throw_type_error(env, "Highlight scope config must be an object",
                     "ERR_INVALID_HIGHLIGHT_CONTEXT");
    return false;
  }
  Napi::Object object = value.As<Napi::Object>();
  Napi::Array keys = object.GetPropertyNames();
  for (uint32_t index = 0; index < keys.Length(); index++) {
    Napi::Value key = keys.Get(index);
    if (!key.IsString())
      continue;
    QueryConfigValue parsed;
    if (!read_config_value(object.Get(key), &parsed)) {
      throw_type_error(
          env,
          "Highlight scope config values must be null, boolean, number or string",
          "ERR_INVALID_HIGHLIGHT_CONTEXT");
      return false;
    }
    resolution->config[key.As<Napi::String>().Utf8Value()] =
        std::move(parsed);
  }
  return true;
}

bool read_context(Napi::Env env, Napi::Value value,
                  uint64_t *context_generation,
                  LayeredQueryContext *context,
                  std::vector<uint32_t> *shard_starts) {
  if (!value.IsObject() || value.IsArray()) {
    throw_type_error(env, "Highlight options must be an object",
                     "ERR_INVALID_HIGHLIGHT_CONTEXT");
    return false;
  }
  Napi::Object options = value.As<Napi::Object>();
  if (!read_uint64(options.Get("contextGeneration"), context_generation)) {
    throw_type_error(env, "Highlight contextGeneration must be an integer",
                     "ERR_INVALID_HIGHLIGHT_CONTEXT");
    return false;
  }
  context->defaults.resolve_scopes = true;
  context->defaults.interpolate_names = true;
  context->defaults.capture_limit = HIGHLIGHT_CAPTURE_CAPACITY;
  context->include_language_scopes = true;
  context->include_node_handles = false;
  Napi::Value packed_shards = options.Get("shardStarts");
  if (!packed_shards.IsUndefined()) {
    if (!packed_shards.IsTypedArray() ||
        packed_shards.As<Napi::TypedArray>().TypedArrayType() !=
            napi_uint32_array) {
      throw_type_error(env, "shardStarts must be a Uint32Array",
                       "ERR_INVALID_HIGHLIGHT_RANGE");
      return false;
    }
    Napi::Uint32Array values = packed_shards.As<Napi::Uint32Array>();
    for (size_t index = 0; index < values.ElementLength(); index++)
      shard_starts->push_back(highlight_shard_start(values[index]));
    std::sort(shard_starts->begin(), shard_starts->end());
    shard_starts->erase(
        std::unique(shard_starts->begin(), shard_starts->end()),
        shard_starts->end());
    if (shard_starts->empty() ||
        shard_starts->size() > HIGHLIGHT_MAX_REQUEST_SHARDS) {
      throw_type_error(env, "shardStarts must contain 1 to 8 unique shards",
                       "ERR_INVALID_HIGHLIGHT_RANGE");
      return false;
    }
  }
  if (!read_config_object(env, options.Get("scopeConfig"),
                          &context->defaults))
    return false;
  Napi::Value by_language_value = options.Get("scopeConfigsByLanguage");
  if (by_language_value.IsUndefined())
    return true;
  if (!by_language_value.IsObject() || by_language_value.IsArray()) {
    throw_type_error(env, "scopeConfigsByLanguage must be an object",
                     "ERR_INVALID_HIGHLIGHT_CONTEXT");
    return false;
  }
  Napi::Object by_language = by_language_value.As<Napi::Object>();
  Napi::Array grammar_ids = by_language.GetPropertyNames();
  for (uint32_t index = 0; index < grammar_ids.Length(); index++) {
    Napi::Value grammar_id = grammar_ids.Get(index);
    if (!grammar_id.IsString())
      continue;
    QueryResolutionContext resolution = context->defaults;
    resolution.config.clear();
    if (!read_config_object(env, by_language.Get(grammar_id), &resolution))
      return false;
    context->by_grammar.emplace(grammar_id.As<Napi::String>().Utf8Value(),
                                std::move(resolution));
  }
  return true;
}

void set_tags(Napi::Object &result, const HighlightIndexTags &tags) {
  result.Set("bufferRevision", Napi::Number::New(
                                   result.Env(), tags.buffer_revision));
  result.Set("syntaxRevision", Napi::Number::New(
                                   result.Env(), tags.syntax_revision));
  result.Set("languageGeneration", Napi::Number::New(
                                      result.Env(), tags.language_generation));
  result.Set("injectionGeneration", Napi::Number::New(
                                       result.Env(), tags.injection_generation));
  result.Set("contextGeneration", Napi::Number::New(
                                     result.Env(), tags.context_generation));
}

Napi::Object response(Napi::Env env, bool accepted, const char *reason,
                      const HighlightIndexTags &tags, uint64_t request_id,
                      uint32_t start_row, uint32_t end_row,
                      uint64_t generation, bool published,
                      bool fallback_sync = false) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("accepted", Napi::Boolean::New(env, accepted));
  result.Set("published", Napi::Boolean::New(env, published));
  result.Set("needsCommit", Napi::Boolean::New(env, false));
  result.Set("fallbackSync", Napi::Boolean::New(env, fallback_sync));
  result.Set("permanentIncomplete", Napi::Boolean::New(env, false));
  result.Set("requestId", Napi::Number::New(env, request_id));
  result.Set("coverageStartRow", Napi::Number::New(env, start_row));
  result.Set("coverageEndRow", Napi::Number::New(env, end_row));
  result.Set("highlightGeneration", Napi::Number::New(env, generation));
  result.Set("captureNames", Napi::Array::New(env));
  result.Set("captureGrammarIds", Napi::Array::New(env));
  if (reason != nullptr && *reason != '\0')
    result.Set("reason", Napi::String::New(env, reason));
  set_tags(result, tags);
  return result;
}

bool tags_current_locked(const SessionState &state,
                         const HighlightIndexTags &tags) {
  return !state.destroyed &&
         state.buffer_revision == tags.buffer_revision &&
         state.syntax_revision == tags.syntax_revision &&
         state.language_generation == tags.language_generation &&
         state.syntax_revision == state.buffer_revision &&
         state.injection_engine->diagnostics().topology_generation ==
             tags.injection_generation &&
         state.highlight_index->active_for(tags);
}

uint32_t offset_for_row(const SnapshotAnalysis &analysis, uint32_t row) {
  if (row >= analysis.line_starts.size())
    return static_cast<uint32_t>(std::min<uint64_t>(
        analysis.utf16_length, std::numeric_limits<uint32_t>::max()));
  return static_cast<uint32_t>(std::min<uint64_t>(
      analysis.line_starts[row], std::numeric_limits<uint32_t>::max()));
}

size_t query_retained_bytes(const QueryIndexSnapshot &query) {
  size_t result = sizeof(query) +
                  query.captures.capacity() * sizeof(QueryCaptureRecord) +
                  query.patterns.capacity() * sizeof(QueryPatternMetadata) +
                  query.layers.capacity() * sizeof(QueryLayerRecord);
  for (const std::string &name : query.capture_names)
    result += sizeof(name) + name.capacity();
  auto property_bytes = [](const std::vector<QueryProperty> &properties) {
    size_t bytes = properties.capacity() * sizeof(QueryProperty);
    for (const QueryProperty &property : properties)
      bytes += property.name.capacity() + property.value.capacity();
    return bytes;
  };
  auto predicate_bytes = [](const std::vector<QueryPredicate> &predicates) {
    size_t bytes = predicates.capacity() * sizeof(QueryPredicate);
    for (const QueryPredicate &predicate : predicates) {
      bytes += predicate.operator_name.capacity() +
               predicate.operands.capacity() * sizeof(QueryOperand);
      for (const QueryOperand &operand : predicate.operands)
        bytes += operand.value.capacity();
    }
    return bytes;
  };
  for (const QueryPatternMetadata &pattern : query.patterns) {
    result += property_bytes(pattern.set_properties) +
              property_bytes(pattern.asserted_properties) +
              property_bytes(pattern.refuted_properties) +
              predicate_bytes(pattern.custom_predicates) +
              predicate_bytes(pattern.unresolved_predicates);
  }
  for (const QueryLayerRecord &layer : query.layers) {
    result += sizeof(layer) + layer.grammar_id.capacity() +
              layer.ranges.capacity() * sizeof(QueryLayerRangeRecord);
    for (const QueryLayerRangeRecord &range : layer.ranges) {
      for (const std::string &scope : range.scopes)
        result += sizeof(scope) + scope.capacity();
    }
  }
  return result;
}

size_t staged_bytes_locked(const SessionState &state) {
  size_t result = 0;
  for (const auto &[_, candidate] : state.staged_highlight_batches) {
    if (candidate)
      result += candidate->retained_bytes;
  }
  return result;
}

} // namespace

struct HighlightCoverageRequest {
  HighlightCoverageRequest(Napi::Promise::Deferred waiter, uint64_t id,
                           HighlightIndexTags request_tags,
                           uint32_t start, uint32_t end,
                           LayeredQueryContext query_context,
                           bool partition_final_ranges)
      : request_id(id), tags(request_tags), coverage_start_row(start),
        coverage_end_row(end), context(std::move(query_context)),
        partition_by_final_range(partition_final_ranges),
        queued_at(HighlightClock::now()) {
    waiters.push_back(waiter);
  }

  ~HighlightCoverageRequest() { release_lease(lease); }

  bool same_request(const HighlightIndexTags &other_tags, uint32_t start,
                    uint32_t end, const std::vector<uint32_t> &shards,
                    uint64_t current_cancellation) const {
    return tags == other_tags && coverage_start_row == start &&
           coverage_end_row == end &&
           requested_shards == shards &&
           cancellation_generation == current_cancellation;
  }

  uint64_t request_id = 0;
  HighlightIndexTags tags;
  uint32_t coverage_start_row = 0;
  uint32_t coverage_end_row = 0;
  LayeredQueryContext context;
  std::shared_ptr<const PublishedSyntaxSnapshot> root;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  std::vector<uint32_t> missing_shards;
  std::vector<uint32_t> requested_shards;
  const SuperstringSnapshotLease *lease = nullptr;
  uint64_t cancellation_generation = 0;
  bool partition_by_final_range = false;
  HighlightClock::time_point queued_at;
  std::vector<Napi::Promise::Deferred> waiters;
};

namespace {

bool request_current_locked(
    const SessionState &state,
    const HighlightCoverageRequest &request) {
  return tags_current_locked(state, request.tags) &&
         state.highlight_cancellation_signal.load(
             std::memory_order_relaxed) == request.cancellation_generation;
}

struct HighlightCancellation {
  std::shared_ptr<SessionState> state;
  HighlightIndexTags tags;
  uint64_t generation = 0;
};

bool highlight_cancelled(void *payload) {
  if (payload == nullptr)
    return false;
  const HighlightCancellation &cancellation =
      *static_cast<const HighlightCancellation *>(payload);
  return cancellation.state->destroyed_signal.load(std::memory_order_relaxed) ||
         cancellation.state->latest_requested_signal.load(
             std::memory_order_relaxed) != cancellation.tags.buffer_revision ||
         cancellation.state->language_generation_signal.load(
             std::memory_order_relaxed) !=
             cancellation.tags.language_generation ||
         cancellation.state->highlight_cancellation_signal.load(
             std::memory_order_relaxed) != cancellation.generation;
}

void resolve_waiters(const std::shared_ptr<HighlightCoverageRequest> &request,
                     Napi::Value value) {
  if (!request)
    return;
  for (Napi::Promise::Deferred &waiter : request->waiters)
    waiter.Resolve(value);
  request->waiters.clear();
}

void resolve_superseded(Napi::Env env,
                        const std::shared_ptr<HighlightCoverageRequest> &request,
                        const char *reason) {
  if (!request)
    return;
  Napi::Object value = response(
      env, false, reason, request->tags, request->request_id,
      request->coverage_start_row, request->coverage_end_row, 0, false);
  resolve_waiters(request, value);
}

void queue_highlight_request(
    Napi::Env env, const std::shared_ptr<SessionState> &state,
    const std::shared_ptr<HighlightCoverageRequest> &request);

class HighlightCoverageWorker final : public Napi::AsyncWorker {
public:
  HighlightCoverageWorker(
      Napi::Env env, std::shared_ptr<SessionState> state,
      std::shared_ptr<HighlightCoverageRequest> request,
      std::shared_ptr<NativeJobControl> job)
      : Napi::AsyncWorker(env, "DocumentSession.requestHighlightCoverage"),
        state_(std::move(state)), request_(std::move(request)),
        job_(std::move(job)) {}

  void OnExecute(Napi::Env env) override {
    Napi::AsyncWorker::OnExecute(env);
    job_->mark_worker_finished();
    if (state_->addon_data != nullptr)
      state_->addon_data->notify_cleanup_progress();
  }

  void Execute() override {
    queue_milliseconds_ =
        std::chrono::duration<double, std::milli>(HighlightClock::now() -
                                                  request_->queued_at)
            .count();
    uint32_t delay = 0;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      delay = state_->worker_delay_ms;
    }
    HighlightCancellation cancellation{state_, request_->tags,
                                       request_->cancellation_generation};
    const auto delay_deadline =
        HighlightClock::now() + std::chrono::milliseconds(delay);
    while (HighlightClock::now() < delay_deadline &&
           !highlight_cancelled(&cancellation)) {
      std::this_thread::sleep_until(std::min(
          delay_deadline,
          HighlightClock::now() + std::chrono::milliseconds(10)));
    }
    SnapshotReader reader(job_->lease());
    if (!reader.valid()) {
      failure_reason_ = "snapshot-unavailable";
      return;
    }
    if (highlight_cancelled(&cancellation)) {
      cancelled_ = true;
      return;
    }
    auto candidate = std::make_shared<HighlightCandidateBatch>();
    candidate->request_id = request_->request_id;
    candidate->tags = request_->tags;
    candidate->coverage_start_row = request_->coverage_start_row;
    candidate->coverage_end_row = request_->coverage_end_row;
    candidate->requested_shards = request_->requested_shards;
    candidate->partition_by_final_range = request_->partition_by_final_range;
    for (uint32_t shard_start : request_->requested_shards) {
      const uint32_t shard_end = highlight_shard_end(shard_start);
      candidate->requested_shard_bounds.push_back(HighlightShardBounds{
          shard_start, shard_end,
          offset_for_row(*request_->analysis, shard_start),
          offset_for_row(*request_->analysis, shard_end),
          shard_end >= request_->analysis->line_starts.size()});
    }
    std::vector<std::pair<uint32_t, uint32_t>> query_windows;
    if (request_->partition_by_final_range) {
      const uint32_t envelope_start = request_->requested_shards.front();
      const uint32_t envelope_end =
          highlight_shard_end(request_->requested_shards.back());
      const uint32_t envelope_start_offset =
          offset_for_row(*request_->analysis, envelope_start);
      const uint32_t envelope_end_offset =
          offset_for_row(*request_->analysis, envelope_end);
      if (static_cast<uint64_t>(envelope_end) - envelope_start >
              HIGHLIGHT_MAX_COHORT_ENVELOPE_ROWS ||
          static_cast<uint64_t>(envelope_end_offset) -
                  envelope_start_offset >
              HIGHLIGHT_MAX_COHORT_ENVELOPE_UTF16) {
        permanent_incomplete_ = true;
        failure_reason_ = "highlight-envelope-capacity";
        return;
      }
      query_windows.emplace_back(envelope_start, envelope_end);
    } else {
      for (uint32_t shard_start : request_->missing_shards)
        query_windows.emplace_back(shard_start,
                                   highlight_shard_end(shard_start));
    }
    std::set<std::pair<std::string, std::string>> scope_keys;
    const auto started_at = HighlightClock::now();
    for (const auto &[shard_start, shard_end] : query_windows) {
      if (highlight_cancelled(&cancellation)) {
        cancelled_ = true;
        break;
      }
      QueryRange range;
      range.start_row = shard_start;
      range.start_column = 0;
      range.end_row = shard_end;
      range.end_column = 0;
      std::shared_ptr<const QueryIndexSnapshot> query;
      QueryErrorInfo error;
      QueryRunStatistics shard_statistics;
      if (!execute_layered_query(
              "highlightsQuery", request_->root, {}, reader,
              *request_->analysis, *request_->analysis, {},
              request_->tags.buffer_revision,
              request_->tags.language_generation, range, request_->context,
              nullptr, highlight_cancelled,
              &cancellation, query, error, shard_statistics)) {
        statistics_.programs_executed += shard_statistics.programs_executed;
        statistics_.captures += shard_statistics.captures;
        statistics_.execute_milliseconds +=
            shard_statistics.execute_milliseconds;
        if (error.code == "ERR_SYNTAX_QUERY_CANCELLED" ||
            highlight_cancelled(&cancellation)) {
          cancelled_ = true;
        } else {
          failure_reason_ = error.message.empty() ? "query-failed"
                                                  : error.message;
        }
        break;
      }
      statistics_.programs_executed += shard_statistics.programs_executed;
      statistics_.captures += shard_statistics.captures;
      statistics_.execute_milliseconds += shard_statistics.execute_milliseconds;
      if (!query) {
        failure_reason_ = "missing-query-result";
        break;
      }
      if (!query->resolution_complete || query->exceeded_match_limit) {
        permanent_incomplete_ = true;
        failure_reason_ = query->exceeded_match_limit
                              ? "highlight-match-limit"
                              : "highlight-resolution-incomplete";
        return;
      }
      for (const QueryCaptureRecord &capture : query->captures) {
        if (capture.name_id >= query->capture_names.size())
          continue;
        const std::string grammar_id =
            capture.layer_index < query->layers.size()
                ? query->layers[capture.layer_index].grammar_id
                : std::string{};
        scope_keys.emplace(grammar_id,
                           query->capture_names[capture.name_id]);
      }
      candidate->shards.push_back(HighlightQueryShard{
          shard_start, shard_end,
          offset_for_row(*request_->analysis, shard_start),
          offset_for_row(*request_->analysis, shard_end),
          shard_end >= request_->analysis->line_starts.size(),
          std::move(query)});
      candidate->retained_bytes +=
          query_retained_bytes(*candidate->shards.back().query);
      if (candidate->retained_bytes > HIGHLIGHT_SHARD_BYTE_CAPACITY) {
        permanent_incomplete_ = true;
        failure_reason_ = "highlight-capacity";
        return;
      }
    }
    query_milliseconds_ =
        std::chrono::duration<double, std::milli>(HighlightClock::now() -
                                                  started_at)
            .count();
    if (!cancelled_ && failure_reason_.empty()) {
      candidate->capture_names.reserve(scope_keys.size());
      candidate->capture_grammar_ids.reserve(scope_keys.size());
      for (const auto &[grammar_id, name] : scope_keys) {
        candidate->capture_grammar_ids.push_back(grammar_id);
        candidate->capture_names.push_back(name);
      }
      for (const std::string &name : candidate->capture_names)
        candidate->retained_bytes += sizeof(name) + name.capacity();
      for (const std::string &grammar_id : candidate->capture_grammar_ids)
        candidate->retained_bytes +=
            sizeof(grammar_id) + grammar_id.capacity();
      if (candidate->retained_bytes > HIGHLIGHT_SHARD_BYTE_CAPACITY) {
        permanent_incomplete_ = true;
        failure_reason_ = "highlight-capacity";
        return;
      }
      candidate_ = std::move(candidate);
    }
  }

  void OnWorkComplete(Napi::Env env, napi_status status) override {
    if (state_->environment_cleanup_signal.load(std::memory_order_acquire)) {
      job_->release_lease_on_owner();
      request_.reset();
      if (state_->addon_data != nullptr)
        state_->addon_data->notify_cleanup_progress();
      Napi::AsyncWorker::OnWorkComplete(env, napi_cancelled);
      return;
    }
    if (status == napi_cancelled) {
      std::shared_ptr<HighlightCoverageRequest> next;
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->active_highlight_request.reset();
        next = std::move(state_->pending_highlight_request);
        state_->counters.highlight_jobs_cancelled++;
      }
      resolve_superseded(env, request_, "cancelled");
      job_->release_lease_on_owner();
      if (next) {
        bool queue_next = false;
        {
          std::lock_guard<std::mutex> lock(state_->mutex);
          queue_next = request_current_locked(*state_, *next);
        }
        if (queue_next)
          queue_highlight_request(env, state_, next);
        else
          resolve_superseded(env, next, "stale-request");
      }
      finish_highlight_job(env, state_);
      Napi::AsyncWorker::OnWorkComplete(env, napi_cancelled);
      return;
    }
    Napi::AsyncWorker::OnWorkComplete(env, status);
  }

  void OnOK() override {
    Napi::Env env = Env();
    std::shared_ptr<HighlightCoverageRequest> next;
    bool accepted = false;
    bool destroyed = false;
    bool permanent_incomplete_current = false;
    uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      destroyed = state_->destroyed;
      accepted = !destroyed && !cancelled_ && !permanent_incomplete_ && candidate_ &&
                 failure_reason_.empty() &&
                 request_current_locked(*state_, *request_);
      permanent_incomplete_current =
          permanent_incomplete_ && !destroyed &&
          request_current_locked(*state_, *request_);
      state_->highlight_queue_milliseconds += queue_milliseconds_;
      state_->highlight_query_milliseconds += query_milliseconds_;
      state_->counters.query_programs_executed +=
          statistics_.programs_executed;
      state_->counters.query_captures += statistics_.captures;
      if (permanent_incomplete_current) {
        state_->counters.highlight_fail_open++;
        state_->highlight_cancellation_signal.fetch_add(
            1, std::memory_order_relaxed);
        state_->highlight_index->invalidate();
        state_->highlight_index->activate(request_->tags);
        state_->staged_highlight_batches.clear();
        state_->async_highlight_mode = true;
        generation = state_->highlight_index->generation();
        for (auto iterator = state_->display_views.begin();
             iterator != state_->display_views.end();) {
          if (std::shared_ptr<DisplayViewState> view = iterator->lock()) {
            view->highlight_ranges.clear();
            view->highlight_buffer_revision = 0;
            view->highlight_syntax_revision = 0;
            view->display_revision++;
            iterator++;
          } else {
            iterator = state_->display_views.erase(iterator);
          }
        }
      } else if (cancelled_)
        state_->counters.highlight_jobs_cancelled++;
      else if (!accepted)
        state_->counters.highlight_stale_results++;
      else {
        state_->counters.highlight_jobs_completed++;
        state_->counters.highlight_capture_count += statistics_.captures;
        while (!state_->staged_highlight_batches.empty() &&
               (state_->staged_highlight_batches.size() >= 4 ||
                staged_bytes_locked(*state_) + candidate_->retained_bytes >
                    HIGHLIGHT_SHARD_BYTE_CAPACITY))
          state_->staged_highlight_batches.erase(
              state_->staged_highlight_batches.begin());
        state_->staged_highlight_batches[candidate_->request_id] = candidate_;
        generation = state_->highlight_index->generation();
      }
      state_->active_highlight_request.reset();
      next = std::move(state_->pending_highlight_request);
    }

    Napi::Object value = response(
        env, accepted,
        accepted ? "" : permanent_incomplete_current ? failure_reason_.c_str()
                      : destroyed                  ? "destroyed"
                      : cancelled_                ? "cancelled"
                                                   : "stale-result",
        request_->tags, request_->request_id, request_->coverage_start_row,
        request_->coverage_end_row, generation, false, false);
    if (permanent_incomplete_current)
      value.Set("permanentIncomplete", Napi::Boolean::New(env, true));
    if (accepted && candidate_) {
      value.Set("needsCommit", Napi::Boolean::New(env, true));
      Napi::Array names =
          Napi::Array::New(env, candidate_->capture_names.size());
      Napi::Array grammar_ids =
          Napi::Array::New(env, candidate_->capture_grammar_ids.size());
      for (uint32_t index = 0; index < candidate_->capture_names.size();
           index++) {
        names.Set(index,
                  Napi::String::New(env, candidate_->capture_names[index]));
        grammar_ids.Set(
            index,
            Napi::String::New(env,
                              candidate_->capture_grammar_ids[index]));
      }
      value.Set("captureNames", names);
      value.Set("captureGrammarIds", grammar_ids);
      value.Set("queryMilliseconds",
                Napi::Number::New(env, query_milliseconds_));
      value.Set("queueMilliseconds",
                Napi::Number::New(env, queue_milliseconds_));
    }
    resolve_waiters(request_, value);
    job_->release_lease_on_owner();

    if (next) {
      bool queue_next = false;
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
          queue_next = request_current_locked(*state_, *next);
      }
      if (queue_next)
        queue_highlight_request(env, state_, next);
      else
        resolve_superseded(env, next, "stale-request");
    }
    finish_highlight_job(env, state_);
  }

  void OnError(const Napi::Error &error) override {
    Napi::Env env = Env();
    std::shared_ptr<HighlightCoverageRequest> next;
    resolve_superseded(env, request_, error.Message().c_str());
    job_->release_lease_on_owner();
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->active_highlight_request.reset();
      next = std::move(state_->pending_highlight_request);
      state_->counters.highlight_stale_results++;
    }
    if (next) {
      bool queue_next = false;
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        queue_next = request_current_locked(*state_, *next);
      }
      if (queue_next)
        queue_highlight_request(env, state_, next);
      else
        resolve_superseded(env, next, "stale-request");
    }
    finish_highlight_job(env, state_);
  }

private:
  std::shared_ptr<SessionState> state_;
  std::shared_ptr<HighlightCoverageRequest> request_;
  std::shared_ptr<NativeJobControl> job_;
  std::shared_ptr<HighlightCandidateBatch> candidate_;
  QueryRunStatistics statistics_;
  bool cancelled_ = false;
  bool permanent_incomplete_ = false;
  std::string failure_reason_;
  double queue_milliseconds_ = 0;
  double query_milliseconds_ = 0;
};

void queue_highlight_request(
    Napi::Env env, const std::shared_ptr<SessionState> &state,
    const std::shared_ptr<HighlightCoverageRequest> &request) {
  auto job = std::make_shared<NativeJobControl>(request->lease);
  request->lease = nullptr;
  DocumentSessionWrapper *owner_to_retain = nullptr;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->active_highlight_request = request;
    state->active_highlight_jobs++;
    state->counters.highlight_jobs_queued++;
    if (!state->owner_referenced && state->owner != nullptr) {
      state->owner_referenced = true;
      owner_to_retain = state->owner;
    }
    register_native_job_locked(*state, job);
  }
  if (owner_to_retain != nullptr)
    owner_to_retain->Ref();
  (new HighlightCoverageWorker(env, state, request, std::move(job)))->Queue();
}

} // namespace

Napi::Value request_highlight_coverage(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &state) {
  Napi::Env env = info.Env();
  uint64_t start_row_raw = 0;
  uint64_t end_row_raw = 0;
  if (info.Length() < 3 || !read_uint64(info[0], &start_row_raw) ||
      !read_uint64(info[1], &end_row_raw) ||
      start_row_raw >= end_row_raw || end_row_raw > UINT32_MAX)
    return throw_type_error(env,
                            "requestHighlightCoverage expects an ordered uint32 row range",
                            "ERR_INVALID_HIGHLIGHT_RANGE");

  uint64_t context_generation = 0;
  LayeredQueryContext context;
  std::vector<uint32_t> requested_shards;
  if (!read_context(env, info[2], &context_generation, &context,
                    &requested_shards))
    return env.Undefined();

  uint32_t start_row =
      highlight_shard_start(static_cast<uint32_t>(start_row_raw));
  uint32_t end_row = static_cast<uint32_t>(end_row_raw);
  if (end_row % HIGHLIGHT_SHARD_ROWS != 0)
    end_row = highlight_shard_end(highlight_shard_start(end_row));
  if (end_row <= start_row)
    end_row = highlight_shard_end(start_row);
  if (requested_shards.empty()) {
    const uint64_t shard_count =
        (static_cast<uint64_t>(end_row) - start_row +
         HIGHLIGHT_SHARD_ROWS - 1) /
        HIGHLIGHT_SHARD_ROWS;
    if (shard_count > HIGHLIGHT_MAX_REQUEST_SHARDS)
      return throw_type_error(
          env, "Highlight coverage request exceeds the shard window limit",
          "ERR_INVALID_HIGHLIGHT_RANGE");
    uint32_t shard = start_row;
    while (shard < end_row) {
      requested_shards.push_back(shard);
      const uint32_t next = highlight_shard_end(shard);
      if (next <= shard)
        break;
      shard = next;
    }
  } else {
    start_row = requested_shards.front();
    end_row = highlight_shard_end(requested_shards.back());
  }
  if (requested_shards.size() > HIGHLIGHT_MAX_REQUEST_SHARDS)
    return throw_type_error(env,
                            "Highlight coverage request exceeds the shard window limit",
                            "ERR_INVALID_HIGHLIGHT_RANGE");

  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
  Napi::Promise promise = deferred.Promise();
  std::shared_ptr<HighlightCoverageRequest> superseded;
  std::shared_ptr<HighlightCoverageRequest> request;
  HighlightIndexTags tags;
  bool start_now = false;
  bool cache_hit = false;
  bool fallback_sync = false;
  bool exclusive_cohort = false;
  bool cleared_staged = false;
  const char *immediate_reason = nullptr;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->destroyed)
      return throw_error(env, "Document session was destroyed",
                         "ERR_DOCUMENT_SESSION_DESTROYED");
    const InjectionEngineDiagnostics injections =
        state->injection_engine->diagnostics();
    tags = HighlightIndexTags{state->buffer_revision, state->syntax_revision,
                              state->language_generation,
                              injections.topology_generation,
                              context_generation};
    state->counters.highlight_requests++;
    if (state->syntax_revision == 0 ||
        state->syntax_revision != state->buffer_revision ||
        !state->published_syntax || !state->syntax_analysis ||
        !state->syntax_lease) {
      immediate_reason = "syntax-pending";
    } else if (injections.active_layer_count > 0) {
      fallback_sync = true;
      immediate_reason = "injections-require-sync";
      state->counters.highlight_fallback_sync++;
      state->highlight_cancellation_signal.fetch_add(
          1, std::memory_order_relaxed);
      state->highlight_index->invalidate();
      generation = state->highlight_index->generation();
      cleared_staged = !state->staged_highlight_batches.empty();
      state->staged_highlight_batches.clear();
      for (auto iterator = state->display_views.begin();
           iterator != state->display_views.end();) {
        if (std::shared_ptr<DisplayViewState> view = iterator->lock()) {
          view->highlight_ranges.clear();
          view->highlight_buffer_revision = 0;
          view->highlight_syntax_revision = 0;
          view->display_revision++;
          iterator++;
        } else {
          iterator = state->display_views.erase(iterator);
        }
      }
      state->async_highlight_mode = false;
    } else {
      state->async_highlight_mode = true;
      context.root_grammar_id = state->language_id;
      for (size_t index = 1; index < requested_shards.size(); index++) {
        if (requested_shards[index] !=
            highlight_shard_end(requested_shards[index - 1])) {
          exclusive_cohort = true;
          break;
        }
      }
      if (exclusive_cohort) {
        const std::shared_ptr<const NativeQueryEngine> query_engine =
            state->published_syntax->query_engine();
        exclusive_cohort =
            query_engine &&
            query_engine->query_may_escape_node_range("highlightsQuery");
      }
      const bool was_active = state->highlight_index->active_for(tags);
      const uint64_t previous_generation =
          state->highlight_index->generation();
      state->highlight_index->prepare_request(tags, requested_shards,
                                              exclusive_cohort);
      if (!was_active ||
          state->highlight_index->generation() != previous_generation) {
        state->highlight_cancellation_signal.fetch_add(
            1, std::memory_order_relaxed);
        cleared_staged = !state->staged_highlight_batches.empty();
        state->staged_highlight_batches.clear();
      }
      generation = state->highlight_index->generation();
      if (state->highlight_index->covers_shards(requested_shards, tags)) {
        cache_hit = true;
        state->counters.highlight_cache_hits++;
      } else {
        state->counters.highlight_cache_misses++;
        const uint64_t current_cancellation =
            state->highlight_cancellation_signal.load(
                std::memory_order_relaxed);
        if (state->active_highlight_request &&
            state->active_highlight_request->same_request(tags, start_row,
                                                          end_row,
                                                          requested_shards,
                                                          current_cancellation)) {
          state->active_highlight_request->waiters.push_back(deferred);
          state->counters.highlight_requests_coalesced++;
          return promise;
        }
        if (state->pending_highlight_request &&
            state->pending_highlight_request->same_request(tags, start_row,
                                                           end_row,
                                                           requested_shards,
                                                           current_cancellation)) {
          state->pending_highlight_request->waiters.push_back(deferred);
          state->counters.highlight_requests_coalesced++;
          return promise;
        }
        request = std::make_shared<HighlightCoverageRequest>(
            deferred, state->next_highlight_request_id++, tags, start_row,
            end_row, std::move(context), exclusive_cohort);
        request->root = state->published_syntax;
        request->analysis = state->syntax_analysis;
        request->requested_shards = requested_shards;
        if (exclusive_cohort) {
          request->missing_shards = requested_shards;
        } else {
          for (uint32_t shard : requested_shards) {
            if (!state->highlight_index->covers_shards({shard}, tags))
              request->missing_shards.push_back(shard);
          }
        }
        request->cancellation_generation = current_cancellation;
        const SuperstringSnapshotLeaseStatus retained =
            state->syntax_lease->functions->retain(
                state->syntax_lease->context);
        if (retained != SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK) {
          immediate_reason = "snapshot-retain-failed";
          request.reset();
        } else {
          request->lease = state->syntax_lease;
          if (!state->active_highlight_request) {
            start_now = true;
          } else {
            superseded = std::move(state->pending_highlight_request);
            state->pending_highlight_request = request;
            if (superseded)
              state->counters.highlight_requests_superseded++;
          }
        }
      }
    }
  }

  if (superseded)
    resolve_superseded(env, superseded, "superseded");
  if (cache_hit) {
    deferred.Resolve(response(env, true, "", tags, 0, start_row, end_row,
                              generation, true));
  } else if (immediate_reason != nullptr) {
    deferred.Resolve(response(env, false, immediate_reason, tags, 0,
                              start_row, end_row, generation, false,
                              fallback_sync));
    if (cleared_staged)
      resolve_session_drains_if_idle(env, state);
  } else if (start_now) {
    queue_highlight_request(env, state, request);
  }
  return promise;
}

Napi::Value commit_highlight_coverage(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &state) {
  Napi::Env env = info.Env();
  uint64_t request_id = 0;
  if (info.Length() < 2 || !read_uint64(info[0], &request_id) ||
      !info[1].IsTypedArray() ||
      info[1].As<Napi::TypedArray>().TypedArrayType() != napi_uint32_array)
    return throw_type_error(
        env,
        "commitHighlightCoverage expects a request id and Uint32Array scope ids",
        "ERR_INVALID_HIGHLIGHT_COMMIT");
  Napi::Uint32Array packed = info[1].As<Napi::Uint32Array>();
  std::shared_ptr<HighlightCandidateBatch> candidate;
  bool accepted = false;
  bool capacity_fallback = false;
  uint64_t generation = 0;
  const auto started_at = HighlightClock::now();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->staged_highlight_batches.find(request_id);
    if (found != state->staged_highlight_batches.end()) {
      candidate = found->second;
      state->staged_highlight_batches.erase(found);
    }
    const bool candidate_current =
        candidate && tags_current_locked(*state, candidate->tags) &&
        packed.ElementLength() == candidate->capture_names.size();
    if (candidate_current) {
      std::vector<uint32_t> scope_ids(
          packed.Data(), packed.Data() + packed.ElementLength());
      accepted = state->highlight_index->commit(*candidate, scope_ids);
      if (accepted) {
        state->counters.highlight_shards_published +=
            candidate->partition_by_final_range
                ? candidate->requested_shards.size()
                : candidate->shards.size();
        generation = state->highlight_index->generation();
      } else {
        capacity_fallback = true;
        state->counters.highlight_fail_open++;
        state->highlight_cancellation_signal.fetch_add(
            1, std::memory_order_relaxed);
        state->highlight_index->invalidate();
        state->highlight_index->activate(candidate->tags);
        state->staged_highlight_batches.clear();
        state->async_highlight_mode = true;
        generation = state->highlight_index->generation();
        for (auto iterator = state->display_views.begin();
             iterator != state->display_views.end();) {
          if (std::shared_ptr<DisplayViewState> view = iterator->lock()) {
            view->highlight_ranges.clear();
            view->highlight_buffer_revision = 0;
            view->highlight_syntax_revision = 0;
            view->display_revision++;
            iterator++;
          } else {
            iterator = state->display_views.erase(iterator);
          }
        }
      }
    }
    if (!accepted)
      state->counters.highlight_stale_results++;
    state->highlight_commit_milliseconds +=
        std::chrono::duration<double, std::milli>(HighlightClock::now() -
                                                  started_at)
            .count();
  }
  HighlightIndexTags tags = candidate ? candidate->tags : HighlightIndexTags{};
  Napi::Object result = response(
      env, accepted,
      accepted ? "" : capacity_fallback ? "highlight-capacity"
                                        : "stale-commit",
      tags, request_id,
      candidate ? candidate->coverage_start_row : 0,
      candidate ? candidate->coverage_end_row : 0, generation, accepted,
      false);
  if (capacity_fallback)
    result.Set("permanentIncomplete", Napi::Boolean::New(env, true));
  resolve_session_drains_if_idle(env, state);
  return result;
}

Napi::Value abort_highlight_coverage(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &state) {
  Napi::Env env = info.Env();
  uint64_t request_id = 0;
  if (info.Length() == 0 || !read_uint64(info[0], &request_id))
    return throw_type_error(env,
                            "abortHighlightCoverage expects a request id",
                            "ERR_INVALID_HIGHLIGHT_COMMIT");
  bool removed = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    removed = state->staged_highlight_batches.erase(request_id) > 0;
  }
  resolve_session_drains_if_idle(env, state);
  return Napi::Boolean::New(env, removed);
}

Napi::Value invalidate_highlight_index(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &state) {
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->destroyed)
      return info.Env().Undefined();
    state->highlight_cancellation_signal.fetch_add(
        1, std::memory_order_relaxed);
    state->highlight_index->invalidate();
    state->staged_highlight_batches.clear();
    state->async_highlight_mode = true;
    for (auto iterator = state->display_views.begin();
         iterator != state->display_views.end();) {
      if (std::shared_ptr<DisplayViewState> view = iterator->lock()) {
        view->highlight_ranges.clear();
        view->highlight_buffer_revision = 0;
        view->highlight_syntax_revision = 0;
        view->display_revision++;
        iterator++;
      } else {
        iterator = state->display_views.erase(iterator);
      }
    }
    generation = state->highlight_index->generation();
  }
  resolve_session_drains_if_idle(info.Env(), state);
  return Napi::Number::New(info.Env(), generation);
}

Napi::Value use_synchronous_highlights(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &state) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->destroyed)
      return info.Env().Undefined();
    state->highlight_cancellation_signal.fetch_add(
        1, std::memory_order_relaxed);
    state->highlight_index->invalidate();
    state->staged_highlight_batches.clear();
    state->async_highlight_mode = false;
    for (auto iterator = state->display_views.begin();
         iterator != state->display_views.end();) {
      if (std::shared_ptr<DisplayViewState> view = iterator->lock()) {
        view->highlight_ranges.clear();
        view->highlight_buffer_revision = 0;
        view->highlight_syntax_revision = 0;
        view->display_revision++;
        iterator++;
      } else {
        iterator = state->display_views.erase(iterator);
      }
    }
  }
  resolve_session_drains_if_idle(info.Env(), state);
  return info.Env().Undefined();
}

void cancel_highlight_requests(
    Napi::Env env, const std::shared_ptr<SessionState> &state,
    const char *reason) {
  std::shared_ptr<HighlightCoverageRequest> active;
  std::shared_ptr<HighlightCoverageRequest> pending;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    active = state->active_highlight_request;
    pending = std::move(state->pending_highlight_request);
    state->staged_highlight_batches.clear();
  }
  resolve_superseded(env, active, reason);
  resolve_superseded(env, pending, reason);
}

} // namespace document_engine
