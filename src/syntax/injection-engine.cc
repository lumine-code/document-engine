#include "syntax/injection-engine.h"

#include "bindings/addon-data.h"
#include "bindings/document-session.h"
#include "snapshot-lease.h"
#include "revision-projection.h"
#include "syntax/query-engine.h"
#include "syntax/syntax-backend.h"
#include "text-bridge/snapshot-reader.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace document_engine {

std::atomic<uint64_t> next_snapshot_lease_reference_id{1};
std::atomic<uint64_t> next_published_syntax_identity{1};

namespace {

using InjectionClock = std::chrono::steady_clock;

double elapsed_milliseconds(InjectionClock::time_point started_at) {
  return std::chrono::duration<double, std::milli>(InjectionClock::now() -
                                                   started_at)
      .count();
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
  if (!std::isfinite(number) || number < 0 || number > 9007199254740991.0)
    return false;
  *result = static_cast<uint64_t>(number);
  return static_cast<double>(*result) == number;
}

bool read_uint32(Napi::Value value, uint32_t *result) {
  uint64_t wide = 0;
  if (!read_uint64(value, &wide) || wide > std::numeric_limits<uint32_t>::max())
    return false;
  *result = static_cast<uint32_t>(wide);
  return true;
}

bool read_tags(Napi::Object object, InjectionRevisionTags *tags) {
  return read_uint64(object.Get("bufferRevision"), &tags->buffer_revision) &&
         read_uint64(object.Get("syntaxRevision"), &tags->syntax_revision) &&
         read_uint64(object.Get("languageGeneration"),
                     &tags->language_generation);
}

bool read_point_value(Napi::Value value, Point *point) {
  if (value.IsArray()) {
    Napi::Array array = value.As<Napi::Array>();
    return array.Length() >= 2 &&
           read_uint64(array.Get(static_cast<uint32_t>(0)), &point->row) &&
           read_uint64(array.Get(static_cast<uint32_t>(1)), &point->column);
  }
  if (!value.IsObject())
    return false;
  Napi::Object object = value.As<Napi::Object>();
  return read_uint64(object.Get("row"), &point->row) &&
         read_uint64(object.Get("column"), &point->column);
}

bool read_range_value(Napi::Value value, Range *range) {
  if (value.IsArray()) {
    Napi::Array array = value.As<Napi::Array>();
    return array.Length() >= 2 &&
           read_point_value(array.Get(static_cast<uint32_t>(0)),
                            &range->start) &&
           read_point_value(array.Get(static_cast<uint32_t>(1)), &range->end);
  }
  if (!value.IsObject())
    return false;
  Napi::Object object = value.As<Napi::Object>();
  return read_point_value(object.Get("start"), &range->start) &&
         read_point_value(object.Get("end"), &range->end);
}

bool tags_equal(const InjectionRevisionTags &left,
                const InjectionRevisionTags &right) {
  return left.buffer_revision == right.buffer_revision &&
         left.syntax_revision == right.syntax_revision &&
         left.language_generation == right.language_generation;
}

void set_tags(Napi::Object object, const InjectionRevisionTags &tags) {
  object.Set("bufferRevision",
             Napi::Number::New(object.Env(), tags.buffer_revision));
  object.Set("syntaxRevision",
             Napi::Number::New(object.Env(), tags.syntax_revision));
  object.Set("languageGeneration",
             Napi::Number::New(object.Env(), tags.language_generation));
}

Napi::Object point_object(Napi::Env env, uint64_t row, uint64_t column) {
  Napi::Object point = Napi::Object::New(env);
  point.Set("row", Napi::Number::New(env, static_cast<double>(row)));
  point.Set("column", Napi::Number::New(env, static_cast<double>(column)));
  return point;
}

Point point_from_ts(TSPoint point) {
  return Point{point.row, point.column / UINT64_C(2)};
}

uint64_t start_index_for(TSNode node) {
  return ts_node_start_byte(node) / UINT64_C(2);
}

uint64_t end_index_for(TSNode node) {
  return ts_node_end_byte(node) / UINT64_C(2);
}

struct NodeKey {
  std::array<uint32_t, 4> context{};
  const void *id = nullptr;

  bool operator==(const NodeKey &other) const {
    return id == other.id && context == other.context;
  }
};

struct NodeKeyHash {
  size_t operator()(const NodeKey &key) const {
    size_t value = std::hash<const void *>{}(key.id);
    for (uint32_t part : key.context) {
      value ^= std::hash<uint32_t>{}(part) + static_cast<size_t>(0x9e3779b9u) +
               (value << 6u) + (value >> 2u);
    }
    return value;
  }
};

NodeKey node_key(TSNode node) {
  return NodeKey{{node.context[0], node.context[1], node.context[2],
                  node.context[3]},
                 node.id};
}

struct RangeSymbolKey {
  uint32_t start_byte = 0;
  uint32_t end_byte = 0;
  uint32_t symbol = 0;

  bool operator==(const RangeSymbolKey &other) const {
    return start_byte == other.start_byte && end_byte == other.end_byte &&
           symbol == other.symbol;
  }
};

struct RangeSymbolKeyHash {
  size_t operator()(const RangeSymbolKey &key) const {
    size_t value = key.start_byte;
    value ^= static_cast<size_t>(key.end_byte) + (value << 6u) +
             (value >> 2u);
    value ^= static_cast<size_t>(key.symbol) + (value << 6u) +
             (value >> 2u);
    return value;
  }
};

Napi::Object range_object(Napi::Env env, TSNode node) {
  Napi::Object range = Napi::Object::New(env);
  const Point start = point_from_ts(ts_node_start_point(node));
  const Point end = point_from_ts(ts_node_end_point(node));
  range.Set("start", point_object(env, start.row, start.column));
  range.Set("end", point_object(env, end.row, end.column));
  return range;
}

std::string utf16_to_utf8(std::u16string_view input) {
  std::string output;
  output.reserve(input.size());
  for (size_t index = 0; index < input.size(); index++) {
    uint32_t code_point = input[index];
    if (code_point >= 0xd800u && code_point <= 0xdbffu &&
        index + 1 < input.size()) {
      const uint32_t low = input[index + 1];
      if (low >= 0xdc00u && low <= 0xdfffu) {
        code_point = UINT32_C(0x10000) +
                     ((code_point - UINT32_C(0xd800)) << 10u) +
                     (low - UINT32_C(0xdc00));
        index++;
      }
    }
    if (code_point <= 0x7fu) {
      output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ffu) {
      output.push_back(static_cast<char>(0xc0u | (code_point >> 6u)));
      output.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
    } else if (code_point <= 0xffffu) {
      output.push_back(static_cast<char>(0xe0u | (code_point >> 12u)));
      output.push_back(
          static_cast<char>(0x80u | ((code_point >> 6u) & 0x3fu)));
      output.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
    } else {
      output.push_back(static_cast<char>(0xf0u | (code_point >> 18u)));
      output.push_back(
          static_cast<char>(0x80u | ((code_point >> 12u) & 0x3fu)));
      output.push_back(
          static_cast<char>(0x80u | ((code_point >> 6u) & 0x3fu)));
      output.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
    }
  }
  return output;
}

std::string trim_ascii(std::string value) {
  const auto whitespace = [](unsigned char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n' || character == '\f' || character == '\v';
  };
  auto first = std::find_if_not(value.begin(), value.end(), whitespace);
  auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
  if (first >= last)
    return {};
  return std::string(first, last);
}

} // namespace

class SnapshotLeaseReference {
public:
  static std::shared_ptr<SnapshotLeaseReference>
  acquire(const SuperstringSnapshotLease *lease) {
    if (lease == nullptr || lease->functions == nullptr ||
        lease->functions->retain(lease->context) !=
            SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK)
      return nullptr;
    return std::shared_ptr<SnapshotLeaseReference>(
        new SnapshotLeaseReference(lease));
  }

  ~SnapshotLeaseReference() {
    // Node-API finalizers run on the environment owner thread. Do not consume
    // the lease if an embedding violates that contract; a wrong-thread release
    // is rejected by Superstring and keeping the memory pinned is safer than a
    // cross-thread free.
    release();
  }

  const SuperstringSnapshotLease *lease() const { return lease_; }
  uint64_t id() const { return id_; }

  void release() {
    if (lease_ == nullptr || std::this_thread::get_id() != owner_thread_)
      return;
    const SuperstringSnapshotLease *lease = lease_;
    lease_ = nullptr;
    lease->functions->release(lease->context);
  }

private:
  explicit SnapshotLeaseReference(const SuperstringSnapshotLease *lease)
      : lease_(lease), owner_thread_(std::this_thread::get_id()),
        id_(next_snapshot_lease_reference_id.fetch_add(
            1, std::memory_order_relaxed)) {}

  const SuperstringSnapshotLease *lease_ = nullptr;
  std::thread::id owner_thread_;
  uint64_t id_ = 0;
};

void release_snapshot_lease_reference(
    const std::shared_ptr<SnapshotLeaseReference> &lease) {
  if (lease)
    lease->release();
}

namespace {

struct NodeRangeSpec {
  uint64_t start_index = 0;
  uint64_t end_index = 0;
  Point start;
  Point end;
  std::vector<NodeRangeSpec> children;
};

struct QueryInjectionProposal {
  uint32_t candidate_id = 0;
  uint32_t pattern_index = UINT32_MAX;
  std::string parent_grammar_id;
  std::string language_name;
  bool target_self = false;
  bool target_parent = false;
  bool include_children = false;
  bool combined = false;
  bool include_language_scope = true;
  std::vector<NodeRangeSpec> content;
};

struct InjectionCandidateRecord {
  uint32_t candidate_id = 0;
  std::shared_ptr<const PublishedSyntaxSnapshot> syntax;
  uint32_t node_handle = 0;
  std::string grammar_id;
  std::string type;
  uint64_t parent_layer_id = 0;
  uint64_t parent_reuse_slot_id = 0;
  uint32_t depth = 0;
};

struct PendingScopeTarget {
  size_t layer_index = 0;
};

struct ProjectedInjectionLayer {
  const InjectionLayerRecord *source = nullptr;
  std::vector<InjectionRangeRecord> ranges;
  bool projectable = false;
};

struct InjectionReuseState {
  InjectionRevisionTags target_tags;
  std::shared_ptr<const InjectionRangeIndex> previous_index;
  std::vector<RevisionEditBatch> projection;
  std::vector<SyntaxEdit> syntax_edits;
  std::vector<ProjectedInjectionLayer> layers;
  bool eligible = false;
  bool generation_compatible = true;
  bool same_extent_edits = false;
};

struct InjectionSourceToken {
  uint64_t reuse_slot_id = 0;
  uint64_t syntax_identity = 0;
  uint64_t registration_fingerprint = 0;

  bool operator<(const InjectionSourceToken &other) const {
    return std::tie(reuse_slot_id, syntax_identity,
                    registration_fingerprint) <
           std::tie(other.reuse_slot_id, other.syntax_identity,
                    other.registration_fingerprint);
  }
};

std::string source_token_fingerprint(
    const std::set<InjectionSourceToken> &tokens) {
  constexpr uint64_t offset_basis = UINT64_C(14695981039346656037);
  constexpr uint64_t prime = UINT64_C(1099511628211);
  uint64_t fingerprint = offset_basis;
  for (const InjectionSourceToken &token : tokens) {
    const uint64_t values[] = {token.reuse_slot_id, token.syntax_identity,
                               token.registration_fingerprint};
    for (uint64_t value : values) {
      for (uint32_t byte = 0; byte < sizeof(value); byte++) {
        fingerprint ^= static_cast<unsigned char>(value >> (byte * 8));
        fingerprint *= prime;
      }
    }
  }
  return std::to_string(fingerprint);
}

struct InjectionRequestState {
  uint64_t request_id = 0;
  uint64_t candidate_epoch = 0;
  uint32_t rescan_wave = 0;
  InjectionRevisionTags tags;
  uint64_t injection_point_generation = 0;
  uint32_t next_batch_index = 0;
  bool final_received = false;
  bool awaiting_language_scopes = false;
  bool child_parse_active = false;
  std::shared_ptr<InjectionReuseState> reuse;
  std::unordered_map<uint32_t, InjectionCandidateRecord> candidates;
  std::unordered_set<uint32_t> valid_candidate_ids;
  std::shared_ptr<InjectionRangeIndex> staged_index;
  std::map<std::string, uint64_t> registration_fingerprints;
  std::set<InjectionSourceToken> scanned_source_tokens;
  std::map<std::pair<uint32_t, uint32_t>, PendingScopeTarget> scope_targets;
};

struct GrammarManifestEntry {
  std::string grammar_id;
  std::vector<std::string> types;
  uint64_t registration_fingerprint = 0;
};

struct CandidateScanItem {
  std::shared_ptr<const PublishedSyntaxSnapshot> syntax;
  std::string grammar_id;
  std::string type;
  uint32_t node_handle = 0;
  uint64_t parent_layer_id = 0;
  uint64_t parent_reuse_slot_id = 0;
  uint32_t depth = 0;
  uint32_t start_byte = 0;
  uint32_t end_byte = 0;
};

struct CandidateSource {
  std::shared_ptr<const PublishedSyntaxSnapshot> syntax;
  std::string grammar_id;
  uint64_t layer_id = 0;
  uint64_t reuse_slot_id = 0;
  uint32_t depth = 0;
};

constexpr uint32_t MAX_INJECTION_RESCAN_WAVES = 32;

struct ParsedGrammarDescriptor {
  std::string language_id;
  std::string runtime;
  std::string wasm_path;
  std::string language_name;
  std::string language_segment;
  std::map<std::string, std::vector<std::string>> query_paths;
};

bool current_tags_locked(const SessionState &state,
                         const InjectionRevisionTags &tags);
Napi::Object stale_result(Napi::Env env, const InjectionRevisionTags &tags,
                          const char *reason, uint64_t request_id = 0);
Napi::Value stale_node(Napi::Env env);
std::vector<QueryInjectionProposal> query_injection_proposals(
    const std::shared_ptr<const PublishedSyntaxSnapshot> &syntax,
    const std::shared_ptr<const SyntaxQuerySnapshot> &queries,
    const SnapshotReader &reader, const std::string &grammar_id);
Napi::Object node_spec_object(Napi::Env env, const NodeRangeSpec &spec);
bool parse_manifest(Napi::Env env, Napi::Value value,
                    std::vector<GrammarManifestEntry> *manifest);
bool parse_grammar_descriptor(Napi::Env env, Napi::Value value,
                              ParsedGrammarDescriptor *descriptor);
bool parse_content_spec(Napi::Env env, Napi::Value value,
                        const SnapshotAnalysis &analysis,
                        NodeRangeSpec *spec);
std::vector<InjectionRangeRecord>
included_ranges(const std::vector<NodeRangeSpec> &content,
                bool include_children, const SnapshotAnalysis &analysis);
void merge_adjacent_whitespace(std::vector<InjectionRangeRecord> &ranges,
                               const SnapshotReader &reader,
                               const SnapshotAnalysis &analysis);
std::vector<std::string> parse_scopes(Napi::Value value);
void finalize_index_counts(InjectionRangeIndex &index);
bool query_layer_is_resolved(const InjectionLayerRecord &layer);
std::shared_ptr<InjectionReuseState> build_reuse_state(
    const std::shared_ptr<const InjectionRangeIndex> &previous,
    const std::vector<RevisionEditBatch> &projection,
    bool root_incremental, const InjectionRevisionTags &target_tags,
    const SnapshotAnalysis &analysis, bool same_root_grammar,
    bool injection_workers_idle);

} // namespace

struct InjectionEngine::Impl {
  std::shared_ptr<const PublishedSyntaxSnapshot> root_syntax;
  std::shared_ptr<const SyntaxQuerySnapshot> root_queries;
  SnapshotAnalysis analysis;
  std::string root_grammar_id;
  std::vector<QueryInjectionProposal> query_proposals;
  std::unordered_map<uint64_t, InjectionRequestState> requests;
  std::shared_ptr<const InjectionRangeIndex> published_index;
  std::shared_ptr<InjectionReuseState> reuse_state;
  uint64_t next_request_id = 1;
  uint64_t candidate_epoch = 0;
  uint64_t next_layer_id = 1;
  uint64_t next_reuse_slot_id = 1;
  uint64_t candidate_count = 0;
  uint64_t stale_request_count = 0;
  uint64_t aborted_request_count = 0;
  uint64_t published_generation = 0;
  uint64_t topology_generation = 0;
  uint64_t query_language_resolution_count = 0;
  uint64_t query_language_rejection_count = 0;
  uint64_t reused_layer_count = 0;
  uint64_t projected_range_count = 0;
  uint64_t child_incremental_parse_count = 0;
  uint64_t child_full_parse_count = 0;
  uint64_t reuse_fallback_count = 0;
  // Keep the last finalized layer count across publish_root's query-only
  // staging index so the bridge can distinguish stable zero from a removal.
  uint64_t published_layer_count = 0;
  std::unordered_map<uint64_t, std::shared_ptr<SyntaxBackend>> child_backends;
  std::unordered_map<std::string, ParsedGrammarDescriptor> query_grammars;
  std::unordered_set<std::string> query_rejected_aliases;
  std::set<InjectionSourceToken> seen_source_tokens;
};

namespace {

struct CandidateCancellation {
  const SessionState *state = nullptr;
  InjectionRevisionTags tags;
};

bool candidate_scan_cancelled(void *payload) {
  if (payload == nullptr)
    return true;
  const auto *cancellation = static_cast<const CandidateCancellation *>(payload);
  return cancellation->state == nullptr ||
         cancellation->state->destroyed_signal.load(std::memory_order_relaxed) ||
         cancellation->state->latest_requested_signal.load(
             std::memory_order_relaxed) != cancellation->tags.buffer_revision ||
         cancellation->state->language_generation_signal.load(
             std::memory_order_relaxed) !=
             cancellation->tags.language_generation;
}

} // namespace

class InjectionCandidateWorker final : public Napi::AsyncWorker {
public:
  InjectionCandidateWorker(
      Napi::Env env, std::shared_ptr<SessionState> session,
      InjectionEngine *engine,
      std::vector<CandidateSource> sources,
      std::vector<GrammarManifestEntry> manifest,
      std::set<InjectionSourceToken> seen_source_tokens,
      InjectionRevisionTags tags, uint64_t injection_point_generation,
      uint32_t rescan_wave, uint64_t candidate_epoch,
      std::shared_ptr<NativeJobControl> job)
      : Napi::AsyncWorker(env, "DocumentSession.getInjectionCandidates"),
        deferred_(Napi::Promise::Deferred::New(env)),
        session_(std::move(session)), engine_(engine),
        sources_(std::move(sources)), manifest_(std::move(manifest)),
        seen_source_tokens_(std::move(seen_source_tokens)), tags_(tags),
        injection_point_generation_(injection_point_generation),
        rescan_wave_(rescan_wave), candidate_epoch_(candidate_epoch),
        cancellation_{session_.get(), tags}, job_(std::move(job)),
        queued_at_(InjectionClock::now()) {}

  Napi::Promise promise() const { return deferred_.Promise(); }

  void OnExecute(Napi::Env env) override {
    Napi::AsyncWorker::OnExecute(env);
    job_->mark_worker_finished();
    if (session_->addon_data != nullptr)
      session_->addon_data->notify_cleanup_progress();
  }

  void Execute() override {
    const auto started_at = InjectionClock::now();
    queue_milliseconds_ = std::chrono::duration<double, std::milli>(
                              started_at - queued_at_)
                              .count();
    for (const CandidateSource &source : sources_) {
      std::set<std::string> requested_types;
      uint64_t registration_fingerprint = 0;
      for (const GrammarManifestEntry &entry : manifest_) {
        if (entry.grammar_id == source.grammar_id) {
          requested_types.insert(entry.types.begin(), entry.types.end());
          registration_fingerprint = entry.registration_fingerprint;
        }
      }
      if (requested_types.empty())
        continue;
      const InjectionSourceToken token{
          source.reuse_slot_id, source.syntax->identity(),
          registration_fingerprint};
      if (seen_source_tokens_.contains(token))
        continue;
      scanned_source_tokens_.insert(token);
      std::map<std::string, std::vector<uint32_t>> handles;
      if (!source.syntax->collect_handles_for_types(
              requested_types, handles, candidate_scan_cancelled,
              &cancellation_)) {
        cancelled_ = true;
        scan_milliseconds_ = elapsed_milliseconds(started_at);
        return;
      }
      for (const std::string &type : requested_types) {
        auto found = handles.find(type);
        if (found == handles.end())
          continue;
        for (uint32_t handle : found->second) {
          const TSNode node = source.syntax->node(handle);
          items_.push_back(CandidateScanItem{source.syntax, source.grammar_id,
                                              type, handle, source.layer_id,
                                              source.reuse_slot_id,
                                              source.depth,
                                              ts_node_start_byte(node),
                                              ts_node_end_byte(node)});
        }
      }
    }
    std::sort(items_.begin(), items_.end(), [](const CandidateScanItem &left,
                                               const CandidateScanItem &right) {
      return std::tie(left.depth, left.start_byte, left.end_byte, left.type) <
             std::tie(right.depth, right.start_byte, right.end_byte,
                      right.type);
    });
    scan_milliseconds_ = elapsed_milliseconds(started_at);
  }

  void OnWorkComplete(Napi::Env env, napi_status status) override {
    if (status == napi_cancelled ||
        session_->environment_cleanup_signal.load(std::memory_order_acquire)) {
      finish_injection_job_without_js(session_);
      Napi::AsyncWorker::OnWorkComplete(env, napi_cancelled);
      return;
    }
    Napi::AsyncWorker::OnWorkComplete(env, status);
  }

  void OnOK() override {
    Napi::Env env = Env();
    const auto pack_started_at = InjectionClock::now();
    std::unique_lock<std::mutex> lock(session_->mutex);
    if (cancelled_ || !current_tags_locked(*session_, tags_) ||
        !engine_->owns_syntax_snapshot(session_->published_syntax, tags_) ||
        session_->injection_engine.get() != engine_) {
      if (engine_ != nullptr)
        engine_->impl_->stale_request_count++;
      deferred_.Resolve(stale_result(env, tags_, "stale-candidates"));
      lock.unlock();
      finish_injection_job(env, session_);
      return;
    }

    InjectionEngine::Impl &impl = *engine_->impl_;
    if (candidate_epoch_ != impl.candidate_epoch) {
      impl.stale_request_count++;
      deferred_.Resolve(stale_result(env, tags_, "stale-candidates"));
      lock.unlock();
      finish_injection_job(env, session_);
      return;
    }
    const uint64_t request_id = impl.next_request_id++;
    InjectionRequestState request;
    request.request_id = request_id;
    request.candidate_epoch = candidate_epoch_;
    request.rescan_wave = rescan_wave_;
    request.tags = tags_;
    request.injection_point_generation = injection_point_generation_;
    if (impl.reuse_state &&
        tags_equal(impl.reuse_state->target_tags, tags_)) {
      request.reuse = impl.reuse_state;
      if (request.reuse->previous_index &&
          request.reuse->previous_index->injection_point_generation !=
              injection_point_generation_) {
        request.reuse->generation_compatible = false;
      }
    }
    request.scanned_source_tokens = scanned_source_tokens_;
    for (const GrammarManifestEntry &entry : manifest_) {
      request.registration_fingerprints[entry.grammar_id] =
          entry.registration_fingerprint;
    }
    if (rescan_wave_ == 0)
      impl.seen_source_tokens.clear();
    const bool same_revision_wave =
        rescan_wave_ > 0 &&
        impl.published_index && tags_equal(impl.published_index->tags, tags_) &&
        impl.published_index->injection_point_generation ==
            injection_point_generation_ &&
        !impl.seen_source_tokens.empty();
    impl.seen_source_tokens.insert(scanned_source_tokens_.begin(),
                                   scanned_source_tokens_.end());
    request.staged_index = std::make_shared<InjectionRangeIndex>();
    if (impl.published_index) {
      for (const InjectionLayerRecord &layer : impl.published_index->layers) {
        if (same_revision_wave || layer.query_defined)
          request.staged_index->layers.push_back(layer);
      }
      finalize_index_counts(*request.staged_index);
    }
    request.staged_index->tags = tags_;
    request.staged_index->injection_point_generation =
        injection_point_generation_;

    std::vector<std::string> grammar_ids;
    std::vector<std::string> kinds;
    std::unordered_map<std::string, uint32_t> grammar_indices;
    std::unordered_map<std::string, uint32_t> kind_indices;
    auto id_for = [](const std::string &value, std::vector<std::string> &values,
                     std::unordered_map<std::string, uint32_t> &indices) {
      auto found = indices.find(value);
      if (found != indices.end())
        return found->second;
      const uint32_t result = static_cast<uint32_t>(values.size());
      values.push_back(value);
      indices.emplace(value, result);
      return result;
    };

    uint32_t next_candidate_id = 1;
    Napi::Uint32Array packed =
        Napi::Uint32Array::New(env, items_.size() * 4);
    uint32_t offset = 0;
    for (const CandidateScanItem &item : items_) {
      const uint32_t candidate_id = next_candidate_id++;
      const uint32_t grammar_index =
          id_for(item.grammar_id, grammar_ids, grammar_indices);
      const uint32_t kind_index = id_for(item.type, kinds, kind_indices);
      packed[offset++] = candidate_id;
      packed[offset++] = grammar_index;
      packed[offset++] = kind_index;
      packed[offset++] = item.node_handle;
      InjectionCandidateRecord record{candidate_id,
                                      item.syntax,
                                      item.node_handle,
                                      item.grammar_id,
                                      item.type,
                                      item.parent_layer_id,
                                      item.parent_reuse_slot_id,
                                      item.depth};
      request.candidates.emplace(candidate_id, std::move(record));
      request.valid_candidate_ids.insert(candidate_id);
    }

    impl.candidate_count = request.valid_candidate_ids.size();
    const uint64_t query_layer_count = request.staged_index->query_layer_count;
    impl.requests.emplace(request_id, std::move(request));

    Napi::Object response = Napi::Object::New(env);
    response.Set("accepted", Napi::Boolean::New(env, true));
    set_tags(response, tags_);
    response.Set("requestId", Napi::Number::New(env, request_id));
    response.Set("injectionPointGeneration",
                 Napi::Number::New(env, injection_point_generation_));
    response.Set("queryLayerCount",
                 Napi::Number::New(env, query_layer_count));
    response.Set("candidateQueueMilliseconds",
                 Napi::Number::New(env, queue_milliseconds_));
    response.Set("candidateScanMilliseconds",
                 Napi::Number::New(env, scan_milliseconds_));
    response.Set("candidateStride", Napi::Number::New(env, 4));
    response.Set("candidates", packed);
    Napi::Array grammar_array = Napi::Array::New(env, grammar_ids.size());
    for (uint32_t index = 0; index < grammar_ids.size(); index++)
      grammar_array.Set(index, Napi::String::New(env, grammar_ids[index]));
    response.Set("grammarIds", grammar_array);
    Napi::Array kind_array = Napi::Array::New(env, kinds.size());
    for (uint32_t index = 0; index < kinds.size(); index++)
      kind_array.Set(index, Napi::String::New(env, kinds[index]));
    response.Set("kinds", kind_array);
    std::set<std::string> unresolved_query_languages;
    const auto stored_request = impl.requests.find(request_id);
    if (stored_request != impl.requests.end() &&
        stored_request->second.staged_index) {
      for (const InjectionLayerRecord &layer :
           stored_request->second.staged_index->layers) {
        if (layer.query_defined && !query_layer_is_resolved(layer) &&
            !layer.language_name.empty() &&
            !impl.query_rejected_aliases.contains(layer.language_name))
          unresolved_query_languages.insert(layer.language_name);
      }
    }
    Napi::Array unresolved =
        Napi::Array::New(env, unresolved_query_languages.size());
    uint32_t unresolved_index = 0;
    for (const std::string &language : unresolved_query_languages)
      unresolved.Set(unresolved_index++, Napi::String::New(env, language));
    response.Set("unresolvedQueryLanguages", unresolved);
    response.Set("candidatePackMilliseconds",
                 Napi::Number::New(
                     env, elapsed_milliseconds(pack_started_at)));
    response.Set("sourceTokenFingerprint",
                 Napi::String::New(
                     env, source_token_fingerprint(impl.seen_source_tokens)));
    response.Set("scannedSourceCount",
                 Napi::Number::New(env, scanned_source_tokens_.size()));
    deferred_.Resolve(response);
    lock.unlock();
    finish_injection_job(env, session_);
  }

  void OnError(const Napi::Error &error) override {
    Napi::Error output = Napi::Error::New(Env(), error.Message());
    set_error_code(output, "ERR_INJECTION_CANDIDATE_SCAN");
    deferred_.Reject(output.Value());
    finish_injection_job(Env(), session_);
  }

private:
  Napi::Promise::Deferred deferred_;
  std::shared_ptr<SessionState> session_;
  InjectionEngine *engine_ = nullptr;
  std::vector<CandidateSource> sources_;
  std::vector<GrammarManifestEntry> manifest_;
  std::set<InjectionSourceToken> seen_source_tokens_;
  std::set<InjectionSourceToken> scanned_source_tokens_;
  InjectionRevisionTags tags_;
  uint64_t injection_point_generation_ = 0;
  uint32_t rescan_wave_ = 0;
  uint64_t candidate_epoch_ = 0;
  std::vector<CandidateScanItem> items_;
  bool cancelled_ = false;
  CandidateCancellation cancellation_;
  std::shared_ptr<NativeJobControl> job_;
  InjectionClock::time_point queued_at_;
  double queue_milliseconds_ = 0;
  double scan_milliseconds_ = 0;
};

namespace {

void finalize_index_counts(InjectionRangeIndex &index) {
  index.dynamic_layer_count = 0;
  index.query_layer_count = 0;
  index.range_count = 0;
  index.parsed_child_layer_count = 0;
  index.failed_child_layer_count = 0;
  index.maximum_depth = 0;
  for (const InjectionLayerRecord &layer : index.layers) {
    if (layer.query_defined)
      index.query_layer_count++;
    else
      index.dynamic_layer_count++;
    index.range_count += layer.ranges.size();
    if (layer.syntax_parsed)
      index.parsed_child_layer_count++;
    if (!layer.syntax_error_code.empty())
      index.failed_child_layer_count++;
    index.maximum_depth = std::max<uint64_t>(index.maximum_depth, layer.depth);
  }
}

std::vector<SyntaxEdit>
syntax_edits_for_projection(const std::vector<RevisionEditBatch> &projection) {
  std::vector<SyntaxEdit> edits;
  for (const RevisionEditBatch &batch : projection) {
    edits.reserve(edits.size() + batch.edits.size() / 8);
    for (size_t index = 0; index + 7 < batch.edits.size(); index += 8) {
      edits.push_back(SyntaxEdit{
          Point{batch.edits[index], batch.edits[index + 1]},
          Point{batch.edits[index + 2], batch.edits[index + 3]},
          Point{batch.edits[index + 4], batch.edits[index + 5]},
          Point{batch.edits[index + 6], batch.edits[index + 7]}});
    }
  }
  return edits;
}

bool projection_has_same_extents(
    const std::vector<RevisionEditBatch> &projection) {
  for (const RevisionEditBatch &batch : projection) {
    if (!batch.before || !batch.after || batch.edits.size() % 8 != 0)
      return false;
    for (size_t index = 0; index < batch.edits.size(); index += 8) {
      const uint64_t old_start = analysis_offset_for_point(
          *batch.before,
          Point{batch.edits[index], batch.edits[index + 1]});
      const uint64_t old_end = analysis_offset_for_point(
          *batch.before,
          Point{batch.edits[index + 2], batch.edits[index + 3]});
      const uint64_t new_start = analysis_offset_for_point(
          *batch.after,
          Point{batch.edits[index + 4], batch.edits[index + 5]});
      const uint64_t new_end = analysis_offset_for_point(
          *batch.after,
          Point{batch.edits[index + 6], batch.edits[index + 7]});
      if (old_end - old_start != new_end - new_start)
        return false;
    }
  }
  return true;
}

bool continuous_projection(const InjectionRangeIndex &previous,
                           const std::vector<RevisionEditBatch> &projection,
                           const InjectionRevisionTags &target_tags) {
  if (previous.tags.buffer_revision + 1 != target_tags.buffer_revision ||
      previous.tags.syntax_revision != previous.tags.buffer_revision ||
      target_tags.syntax_revision != target_tags.buffer_revision)
    return false;
  if (projection.empty())
    return true;
  uint64_t revision = previous.tags.buffer_revision;
  for (const RevisionEditBatch &batch : projection) {
    if (batch.from_revision != revision ||
        batch.to_revision != batch.from_revision + 1 || !batch.before ||
        !batch.after || batch.edits.size() % 8 != 0)
      return false;
    revision = batch.to_revision;
  }
  return revision == target_tags.buffer_revision;
}

std::shared_ptr<InjectionReuseState> build_reuse_state(
    const std::shared_ptr<const InjectionRangeIndex> &previous,
    const std::vector<RevisionEditBatch> &projection,
    bool root_incremental, const InjectionRevisionTags &target_tags,
    const SnapshotAnalysis &analysis, bool same_root_grammar,
    bool injection_workers_idle) {
  if (!previous)
    return nullptr;
  auto state = std::make_shared<InjectionReuseState>();
  state->target_tags = target_tags;
  state->previous_index = previous;
  state->projection = projection;
  state->eligible =
      root_incremental && injection_workers_idle && same_root_grammar &&
      previous->tags.language_generation == target_tags.language_generation &&
      continuous_projection(*previous, projection, target_tags);
  if (!state->eligible)
    return state;
  state->syntax_edits = syntax_edits_for_projection(projection);
  state->same_extent_edits = projection_has_same_extents(projection);
  state->layers.reserve(previous->layers.size());
  for (const InjectionLayerRecord &layer : previous->layers) {
    ProjectedInjectionLayer projected;
    projected.source = &layer;
    projected.projectable = true;
    projected.ranges.reserve(layer.ranges.size());
    for (const InjectionRangeRecord &range : layer.ranges) {
      uint64_t start = range.start_index;
      uint64_t end = range.end_index;
      if (!project_offset_range(projection, &start, &end)) {
        projected.projectable = false;
        projected.ranges.clear();
        break;
      }
      projected.ranges.push_back(InjectionRangeRecord{
          start, end, analysis_point_for_offset(analysis, start),
          analysis_point_for_offset(analysis, end), range.scopes});
    }
    state->layers.push_back(std::move(projected));
  }
  return state;
}

bool same_projected_ranges(
    const std::vector<InjectionRangeRecord> &projected,
    const std::vector<InjectionRangeRecord> &current) {
  if (projected.size() != current.size())
    return false;
  for (size_t index = 0; index < projected.size(); index++) {
    const InjectionRangeRecord &left = projected[index];
    const InjectionRangeRecord &right = current[index];
    if (left.start_index != right.start_index ||
        left.end_index != right.end_index || left.start != right.start ||
        left.end != right.end || left.scopes != right.scopes)
      return false;
  }
  return true;
}

bool same_layer_configuration(const InjectionLayerRecord &previous,
                              const InjectionLayerRecord &current,
                              uint64_t current_parent_reuse_slot_id) {
  return previous.parent_reuse_slot_id == current_parent_reuse_slot_id &&
         previous.depth == current.depth &&
         previous.injection_point_id == current.injection_point_id &&
         previous.query_pattern_index == current.query_pattern_index &&
         previous.parent_grammar_id == current.parent_grammar_id &&
         previous.language_name == current.language_name &&
         previous.language_id == current.language_id &&
         previous.runtime == current.runtime &&
         previous.wasm_path == current.wasm_path &&
         previous.language_name_export == current.language_name_export &&
         previous.language_segment == current.language_segment &&
         previous.query_paths == current.query_paths &&
         previous.include_children == current.include_children &&
         previous.include_adjacent_whitespace ==
             current.include_adjacent_whitespace &&
         previous.newlines_between == current.newlines_between &&
         previous.cover_shallower_scopes == current.cover_shallower_scopes &&
         previous.query_defined == current.query_defined &&
         previous.include_language_scope == current.include_language_scope;
}

struct LayerReconciliation {
  bool fallback = false;
  uint64_t reused_layers = 0;
  uint64_t projected_ranges = 0;
  std::vector<SyntaxEdit> syntax_edits;
};

LayerReconciliation reconcile_layers(
    InjectionRangeIndex &index,
    const std::shared_ptr<InjectionReuseState> &reuse) {
  LayerReconciliation result;
  if (!reuse || !reuse->previous_index)
    return result;
  result.fallback = !reuse->previous_index->layers.empty() ||
                    !index.layers.empty();
  if (!reuse->eligible || !reuse->generation_compatible)
    return result;

  std::vector<size_t> order(index.layers.size());
  for (size_t index_value = 0; index_value < order.size(); index_value++)
    order[index_value] = index_value;
  std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right) {
    return index.layers[left].depth < index.layers[right].depth;
  });

  std::unordered_set<uint64_t> used_previous_slots;
  std::unordered_map<uint64_t, uint64_t> remapped_slots;
  uint64_t projected_ranges = 0;
  for (size_t layer_index : order) {
    InjectionLayerRecord &layer = index.layers[layer_index];
    const bool already_current =
        layer.syntax_parsed && layer.syntax &&
        layer.syntax->buffer_revision() == reuse->target_tags.syntax_revision;
    uint64_t parent_reuse_slot_id = layer.parent_reuse_slot_id;
    const auto remapped_parent = remapped_slots.find(parent_reuse_slot_id);
    if (remapped_parent != remapped_slots.end())
      parent_reuse_slot_id = remapped_parent->second;
    bool used_projection = false;
    const auto match = std::find_if(
        reuse->layers.begin(), reuse->layers.end(),
        [&](const ProjectedInjectionLayer &candidate) {
          if (candidate.source == nullptr ||
              used_previous_slots.contains(candidate.source->reuse_slot_id) ||
              !same_layer_configuration(*candidate.source, layer,
                                        parent_reuse_slot_id))
            return false;
          if (candidate.projectable &&
              same_projected_ranges(candidate.ranges, layer.ranges)) {
            used_projection = true;
            return true;
          }
          return reuse->same_extent_edits &&
                 same_projected_ranges(candidate.source->ranges,
                                       layer.ranges);
        });
    if (match == reuse->layers.end())
      continue;
    const uint64_t provisional_slot = layer.reuse_slot_id;
    layer.reuse_slot_id = match->source->reuse_slot_id;
    layer.parent_reuse_slot_id = parent_reuse_slot_id;
    layer.reuse_with_edits = true;
    remapped_slots.emplace(provisional_slot, layer.reuse_slot_id);
    used_previous_slots.insert(layer.reuse_slot_id);
    if (!already_current && used_projection)
      projected_ranges += layer.ranges.size();
    if (already_current)
      layer.reuse_with_edits = false;
  }
  result.reused_layers = std::count_if(
      index.layers.begin(), index.layers.end(), [&](const auto &layer) {
        return layer.reuse_with_edits &&
               used_previous_slots.contains(layer.reuse_slot_id);
      });
  result.projected_ranges = projected_ranges;
  if (result.reused_layers > 0)
    result.syntax_edits = reuse->syntax_edits;
  result.fallback = used_previous_slots.size() != index.layers.size() ||
                    used_previous_slots.size() !=
                        reuse->previous_index->layers.size();
  return result;
}

void prune_child_backends(
    std::unordered_map<uint64_t, std::shared_ptr<SyntaxBackend>> &backends,
    const InjectionRangeIndex &index) {
  std::unordered_set<uint64_t> active;
  active.reserve(index.layers.size());
  for (const InjectionLayerRecord &layer : index.layers)
    active.insert(layer.reuse_slot_id);
  std::erase_if(backends, [&](const auto &entry) {
    return !active.contains(entry.first);
  });
}

void isolate_unparsed_reuse_slots(InjectionRangeIndex &index,
                                  uint64_t revision,
                                  uint64_t &next_reuse_slot_id) {
  std::vector<size_t> order(index.layers.size());
  for (size_t index_value = 0; index_value < order.size(); index_value++)
    order[index_value] = index_value;
  std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right) {
    return index.layers[left].depth < index.layers[right].depth;
  });
  std::unordered_map<uint64_t, uint64_t> remapped_slots;
  for (size_t layer_index : order) {
    InjectionLayerRecord &layer = index.layers[layer_index];
    const auto parent = remapped_slots.find(layer.parent_reuse_slot_id);
    if (parent != remapped_slots.end())
      layer.parent_reuse_slot_id = parent->second;
    if (layer.syntax_parsed && layer.syntax &&
        layer.syntax->buffer_revision() == revision)
      continue;
    const uint64_t old_slot = layer.reuse_slot_id;
    layer.reuse_slot_id = next_reuse_slot_id++;
    layer.reuse_with_edits = false;
    remapped_slots[old_slot] = layer.reuse_slot_id;
  }
}

std::set<InjectionSourceToken> available_source_tokens(
    const std::shared_ptr<const PublishedSyntaxSnapshot> &root_syntax,
    const std::string &root_grammar_id, const InjectionRangeIndex &index,
    const std::map<std::string, uint64_t> &registration_fingerprints) {
  std::set<InjectionSourceToken> tokens;
  const auto root_registration =
      registration_fingerprints.find(root_grammar_id);
  if (root_syntax && root_registration != registration_fingerprints.end()) {
    tokens.insert(InjectionSourceToken{0, root_syntax->identity(),
                                       root_registration->second});
  }
  for (const InjectionLayerRecord &layer : index.layers) {
    const auto registration =
        registration_fingerprints.find(layer.language_id);
    if (!layer.syntax_parsed || !layer.syntax ||
        registration == registration_fingerprints.end())
      continue;
    tokens.insert(InjectionSourceToken{layer.reuse_slot_id,
                                       layer.syntax->identity(),
                                       registration->second});
  }
  return tokens;
}

bool has_unseen_source_tokens(
    const std::set<InjectionSourceToken> &available,
    const std::set<InjectionSourceToken> &seen) {
  return std::any_of(available.begin(), available.end(),
                     [&](const InjectionSourceToken &token) {
                       return !seen.contains(token);
                     });
}

bool publish_topology(uint64_t &published_layer_count,
                      const InjectionRangeIndex &index) {
  // A non-empty layer set has just published fresh child syntax, even when its
  // structural topology is unchanged. The second language-mode notification
  // is required so consumers cannot retain scopes from the pre-child root
  // notification. Only stable empty-to-empty publication is suppressible.
  const bool changed = published_layer_count > 0 || !index.layers.empty();
  published_layer_count = index.layers.size();
  return changed;
}

std::string wasm_language_name(const std::string &path,
                               const std::string &explicit_name) {
  if (!explicit_name.empty())
    return explicit_name;
  const size_t separator = path.find_last_of("/\\");
  const size_t start = separator == std::string::npos ? 0 : separator + 1;
  const size_t extension = path.find_last_of('.');
  std::string name = path.substr(
      start, extension == std::string::npos || extension < start
                 ? std::string::npos
                 : extension - start);
  constexpr std::string_view prefix = "tree-sitter-";
  if (name.starts_with(prefix))
    name.erase(0, prefix.size());
  std::replace(name.begin(), name.end(), '-', '_');
  return name;
}

void apply_query_grammar(InjectionLayerRecord &layer,
                         const ParsedGrammarDescriptor &grammar) {
  layer.language_id = grammar.language_id;
  layer.runtime = grammar.runtime;
  layer.wasm_path = grammar.wasm_path;
  layer.language_name_export = grammar.language_name;
  layer.language_segment = grammar.language_segment;
  layer.query_paths = grammar.query_paths;
  for (InjectionRangeRecord &range : layer.ranges) {
    if (layer.include_language_scope && range.scopes.empty() &&
        !grammar.language_id.empty())
      range.scopes.push_back(grammar.language_id);
  }
}

bool same_query_grammar(const InjectionLayerRecord &layer,
                        const ParsedGrammarDescriptor &grammar) {
  return layer.language_id == grammar.language_id &&
         layer.runtime == grammar.runtime &&
         layer.wasm_path == grammar.wasm_path &&
         layer.language_name_export == grammar.language_name &&
         layer.language_segment == grammar.language_segment &&
         layer.query_paths == grammar.query_paths;
}

bool query_layer_is_resolved(const InjectionLayerRecord &layer) {
  return !layer.language_id.empty() && layer.runtime == "wasm" &&
         !layer.wasm_path.empty();
}

std::vector<SyntaxConfiguration::IncludedRange> syntax_ranges_for_layer(
    const InjectionLayerRecord &layer, const SnapshotAnalysis &analysis) {
  std::vector<SyntaxConfiguration::IncludedRange> output;
  output.reserve(layer.ranges.size() * (layer.newlines_between ? 2 : 1));
  Point previous_end{};
  bool has_previous = false;
  for (const InjectionRangeRecord &range : layer.ranges) {
    if (layer.newlines_between && has_previous &&
        previous_end.row < range.start.row && range.start.row > 0 &&
        range.start.row < analysis.line_starts.size()) {
      const uint64_t newline_end = analysis.line_starts[range.start.row];
      if (newline_end > 0) {
        const uint64_t newline_start = newline_end - 1;
        const uint64_t prior_row = range.start.row - 1;
        output.push_back(SyntaxConfiguration::IncludedRange{
            Point{prior_row,
                  analysis.line_ends[prior_row] -
                      analysis.line_starts[prior_row]},
            Point{range.start.row, 0}, newline_start, newline_end});
      }
    }
    output.push_back(SyntaxConfiguration::IncludedRange{
        range.start, range.end, range.start_index, range.end_index});
    previous_end = range.end;
    has_previous = true;
  }
  return output;
}

} // namespace

class InjectionLayerParseWorker final : public Napi::AsyncWorker {
public:
  struct WorkItem {
    size_t layer_index = 0;
    std::shared_ptr<SyntaxBackend> backend;
    bool use_edits = false;
  };

  InjectionLayerParseWorker(
      Napi::Env env, std::shared_ptr<SessionState> session,
      InjectionEngine *engine, uint64_t request_id,
      InjectionRevisionTags tags,
      std::shared_ptr<InjectionRangeIndex> index,
      std::shared_ptr<const SnapshotAnalysis> analysis,
      std::shared_ptr<NativeJobControl> job,
      std::vector<WorkItem> work_items, std::vector<SyntaxEdit> syntax_edits,
      uint64_t reused_layers, uint64_t projected_ranges,
      bool reuse_fallback, uint64_t maximum_utf16_length)
      : Napi::AsyncWorker(env, "DocumentSession.parseInjectionLayers"),
        deferred_(Napi::Promise::Deferred::New(env)),
        session_(std::move(session)), engine_(engine), request_id_(request_id),
        tags_(tags), index_(std::move(index)), analysis_(std::move(analysis)),
        job_(std::move(job)), work_items_(std::move(work_items)),
        syntax_edits_(std::move(syntax_edits)),
        reused_layers_(reused_layers), projected_ranges_(projected_ranges),
        reuse_fallback_(reuse_fallback),
        maximum_utf16_length_(maximum_utf16_length),
        queued_at_(InjectionClock::now()) {}

  Napi::Promise promise() const { return deferred_.Promise(); }

  void OnExecute(Napi::Env env) override {
    Napi::AsyncWorker::OnExecute(env);
    job_->mark_worker_finished();
    if (session_->addon_data != nullptr)
      session_->addon_data->notify_cleanup_progress();
  }

  void Execute() override {
    const auto started_at = InjectionClock::now();
    queue_milliseconds_ = std::chrono::duration<double, std::milli>(
                              started_at - queued_at_)
                              .count();
    const auto finish_timing = [&]() {
      parse_milliseconds_ = elapsed_milliseconds(started_at);
    };
    SnapshotReader reader(job_->lease());
    if (!reader.valid()) {
      SetError("Unable to read snapshot while parsing injected languages");
      finish_timing();
      return;
    }
    SyntaxCancellation cancellation{
        &session_->destroyed_signal, &session_->latest_requested_signal,
        &session_->language_generation_signal, tags_.buffer_revision,
        tags_.language_generation};
    for (const WorkItem &item : work_items_) {
      if (cancellation.requested()) {
        cancelled_ = true;
        finish_timing();
        return;
      }
      InjectionLayerRecord &layer = index_->layers[item.layer_index];
      SyntaxConfiguration configuration;
      configuration.language_id = layer.language_id;
      configuration.runtime = layer.runtime;
      configuration.wasm_path = layer.wasm_path;
      configuration.language_name =
          wasm_language_name(layer.wasm_path, layer.language_name_export);
      configuration.queries.paths = layer.query_paths;
      configuration.queries.language_segment = layer.language_segment;
      configuration.queries.grammar_identity =
          layer.wasm_path + '\n' + configuration.language_name;
      configuration.queries.grammar_generation = tags_.language_generation;
      configuration.included_ranges =
          syntax_ranges_for_layer(layer, *analysis_);
      configuration.generation = tags_.language_generation;

      SyntaxParseResult result;
      const std::vector<SyntaxEdit> &edits =
          item.use_edits ? syntax_edits_ : no_edits_;
      if (!item.backend->parse(reader, analysis_, configuration, edits,
                               tags_.buffer_revision,
                               maximum_utf16_length_, cancellation, result)) {
        layer.syntax_error_code = result.error_code.empty()
                                      ? "ERR_INJECTION_SYNTAX_PARSE"
                                      : result.error_code;
        layer.syntax_error_message = result.error_message;
        continue;
      }
      if (result.cancelled || cancellation.requested()) {
        cancelled_ = true;
        finish_timing();
        return;
      }
      layer.syntax_parsed = result.parsed;
      layer.syntax_root_has_error = result.root_has_error;
      layer.syntax = std::move(result.published_snapshot);
      layer.queries = std::move(result.query_snapshot);
      if (result.parsed) {
        if (result.incremental)
          child_incremental_parses_++;
        else
          child_full_parses_++;
      }
    }
    finalize_index_counts(*index_);
    finish_timing();
  }

  void OnWorkComplete(Napi::Env env, napi_status status) override {
    if (status == napi_cancelled ||
        session_->environment_cleanup_signal.load(std::memory_order_acquire)) {
      job_->release_lease_on_owner();
      finish_injection_job_without_js(session_);
      Napi::AsyncWorker::OnWorkComplete(env, napi_cancelled);
      return;
    }
    Napi::AsyncWorker::OnWorkComplete(env, status);
  }

  void OnOK() override {
    Napi::Env env = Env();
    job_->release_lease_on_owner();
    std::unique_lock<std::mutex> lock(session_->mutex);
    auto request = engine_->impl_->requests.find(request_id_);
    if (cancelled_ || request == engine_->impl_->requests.end() ||
        !current_tags_locked(*session_, tags_) ||
        session_->injection_engine.get() != engine_) {
      if (request != engine_->impl_->requests.end())
        engine_->impl_->requests.erase(request);
      engine_->impl_->stale_request_count++;
      deferred_.Resolve(stale_result(env, tags_, "stale-child-parse",
                                     request_id_));
      lock.unlock();
      finish_injection_job(env, session_);
      return;
    }
    const bool topology_changed = publish_topology(
        engine_->impl_->published_layer_count, *index_);
    if (topology_changed)
      engine_->impl_->topology_generation++;
    engine_->impl_->reused_layer_count += reused_layers_;
    engine_->impl_->projected_range_count += projected_ranges_;
    engine_->impl_->child_incremental_parse_count +=
        child_incremental_parses_;
    engine_->impl_->child_full_parse_count += child_full_parses_;
    const std::set<InjectionSourceToken> available_tokens =
        available_source_tokens(engine_->impl_->root_syntax,
                                engine_->impl_->root_grammar_id, *index_,
                                request->second.registration_fingerprints);
    const bool unseen_sources = has_unseen_source_tokens(
        available_tokens, engine_->impl_->seen_source_tokens);
    const bool rescan_truncated =
        unseen_sources &&
        request->second.rescan_wave >= MAX_INJECTION_RESCAN_WAVES;
    const bool rescan_required = unseen_sources && !rescan_truncated;
    const bool reuse_fallback = reuse_fallback_ && !rescan_required;
    if (reuse_fallback)
      engine_->impl_->reuse_fallback_count++;
    engine_->impl_->published_index = index_;
    if (!rescan_required)
      prune_child_backends(engine_->impl_->child_backends, *index_);
    if (!rescan_required &&
        engine_->impl_->reuse_state == request->second.reuse)
      engine_->impl_->reuse_state.reset();
    engine_->impl_->published_generation++;
    engine_->impl_->candidate_count = 0;
    engine_->impl_->requests.erase(request);
    Napi::Object response = Napi::Object::New(env);
    response.Set("accepted", Napi::Boolean::New(env, true));
    set_tags(response, tags_);
    response.Set("requestId", Napi::Number::New(env, request_id_));
    response.Set("parsedChildLayers",
                 Napi::Number::New(env, index_->parsed_child_layer_count));
    response.Set("failedChildLayers",
                 Napi::Number::New(env, index_->failed_child_layer_count));
    response.Set("topologyChanged",
                 Napi::Boolean::New(env, topology_changed));
    response.Set("childParseQueueMilliseconds",
                 Napi::Number::New(env, queue_milliseconds_));
    response.Set("childParseMilliseconds",
                 Napi::Number::New(env, parse_milliseconds_));
    response.Set("reusedLayers", Napi::Number::New(env, reused_layers_));
    response.Set("projectedRanges",
                 Napi::Number::New(env, projected_ranges_));
    response.Set("childIncrementalParses",
                 Napi::Number::New(env, child_incremental_parses_));
    response.Set("childFullParses",
                 Napi::Number::New(env, child_full_parses_));
    response.Set("reuseFallback", Napi::Boolean::New(env, reuse_fallback));
    response.Set("rescanRequired",
                 Napi::Boolean::New(env, rescan_required));
    response.Set("rescanTruncated",
                 Napi::Boolean::New(env, rescan_truncated));
    response.Set("sourceTokenFingerprint",
                 Napi::String::New(
                     env, source_token_fingerprint(available_tokens)));
    deferred_.Resolve(response);
    lock.unlock();
    finish_injection_job(env, session_);
  }

  void OnError(const Napi::Error &error) override {
    job_->release_lease_on_owner();
    {
      std::lock_guard<std::mutex> lock(session_->mutex);
      engine_->impl_->requests.erase(request_id_);
      engine_->impl_->candidate_count = 0;
    }
    Napi::Error output = Napi::Error::New(Env(), error.Message());
    set_error_code(output, "ERR_INJECTION_LAYER_PARSE");
    deferred_.Reject(output.Value());
    finish_injection_job(Env(), session_);
  }

  ~InjectionLayerParseWorker() override {
    // NativeJobControl releases only on the environment owner thread.
  }

private:
  Napi::Promise::Deferred deferred_;
  std::shared_ptr<SessionState> session_;
  InjectionEngine *engine_ = nullptr;
  uint64_t request_id_ = 0;
  InjectionRevisionTags tags_;
  std::shared_ptr<InjectionRangeIndex> index_;
  std::shared_ptr<const SnapshotAnalysis> analysis_;
  std::shared_ptr<NativeJobControl> job_;
  std::vector<WorkItem> work_items_;
  std::vector<SyntaxEdit> syntax_edits_;
  const std::vector<SyntaxEdit> no_edits_;
  uint64_t reused_layers_ = 0;
  uint64_t projected_ranges_ = 0;
  uint64_t child_incremental_parses_ = 0;
  uint64_t child_full_parses_ = 0;
  bool reuse_fallback_ = false;
  uint64_t maximum_utf16_length_ = MAX_SYNTAX_UTF16_LENGTH;
  bool cancelled_ = false;
  InjectionClock::time_point queued_at_;
  double queue_milliseconds_ = 0;
  double parse_milliseconds_ = 0;
};

InjectionEngine::InjectionEngine() : impl_(std::make_unique<Impl>()) {}

InjectionEngine::~InjectionEngine() = default;

void InjectionEngine::publish_root(
    std::shared_ptr<const PublishedSyntaxSnapshot> syntax,
    std::shared_ptr<const SyntaxQuerySnapshot> queries,
    const SnapshotAnalysis &analysis, const std::string &grammar_id,
    const std::vector<RevisionEditBatch> &projection, bool root_incremental,
    bool injection_workers_idle,
    const void *snapshot_lease) {
  const std::shared_ptr<const InjectionRangeIndex> previous_index =
      impl_->published_index;
  const std::string previous_root_grammar_id = impl_->root_grammar_id;
  impl_->stale_request_count += impl_->requests.size();
  impl_->requests.clear();
  impl_->candidate_epoch++;
  impl_->seen_source_tokens.clear();
  impl_->candidate_count = 0;
  impl_->root_syntax = std::move(syntax);
  impl_->root_queries = std::move(queries);
  impl_->analysis = analysis;
  impl_->root_grammar_id = grammar_id;
  InjectionRevisionTags target_tags;
  if (impl_->root_syntax) {
    target_tags = InjectionRevisionTags{
        impl_->root_syntax->buffer_revision(),
        impl_->root_syntax->buffer_revision(),
        impl_->root_syntax->language_generation()};
  }
  impl_->reuse_state = build_reuse_state(
      previous_index, projection, root_incremental, target_tags, analysis,
      previous_root_grammar_id == grammar_id, injection_workers_idle);
  impl_->query_proposals.clear();
  if (impl_->root_syntax && snapshot_lease != nullptr) {
    SnapshotReader reader(
        static_cast<const SuperstringSnapshotLease *>(snapshot_lease));
    if (reader.valid()) {
      impl_->query_proposals = query_injection_proposals(
          impl_->root_syntax, impl_->root_queries, reader, grammar_id);
    }
  }
  auto empty = std::make_shared<InjectionRangeIndex>();
  if (impl_->root_syntax) {
    empty->tags = InjectionRevisionTags{
        impl_->root_syntax->buffer_revision(),
        impl_->root_syntax->buffer_revision(),
        impl_->root_syntax->language_generation()};
  }
  for (const QueryInjectionProposal &proposal : impl_->query_proposals) {
    InjectionLayerRecord layer;
    layer.layer_id = impl_->next_layer_id++;
    layer.reuse_slot_id = impl_->next_reuse_slot_id++;
    layer.query_pattern_index = proposal.pattern_index;
    layer.depth = 1;
    layer.parent_grammar_id = proposal.parent_grammar_id;
    layer.language_name = proposal.target_self || proposal.target_parent
                              ? grammar_id
                              : proposal.language_name;
    layer.include_children = proposal.include_children;
    layer.query_defined = true;
    layer.include_language_scope = proposal.include_language_scope;
    layer.ranges =
        included_ranges(proposal.content, proposal.include_children, analysis);
    if (layer.ranges.empty())
      continue;
    if (impl_->query_rejected_aliases.contains(layer.language_name))
      continue;
    const auto known_grammar =
        impl_->query_grammars.find(layer.language_name);
    if (known_grammar != impl_->query_grammars.end())
      apply_query_grammar(layer, known_grammar->second);
    empty->range_count += layer.ranges.size();
    empty->query_layer_count++;
    empty->layers.push_back(std::move(layer));
  }
  impl_->published_index = std::move(empty);
  impl_->published_generation++;
}

void InjectionEngine::clear() {
  impl_->stale_request_count += impl_->requests.size();
  impl_->requests.clear();
  impl_->candidate_epoch++;
  impl_->seen_source_tokens.clear();
  impl_->root_syntax.reset();
  impl_->root_queries.reset();
  impl_->root_grammar_id.clear();
  impl_->query_proposals.clear();
  impl_->published_index.reset();
  impl_->reuse_state.reset();
  impl_->child_backends.clear();
  impl_->query_grammars.clear();
  impl_->query_rejected_aliases.clear();
  impl_->candidate_count = 0;
  impl_->published_layer_count = 0;
  impl_->published_generation++;
}

std::shared_ptr<const InjectionRangeIndex>
InjectionEngine::index_snapshot() const {
  return impl_->published_index;
}

InjectionEngineDiagnostics InjectionEngine::diagnostics() const {
  InjectionEngineDiagnostics result;
  result.active_request_count = impl_->requests.size();
  result.candidate_count = impl_->candidate_count;
  result.stale_request_count = impl_->stale_request_count;
  result.aborted_request_count = impl_->aborted_request_count;
  result.published_generation = impl_->published_generation;
  result.topology_generation = impl_->topology_generation;
  result.reused_layer_count = impl_->reused_layer_count;
  result.projected_range_count = impl_->projected_range_count;
  result.child_incremental_parse_count =
      impl_->child_incremental_parse_count;
  result.child_full_parse_count = impl_->child_full_parse_count;
  result.reuse_fallback_count = impl_->reuse_fallback_count;
  result.child_backend_count = impl_->child_backends.size();
  result.query_language_resolution_count =
      impl_->query_language_resolution_count;
  result.query_language_rejection_count =
      impl_->query_language_rejection_count;
  if (impl_->published_index) {
    result.active_layer_count = impl_->published_index->layers.size();
    result.dynamic_layer_count = impl_->published_index->dynamic_layer_count;
    result.query_layer_count = impl_->published_index->query_layer_count;
    result.range_count = impl_->published_index->range_count;
    result.parsed_child_layer_count =
        impl_->published_index->parsed_child_layer_count;
    result.failed_child_layer_count =
        impl_->published_index->failed_child_layer_count;
    result.maximum_depth = impl_->published_index->maximum_depth;
    result.unresolved_query_language_count = std::count_if(
        impl_->published_index->layers.begin(),
        impl_->published_index->layers.end(),
        [](const InjectionLayerRecord &layer) {
          return layer.query_defined && !query_layer_is_resolved(layer);
        });
  }
  return result;
}

bool InjectionEngine::owns_syntax_snapshot(
    const std::shared_ptr<const PublishedSyntaxSnapshot> &syntax,
    const InjectionRevisionTags &tags) const {
  if (!syntax || syntax->buffer_revision() != tags.syntax_revision ||
      syntax->language_generation() != tags.language_generation)
    return false;
  if (impl_->root_syntax.get() == syntax.get())
    return true;
  if (!impl_->published_index ||
      !tags_equal(impl_->published_index->tags, tags))
    return false;
  return std::any_of(
      impl_->published_index->layers.begin(),
      impl_->published_index->layers.end(),
      [&](const InjectionLayerRecord &layer) {
        return layer.syntax.get() == syntax.get();
      });
}

std::vector<InjectionQuerySource>
InjectionEngine::query_sources(const QueryRange &range) const {
  std::vector<InjectionQuerySource> result;
  if (!impl_->published_index)
    return result;
  const Point query_start{range.start_row, range.start_column};
  const Point query_end{range.end_row, range.end_column};
  for (const InjectionLayerRecord &layer : impl_->published_index->layers) {
    if (!layer.syntax_parsed || !layer.syntax)
      continue;
    InjectionQuerySource source;
    source.layer_id = layer.layer_id;
    source.parent_layer_id = layer.parent_layer_id;
    source.depth = layer.depth;
    source.grammar_id = layer.language_id;
    source.syntax = layer.syntax;
    source.cover_shallower_scopes = layer.cover_shallower_scopes;
    for (const InjectionRangeRecord &candidate : layer.ranges) {
      if (candidate.end < query_start || query_end < candidate.start)
        continue;
      source.included_ranges.push_back(InjectionQuerySource::IncludedRange{
          Range{candidate.start, candidate.end}, candidate.scopes});
    }
    if (!source.included_ranges.empty())
      result.push_back(std::move(source));
  }
  std::sort(result.begin(), result.end(),
            [](const InjectionQuerySource &left,
               const InjectionQuerySource &right) {
              return std::tie(left.depth, left.layer_id) <
                     std::tie(right.depth, right.layer_id);
            });
  return result;
}

Napi::Value InjectionEngine::queue_child_parse(
    Napi::Env env, uint64_t request_id,
    const std::shared_ptr<SessionState> &session) {
  auto request_found = impl_->requests.find(request_id);
  if (request_found == impl_->requests.end())
    return stale_result(env, InjectionRevisionTags{}, "stale-child-parse",
                        request_id);
  InjectionRequestState &request = request_found->second;
  if (request.child_parse_active)
    return throw_error(env, "Injected language parse is already active",
                       "ERR_INJECTION_PARSE_ACTIVE");

  const bool overlapping_injection_job = session->active_injection_jobs > 0;
  if (request.reuse && overlapping_injection_job)
    request.reuse->eligible = false;
  LayerReconciliation reconciliation =
      reconcile_layers(*request.staged_index, request.reuse);
  if (overlapping_injection_job) {
    isolate_unparsed_reuse_slots(*request.staged_index,
                                 request.tags.syntax_revision,
                                 impl_->next_reuse_slot_id);
    reconciliation.reused_layers = 0;
    reconciliation.projected_ranges = 0;
    reconciliation.syntax_edits.clear();
    reconciliation.fallback = !request.staged_index->layers.empty();
  }

  std::vector<InjectionLayerParseWorker::WorkItem> work_items;
  for (size_t index = 0; index < request.staged_index->layers.size(); index++) {
    InjectionLayerRecord &layer = request.staged_index->layers[index];
    if (layer.runtime != "wasm" || layer.wasm_path.empty() ||
        layer.ranges.empty() ||
        (layer.syntax_parsed && layer.syntax &&
         layer.syntax->buffer_revision() == request.tags.syntax_revision))
      continue;
    std::shared_ptr<SyntaxBackend> &backend =
        impl_->child_backends[layer.reuse_slot_id];
    if (!backend)
      backend = std::make_shared<SyntaxBackend>();
    work_items.push_back(
        InjectionLayerParseWorker::WorkItem{
            index, backend, layer.reuse_with_edits});
  }
  if (work_items.empty()) {
    finalize_index_counts(*request.staged_index);
    const bool topology_changed =
        publish_topology(impl_->published_layer_count, *request.staged_index);
    if (topology_changed)
      impl_->topology_generation++;
    impl_->reused_layer_count += reconciliation.reused_layers;
    impl_->projected_range_count += reconciliation.projected_ranges;
    const std::set<InjectionSourceToken> available_tokens =
        available_source_tokens(impl_->root_syntax, impl_->root_grammar_id,
                                *request.staged_index,
                                request.registration_fingerprints);
    const bool unseen_sources =
        has_unseen_source_tokens(available_tokens, impl_->seen_source_tokens);
    const bool rescan_truncated =
        unseen_sources && request.rescan_wave >= MAX_INJECTION_RESCAN_WAVES;
    const bool rescan_required = unseen_sources && !rescan_truncated;
    const bool reuse_fallback =
        reconciliation.fallback && !rescan_required;
    if (reuse_fallback)
      impl_->reuse_fallback_count++;
    impl_->published_index = request.staged_index;
    if (!rescan_required)
      prune_child_backends(impl_->child_backends, *request.staged_index);
    if (!rescan_required && impl_->reuse_state == request.reuse)
      impl_->reuse_state.reset();
    impl_->published_generation++;
    impl_->candidate_count = 0;
    const InjectionRevisionTags tags = request.tags;
    const uint64_t parsed_child_layers =
        request.staged_index->parsed_child_layer_count;
    const uint64_t failed_child_layers =
        request.staged_index->failed_child_layer_count;
    impl_->requests.erase(request_found);
    Napi::Object response = Napi::Object::New(env);
    response.Set("accepted", Napi::Boolean::New(env, true));
    set_tags(response, tags);
    response.Set("requestId", Napi::Number::New(env, request_id));
    response.Set(
        "parsedChildLayers",
        Napi::Number::New(env, parsed_child_layers));
    response.Set(
        "failedChildLayers",
        Napi::Number::New(env, failed_child_layers));
    response.Set("topologyChanged",
                 Napi::Boolean::New(env, topology_changed));
    response.Set("childParseQueueMilliseconds", Napi::Number::New(env, 0));
    response.Set("childParseMilliseconds", Napi::Number::New(env, 0));
    response.Set("reusedLayers",
                 Napi::Number::New(env, reconciliation.reused_layers));
    response.Set("projectedRanges",
                 Napi::Number::New(env, reconciliation.projected_ranges));
    response.Set("childIncrementalParses", Napi::Number::New(env, 0));
    response.Set("childFullParses", Napi::Number::New(env, 0));
    response.Set("reuseFallback",
                 Napi::Boolean::New(env, reuse_fallback));
    response.Set("rescanRequired",
                 Napi::Boolean::New(env, rescan_required));
    response.Set("rescanTruncated",
                 Napi::Boolean::New(env, rescan_truncated));
    response.Set("sourceTokenFingerprint",
                 Napi::String::New(
                     env, source_token_fingerprint(available_tokens)));
    return response;
  }

  if (session->syntax_lease == nullptr || session->syntax_analysis == nullptr ||
      session->syntax_revision != request.tags.syntax_revision)
    return stale_result(env, request.tags, "snapshot-unavailable", request_id);
  const SuperstringSnapshotLeaseStatus retained =
      session->syntax_lease->functions->retain(
          session->syntax_lease->context);
  if (retained != SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK)
    return throw_error(env, "Unable to retain snapshot for injected parse",
                       "ERR_INJECTION_SNAPSHOT");
  request.child_parse_active = true;
  session->active_injection_jobs++;
  auto job = std::make_shared<NativeJobControl>(session->syntax_lease);
  register_native_job_locked(*session, job);
  auto *worker = new InjectionLayerParseWorker(
      env, session, this, request_id, request.tags, request.staged_index,
      session->syntax_analysis, std::move(job), std::move(work_items),
      std::move(reconciliation.syntax_edits),
      reconciliation.reused_layers, reconciliation.projected_ranges,
      reconciliation.fallback,
      session->maximum_syntax_utf16_length);
  Napi::Promise promise = worker->promise();
  worker->Queue();
  return promise;
}

Napi::Value InjectionEngine::get_candidates(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &session) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !info[0].IsObject())
    return throw_type_error(env, "getInjectionCandidates expects a request",
                            "ERR_INVALID_INJECTION_REQUEST");
  Napi::Object request = info[0].As<Napi::Object>();
  InjectionRevisionTags tags;
  uint64_t generation = 0;
  uint32_t rescan_wave = 0;
  if (!read_tags(request, &tags) ||
      !read_uint64(request.Get("injectionPointGeneration"), &generation))
    return throw_type_error(env, "Injection request revision tags are invalid",
                            "ERR_INVALID_INJECTION_REQUEST");
  Napi::Value rescan_wave_value = request.Get("injectionRescanWave");
  if (!rescan_wave_value.IsUndefined() &&
      !read_uint32(rescan_wave_value, &rescan_wave))
    return throw_type_error(env, "Injection rescan wave must fit uint32",
                            "ERR_INVALID_INJECTION_REQUEST");
  std::vector<GrammarManifestEntry> manifest;
  if (!parse_manifest(env, request.Get("grammars"), &manifest))
    return env.Undefined();

  std::vector<CandidateSource> sources;
  auto job = std::make_shared<NativeJobControl>();
  uint64_t candidate_epoch = 0;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!current_tags_locked(*session, tags) || !session->published_syntax ||
        session->published_syntax->buffer_revision() != tags.syntax_revision)
      return stale_result(env, tags, "stale-request");
    impl_->stale_request_count += impl_->requests.size();
    impl_->requests.clear();
    candidate_epoch = ++impl_->candidate_epoch;
    sources.push_back(CandidateSource{session->published_syntax,
                                     impl_->root_grammar_id, 0, 0, 0});
    if (impl_->published_index) {
      for (const InjectionLayerRecord &layer : impl_->published_index->layers) {
        if (layer.syntax_parsed && layer.syntax)
          sources.push_back(CandidateSource{
              layer.syntax, layer.language_id, layer.layer_id,
              layer.reuse_slot_id, layer.depth});
      }
    }
    session->active_injection_jobs++;
    register_native_job_locked(*session, job);
  }
  auto *worker = new InjectionCandidateWorker(
      env, session, this, std::move(sources), std::move(manifest),
      rescan_wave > 0 ? impl_->seen_source_tokens
                      : std::set<InjectionSourceToken>{},
      tags, generation, rescan_wave, candidate_epoch, std::move(job));
  Napi::Promise promise = worker->promise();
  worker->Queue();
  return promise;
}

Napi::Value InjectionEngine::resolve_node(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &session) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsObject() || !info[1].IsObject())
    return throw_type_error(env,
                            "resolveInjectionNode expects candidate and tags",
                            "ERR_INVALID_INJECTION_REQUEST");
  Napi::Object candidate = info[0].As<Napi::Object>();
  Napi::Object tag_object = info[1].As<Napi::Object>();
  InjectionRevisionTags tags;
  uint32_t candidate_id = 0;
  uint64_t request_id = 0;
  if (!read_tags(tag_object, &tags) ||
      !read_uint32(candidate.Get("candidateId"), &candidate_id) ||
      !read_uint64(candidate.Get("requestId"), &request_id))
    return throw_type_error(env, "Injection candidate identity is invalid",
                            "ERR_INVALID_INJECTION_REQUEST");

  std::lock_guard<std::mutex> lock(session->mutex);
  if (!current_tags_locked(*session, tags))
    return stale_node(env);
  const auto request = impl_->requests.find(request_id);
  if (request == impl_->requests.end() ||
      request->second.candidate_epoch != impl_->candidate_epoch ||
      !tags_equal(request->second.tags, tags))
    return throw_error(env, "Injection candidate is no longer active",
                       "ERR_STALE_INJECTION_NODE");
  const auto found = request->second.candidates.find(candidate_id);
  if (found == request->second.candidates.end())
    return throw_error(env, "Injection candidate is no longer active",
                       "ERR_STALE_INJECTION_NODE");
  const InjectionCandidateRecord &record = found->second;
  Napi::Value requested_handle = candidate.Get("nodeHandle");
  uint32_t handle = record.node_handle;
  if (!requested_handle.IsUndefined()) {
    uint32_t parsed_handle = 0;
    if (!read_uint32(requested_handle, &parsed_handle) ||
        parsed_handle != handle)
      return throw_error(env, "Injection node handle does not match candidate",
                         "ERR_INVALID_INJECTION_NODE_HANDLE");
  }
  return InjectionNodeWrapper::new_instance(env, session, record.syntax,
                                             handle, tags,
                                             session->syntax_lease);
}

Napi::Value InjectionEngine::resolve_query_node(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &session) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsObject() || !info[1].IsObject())
    return throw_type_error(
        env, "resolveQueryNode expects an identity and revision tags",
        "ERR_INVALID_INJECTION_REQUEST");
  Napi::Object identity = info[0].As<Napi::Object>();
  InjectionRevisionTags tags;
  uint64_t layer_id = 0;
  uint32_t node_handle = 0;
  if (!read_tags(info[1].As<Napi::Object>(), &tags) ||
      !read_uint64(identity.Get("layerId"), &layer_id) ||
      !read_uint32(identity.Get("nodeHandle"), &node_handle) ||
      node_handle == 0)
    return throw_type_error(env, "Query node identity is invalid",
                            "ERR_INVALID_INJECTION_NODE_HANDLE");

  std::lock_guard<std::mutex> lock(session->mutex);
  if (!current_tags_locked(*session, tags) ||
      tags.buffer_revision != tags.syntax_revision)
    return stale_node(env);
  std::shared_ptr<const PublishedSyntaxSnapshot> syntax;
  if (layer_id == 0) {
    syntax = session->published_syntax;
  } else if (impl_->published_index &&
             tags_equal(impl_->published_index->tags, tags)) {
    const auto layer = std::find_if(
        impl_->published_index->layers.begin(),
        impl_->published_index->layers.end(),
        [&](const InjectionLayerRecord &candidate) {
          return candidate.layer_id == layer_id;
        });
    if (layer != impl_->published_index->layers.end())
      syntax = layer->syntax;
  }
  if (!syntax || !owns_syntax_snapshot(syntax, tags) ||
      ts_node_is_null(syntax->node(node_handle)))
    return throw_error(env, "Query node is no longer available",
                       "ERR_STALE_INJECTION_NODE");
  return InjectionNodeWrapper::new_instance(env, session, syntax,
                                             node_handle, tags,
                                             session->syntax_lease);
}

namespace {

struct SyntaxSearchSource {
  uint64_t layer_id = 0;
  uint32_t depth = 0;
  std::string grammar_id;
  std::shared_ptr<const PublishedSyntaxSnapshot> syntax;
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
};

struct SyntaxSearchCandidate {
  SyntaxSearchSource source;
  uint32_t node_handle = 0;
  uint64_t breadth = 0;
};

bool source_contains_point(const SyntaxSearchSource &source, uint64_t index) {
  if (source.layer_id == 0)
    return true;
  return std::any_of(source.ranges.begin(), source.ranges.end(),
                     [index](const auto &range) {
                       return range.first <= index && index <= range.second;
                     });
}

bool source_contains_range(const SyntaxSearchSource &source, uint64_t start,
                           uint64_t end) {
  if (source.layer_id == 0)
    return true;
  bool contains_start = false;
  bool contains_end = false;
  for (const auto &[range_start, range_end] : source.ranges) {
    contains_start |= range_start <= start && start <= range_end;
    contains_end |= range_start <= end && end <= range_end;
  }
  return contains_start && contains_end;
}

TSNode node_at_index(const PublishedSyntaxSnapshot &syntax, uint64_t index) {
  const TSNode root = ts_tree_root_node(syntax.tree());
  const uint64_t root_start = start_index_for(root);
  const uint64_t root_end = end_index_for(root);
  if (index < root_start || index > root_end)
    return root;
  const uint32_t byte = static_cast<uint32_t>(index * UINT64_C(2));
  const TSNode node = ts_node_descendant_for_byte_range(root, byte, byte);
  return ts_node_is_null(node) ? root : node;
}

TSNode node_containing_range(const PublishedSyntaxSnapshot &syntax,
                             uint64_t start, uint64_t end) {
  const TSNode root = ts_tree_root_node(syntax.tree());
  if (start < start_index_for(root) || end > end_index_for(root))
    return root;
  const uint32_t start_byte = static_cast<uint32_t>(start * UINT64_C(2));
  const uint32_t end_byte = static_cast<uint32_t>(end * UINT64_C(2));
  TSNode node =
      ts_node_descendant_for_byte_range(root, start_byte, end_byte);
  const uint64_t requested_breadth = end - start;
  while (!ts_node_is_null(node)) {
    const uint64_t node_start = start_index_for(node);
    const uint64_t node_end = end_index_for(node);
    if (node_start <= start && node_end >= end &&
        node_end - node_start > requested_breadth)
      return node;
    node = ts_node_parent(node);
  }
  return TSNode{};
}

void sort_search_candidates(std::vector<SyntaxSearchCandidate> *candidates) {
  std::stable_sort(candidates->begin(), candidates->end(),
                   [](const SyntaxSearchCandidate &left,
                      const SyntaxSearchCandidate &right) {
                     return left.breadth != right.breadth
                                ? left.breadth < right.breadth
                                : left.source.depth > right.source.depth;
                   });
}

Napi::Object syntax_search_response(
    Napi::Env env, const InjectionRevisionTags &tags, bool current,
    const std::weak_ptr<SessionState> &session,
    const std::shared_ptr<SnapshotLeaseReference> &lease,
    const std::vector<SyntaxSearchCandidate> &candidates) {
  Napi::Object result = Napi::Object::New(env);
  set_tags(result, tags);
  result.Set("current", Napi::Boolean::New(env, current));
  Napi::Array values = Napi::Array::New(env, candidates.size());
  for (uint32_t index = 0; index < candidates.size(); index++) {
    const SyntaxSearchCandidate &candidate = candidates[index];
    Napi::Object value = Napi::Object::New(env);
    value.Set("node", InjectionNodeWrapper::new_instance(
                          env, session, candidate.source.syntax,
                          candidate.node_handle, tags, lease));
    value.Set("grammarId", Napi::String::New(env,
                                             candidate.source.grammar_id));
    value.Set("depth", Napi::Number::New(env, candidate.source.depth));
    value.Set("layerId", Napi::Number::New(
                             env, static_cast<double>(candidate.source.layer_id)));
    values.Set(index, value);
  }
  result.Set("candidates", values);
  if (candidates.empty()) {
    result.Set("node", env.Null());
    result.Set("grammarId", env.Null());
    result.Set("depth", env.Null());
    result.Set("layerId", env.Null());
  } else {
    Napi::Object first =
        values.Get(static_cast<uint32_t>(0)).As<Napi::Object>();
    result.Set("node", first.Get("node"));
    result.Set("grammarId", first.Get("grammarId"));
    result.Set("depth", first.Get("depth"));
    result.Set("layerId", first.Get("layerId"));
  }
  return result;
}

} // namespace

Napi::Value InjectionEngine::get_syntax_node_at_position(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &session) {
  Napi::Env env = info.Env();
  Point point;
  if (info.Length() == 0 || !read_point_value(info[0], &point))
    return throw_type_error(env, "getSyntaxNodeAtPosition expects a point",
                            "ERR_INVALID_SYNTAX_NODE_RANGE");

  InjectionRevisionTags tags;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  std::vector<SyntaxSearchSource> sources;
  std::shared_ptr<SnapshotLeaseReference> lease;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->destroyed)
      return throw_error(env, "Document session was destroyed",
                         "ERR_DOCUMENT_SESSION_DESTROYED");
    tags = InjectionRevisionTags{session->buffer_revision,
                                 session->syntax_revision,
                                 session->language_generation};
    const bool current = session->buffer_revision == session->syntax_revision &&
                         session->published_syntax &&
                         session->syntax_analysis && session->syntax_lease;
    if (!current)
      return syntax_search_response(env, tags, false, session, nullptr, {});
    analysis = session->syntax_analysis;
    lease = SnapshotLeaseReference::acquire(session->syntax_lease);
    if (!lease)
      return throw_error(env, "Unable to retain syntax snapshot lease",
                         "ERR_SYNTAX_NODE_LEASE");
    session->addon_data->register_node_lease(lease->id(), lease);
    SyntaxSearchSource root;
    root.grammar_id = session->language_id;
    root.syntax = session->published_syntax;
    sources.push_back(std::move(root));
    if (impl_->published_index &&
        impl_->published_index->tags.syntax_revision == tags.syntax_revision &&
        impl_->published_index->tags.language_generation ==
            tags.language_generation) {
      for (const InjectionLayerRecord &layer : impl_->published_index->layers) {
        if (!layer.syntax_parsed || !layer.syntax)
          continue;
        SyntaxSearchSource source;
        source.layer_id = layer.layer_id;
        source.depth = layer.depth;
        source.grammar_id = layer.language_id;
        source.syntax = layer.syntax;
        for (const InjectionRangeRecord &range : layer.ranges)
          source.ranges.emplace_back(range.start_index, range.end_index);
        sources.push_back(std::move(source));
      }
    }
  }

  const uint64_t index = analysis_offset_for_point(*analysis, point);
  std::vector<SyntaxSearchCandidate> candidates;
  for (const SyntaxSearchSource &source : sources) {
    if (!source.syntax || !source_contains_point(source, index))
      continue;
    const TSNode node = node_at_index(*source.syntax, index);
    const uint32_t handle = source.syntax->handle(node);
    if (handle == 0)
      continue;
    candidates.push_back(SyntaxSearchCandidate{
        source, handle, end_index_for(node) - start_index_for(node)});
  }
  sort_search_candidates(&candidates);
  return syntax_search_response(env, tags, true, session, lease, candidates);
}

Napi::Value InjectionEngine::get_syntax_node_containing_range(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &session) {
  Napi::Env env = info.Env();
  Range range;
  if (info.Length() == 0 || !read_range_value(info[0], &range) ||
      range.end < range.start)
    return throw_type_error(env, "getSyntaxNodeContainingRange expects an ordered range",
                            "ERR_INVALID_SYNTAX_NODE_RANGE");

  InjectionRevisionTags tags;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  std::vector<SyntaxSearchSource> sources;
  std::shared_ptr<SnapshotLeaseReference> lease;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->destroyed)
      return throw_error(env, "Document session was destroyed",
                         "ERR_DOCUMENT_SESSION_DESTROYED");
    tags = InjectionRevisionTags{session->buffer_revision,
                                 session->syntax_revision,
                                 session->language_generation};
    const bool current = session->buffer_revision == session->syntax_revision &&
                         session->published_syntax &&
                         session->syntax_analysis && session->syntax_lease;
    if (!current)
      return syntax_search_response(env, tags, false, session, nullptr, {});
    analysis = session->syntax_analysis;
    lease = SnapshotLeaseReference::acquire(session->syntax_lease);
    if (!lease)
      return throw_error(env, "Unable to retain syntax snapshot lease",
                         "ERR_SYNTAX_NODE_LEASE");
    session->addon_data->register_node_lease(lease->id(), lease);
    SyntaxSearchSource root;
    root.grammar_id = session->language_id;
    root.syntax = session->published_syntax;
    sources.push_back(std::move(root));
    if (impl_->published_index &&
        impl_->published_index->tags.syntax_revision == tags.syntax_revision &&
        impl_->published_index->tags.language_generation ==
            tags.language_generation) {
      for (const InjectionLayerRecord &layer : impl_->published_index->layers) {
        if (!layer.syntax_parsed || !layer.syntax)
          continue;
        SyntaxSearchSource source;
        source.layer_id = layer.layer_id;
        source.depth = layer.depth;
        source.grammar_id = layer.language_id;
        source.syntax = layer.syntax;
        for (const InjectionRangeRecord &included : layer.ranges)
          source.ranges.emplace_back(included.start_index,
                                     included.end_index);
        sources.push_back(std::move(source));
      }
    }
  }

  const uint64_t start = analysis_offset_for_point(*analysis, range.start);
  const uint64_t end = analysis_offset_for_point(*analysis, range.end);
  std::vector<SyntaxSearchCandidate> candidates;
  for (const SyntaxSearchSource &source : sources) {
    if (!source.syntax || !source_contains_range(source, start, end))
      continue;
    const TSNode node = node_containing_range(*source.syntax, start, end);
    if (ts_node_is_null(node))
      continue;
    const uint32_t handle = source.syntax->handle(node);
    if (handle == 0)
      continue;
    candidates.push_back(SyntaxSearchCandidate{
        source, handle, end_index_for(node) - start_index_for(node)});
  }
  sort_search_candidates(&candidates);
  return syntax_search_response(env, tags, true, session, lease, candidates);
}

Napi::Value InjectionEngine::apply_result_batch(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &session) {
  if (info.Length() == 0 || !info[0].IsObject())
    return throw_type_error(info.Env(),
                            "applyInjectionResultBatch expects a batch",
                            "ERR_INVALID_INJECTION_RESULT");
  return apply_result_object(info.Env(), info[0].As<Napi::Object>(), session);
}

Napi::Value InjectionEngine::apply_results(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &session) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !info[0].IsObject())
    return throw_type_error(env, "applyInjectionResults expects a payload",
                            "ERR_INVALID_INJECTION_RESULT");
  Napi::Object source = info[0].As<Napi::Object>();
  Napi::Object batch = Napi::Object::New(env);
  Napi::Array names = source.GetPropertyNames();
  for (uint32_t index = 0; index < names.Length(); index++) {
    Napi::Value name = names.Get(index);
    batch.Set(name, source.Get(name));
  }
  batch.Set("batchIndex", Napi::Number::New(env, 0));
  batch.Set("isFinal", Napi::Boolean::New(env, true));
  return apply_result_object(env, batch, session);
}

Napi::Value InjectionEngine::apply_result_object(
    Napi::Env env, Napi::Object batch,
    const std::shared_ptr<SessionState> &session) {
  InjectionRevisionTags tags;
  uint64_t request_id = 0;
  uint64_t generation = 0;
  uint32_t batch_index = 0;
  if (!read_tags(batch, &tags) ||
      !read_uint64(batch.Get("requestId"), &request_id) ||
      !read_uint64(batch.Get("injectionPointGeneration"), &generation) ||
      !read_uint32(batch.Get("batchIndex"), &batch_index) ||
      !batch.Get("isFinal").IsBoolean() ||
      !batch.Get("results").IsArray())
    return throw_type_error(env, "Injection result batch fields are invalid",
                            "ERR_INVALID_INJECTION_RESULT");
  const bool is_final = batch.Get("isFinal").As<Napi::Boolean>().Value();

  std::lock_guard<std::mutex> lock(session->mutex);
  auto request_found = impl_->requests.find(request_id);
  if (request_found == impl_->requests.end() ||
      !current_tags_locked(*session, tags) ||
      !tags_equal(request_found->second.tags, tags) ||
      request_found->second.injection_point_generation != generation) {
    impl_->stale_request_count++;
    return stale_result(env, tags, "stale-results", request_id);
  }
  InjectionRequestState &request = request_found->second;
  if (request.final_received || request.next_batch_index != batch_index)
    return throw_error(env, "Injection result batches are out of order",
                       "ERR_INJECTION_BATCH_ORDER");
  if (!session->syntax_lease || !session->syntax_analysis ||
      session->syntax_revision != tags.syntax_revision)
    return stale_result(env, tags, "snapshot-unavailable", request_id);
  SnapshotReader reader(session->syntax_lease);
  if (!reader.valid())
    return throw_error(env, "Unable to read injection snapshot",
                       "ERR_INJECTION_SNAPSHOT");

  Napi::Array results = batch.Get("results").As<Napi::Array>();
  for (uint32_t result_index = 0; result_index < results.Length();
       result_index++) {
    Napi::Value result_value = results.Get(result_index);
    if (!result_value.IsObject())
      return throw_type_error(env, "Injection results must be objects",
                              "ERR_INVALID_INJECTION_RESULT");
    Napi::Object result = result_value.As<Napi::Object>();
    uint32_t candidate_id = 0;
    uint32_t injection_point_id = 0;
    if (!read_uint32(result.Get("candidateId"), &candidate_id) ||
        !read_uint32(result.Get("injectionPointId"), &injection_point_id) ||
        !request.valid_candidate_ids.contains(candidate_id) ||
        !result.Get("parentGrammarId").IsString() ||
        !result.Get("languageName").IsString() ||
        !result.Get("content").IsArray())
      return throw_type_error(env, "Injection result identity is invalid",
                              "ERR_INVALID_INJECTION_RESULT");
    auto candidate_found = request.candidates.find(candidate_id);
    if (candidate_found == request.candidates.end())
      return throw_error(env, "Injection candidate no longer has a syntax node",
                         "ERR_STALE_INJECTION_NODE");
    const InjectionCandidateRecord &candidate_record = candidate_found->second;
    if (candidate_record.parent_layer_id != 0) {
      const auto current_parent = std::find_if(
          request.staged_index->layers.begin(),
          request.staged_index->layers.end(),
          [&](const InjectionLayerRecord &candidate) {
            return candidate.layer_id == candidate_record.parent_layer_id &&
                   candidate.reuse_slot_id ==
                       candidate_record.parent_reuse_slot_id;
          });
      if (current_parent == request.staged_index->layers.end())
        continue;
    }
    ParsedGrammarDescriptor grammar;
    if (!parse_grammar_descriptor(env, result.Get("grammar"), &grammar))
      return env.Undefined();

    std::vector<NodeRangeSpec> content;
    Napi::Array content_array = result.Get("content").As<Napi::Array>();
    content.reserve(content_array.Length());
    for (uint32_t content_index = 0; content_index < content_array.Length();
         content_index++) {
      NodeRangeSpec spec;
      if (!parse_content_spec(env, content_array.Get(content_index),
                               *session->syntax_analysis, &spec))
        return env.Undefined();
      content.push_back(std::move(spec));
    }

    const bool include_children =
        result.Get("includeChildren").ToBoolean().Value();
    std::vector<InjectionRangeRecord> ranges =
        included_ranges(content, include_children, *session->syntax_analysis);
    if (result.Get("includeAdjacentWhitespace").ToBoolean().Value())
      merge_adjacent_whitespace(ranges, reader, *session->syntax_analysis);
    if (ranges.empty())
      continue;

    InjectionLayerRecord layer;
    layer.parent_layer_id = candidate_record.parent_layer_id;
    layer.parent_reuse_slot_id = candidate_record.parent_reuse_slot_id;
    layer.depth = candidate_record.depth + 1;
    layer.candidate_id = candidate_id;
    layer.injection_point_id = injection_point_id;
    layer.parent_grammar_id =
        result.Get("parentGrammarId").As<Napi::String>().Utf8Value();
    layer.language_name =
        result.Get("languageName").As<Napi::String>().Utf8Value();
    layer.language_id = std::move(grammar.language_id);
    layer.runtime = std::move(grammar.runtime);
    layer.wasm_path = std::move(grammar.wasm_path);
    layer.language_name_export = std::move(grammar.language_name);
    layer.language_segment = std::move(grammar.language_segment);
    layer.query_paths = std::move(grammar.query_paths);
    layer.include_children = include_children;
    layer.include_adjacent_whitespace =
        result.Get("includeAdjacentWhitespace").ToBoolean().Value();
    layer.newlines_between =
        result.Get("newlinesBetween").ToBoolean().Value();
    layer.cover_shallower_scopes =
        result.Get("coverShallowerScopes").ToBoolean().Value();
    layer.query_defined = result.Get("queryDefined").ToBoolean().Value();
    layer.ranges = std::move(ranges);

    layer.layer_id = impl_->next_layer_id++;
    layer.reuse_slot_id = impl_->next_reuse_slot_id++;

    const bool dynamic_scope =
        result.Get("languageScopeIsDynamic").ToBoolean().Value();
    if (!dynamic_scope) {
      const std::vector<std::string> scopes =
          parse_scopes(result.Get("languageScope"));
      for (InjectionRangeRecord &range : layer.ranges)
        range.scopes = scopes;
    }
    const size_t layer_index = request.staged_index->layers.size();
    request.staged_index->layers.push_back(std::move(layer));
    if (dynamic_scope) {
      request.scope_targets[{candidate_id, injection_point_id}] =
          PendingScopeTarget{layer_index};
    }
  }

  request.next_batch_index++;
  request.final_received = is_final;
  Napi::Object response = Napi::Object::New(env);
  response.Set("accepted", Napi::Boolean::New(env, true));
  set_tags(response, tags);
  response.Set("requestId", Napi::Number::New(env, request_id));
  if (!is_final)
    return response;

  if (request.scope_targets.empty()) {
    return queue_child_parse(env, request_id, session);
  }

  request.awaiting_language_scopes = true;
  Napi::Object scope_batch = Napi::Object::New(env);
  set_tags(scope_batch, tags);
  scope_batch.Set("requestId", Napi::Number::New(env, request_id));
  scope_batch.Set("injectionPointGeneration",
                  Napi::Number::New(env, generation));
  Napi::Array entries = Napi::Array::New(env, request.scope_targets.size());
  uint32_t entry_index = 0;
  for (const auto &[identity, target] : request.scope_targets) {
    const InjectionLayerRecord &layer =
        request.staged_index->layers[target.layer_index];
    Napi::Object entry = Napi::Object::New(env);
    entry.Set("candidateId", Napi::Number::New(env, identity.first));
    entry.Set("injectionPointId", Napi::Number::New(env, identity.second));
    entry.Set("languageName", Napi::String::New(env, layer.language_name));
    Napi::Array ranges = Napi::Array::New(env, layer.ranges.size());
    for (uint32_t range_index = 0; range_index < layer.ranges.size();
         range_index++) {
      const InjectionRangeRecord &range = layer.ranges[range_index];
      Napi::Object range_object_value = Napi::Object::New(env);
      range_object_value.Set(
          "start", point_object(env, range.start.row, range.start.column));
      range_object_value.Set("end",
                             point_object(env, range.end.row, range.end.column));
      ranges.Set(range_index, range_object_value);
    }
    entry.Set("ranges", ranges);
    entries.Set(entry_index++, entry);
  }
  scope_batch.Set("entries", entries);
  response.Set("languageScopeBatch", scope_batch);
  return response;
}

Napi::Value InjectionEngine::apply_language_scopes(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &session) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !info[0].IsObject())
    return throw_type_error(
        env, "applyInjectionLanguageScopes expects a payload",
        "ERR_INVALID_INJECTION_SCOPE_RESULT");
  Napi::Object payload = info[0].As<Napi::Object>();
  InjectionRevisionTags tags;
  uint64_t request_id = 0;
  uint64_t generation = 0;
  if (!read_tags(payload, &tags) ||
      !read_uint64(payload.Get("requestId"), &request_id) ||
      !read_uint64(payload.Get("injectionPointGeneration"), &generation) ||
      !payload.Get("results").IsArray())
    return throw_type_error(env, "Injection scope result fields are invalid",
                            "ERR_INVALID_INJECTION_SCOPE_RESULT");

  std::lock_guard<std::mutex> lock(session->mutex);
  auto request_found = impl_->requests.find(request_id);
  if (request_found == impl_->requests.end() ||
      !current_tags_locked(*session, tags) ||
      !tags_equal(request_found->second.tags, tags) ||
      request_found->second.injection_point_generation != generation ||
      !request_found->second.awaiting_language_scopes)
    return stale_result(env, tags, "stale-scope-results", request_id);
  InjectionRequestState &request = request_found->second;
  Napi::Array results = payload.Get("results").As<Napi::Array>();
  std::set<std::pair<uint32_t, uint32_t>> applied;
  for (uint32_t index = 0; index < results.Length(); index++) {
    Napi::Value result_value = results.Get(index);
    if (!result_value.IsObject())
      return throw_type_error(env, "Injection scope results must be objects",
                              "ERR_INVALID_INJECTION_SCOPE_RESULT");
    Napi::Object result = result_value.As<Napi::Object>();
    uint32_t candidate_id = 0;
    uint32_t injection_point_id = 0;
    if (!read_uint32(result.Get("candidateId"), &candidate_id) ||
        !read_uint32(result.Get("injectionPointId"), &injection_point_id) ||
        !result.Get("scopes").IsArray())
      return throw_type_error(env, "Injection scope identity is invalid",
                              "ERR_INVALID_INJECTION_SCOPE_RESULT");
    const auto identity = std::make_pair(candidate_id, injection_point_id);
    auto target_found = request.scope_targets.find(identity);
    if (target_found == request.scope_targets.end())
      return throw_error(env, "Injection scope target is not pending",
                         "ERR_INVALID_INJECTION_SCOPE_RESULT");
    InjectionLayerRecord &layer =
        request.staged_index->layers[target_found->second.layer_index];
    Napi::Array scopes = result.Get("scopes").As<Napi::Array>();
    if (scopes.Length() != layer.ranges.size())
      return throw_error(env, "Injection scope count does not match ranges",
                         "ERR_INVALID_INJECTION_SCOPE_RESULT");
    for (uint32_t range_index = 0; range_index < scopes.Length();
         range_index++)
      layer.ranges[range_index].scopes = parse_scopes(scopes.Get(range_index));
    applied.insert(identity);
  }
  if (applied.size() != request.scope_targets.size())
    return throw_error(env, "Injection scope results are incomplete",
                       "ERR_INVALID_INJECTION_SCOPE_RESULT");

  return queue_child_parse(env, request_id, session);
}

Napi::Value InjectionEngine::apply_query_language_descriptors(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &session) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !info[0].IsObject())
    return throw_type_error(
        env, "applyQueryLanguageDescriptors expects a payload",
        "ERR_INVALID_INJECTION_RESULT");
  Napi::Object payload = info[0].As<Napi::Object>();
  InjectionRevisionTags tags;
  uint64_t request_id = 0;
  uint64_t generation = 0;
  if (!read_tags(payload, &tags) ||
      !read_uint64(payload.Get("requestId"), &request_id) ||
      !read_uint64(payload.Get("injectionPointGeneration"), &generation) ||
      !payload.Get("descriptors").IsArray())
    return throw_type_error(env, "Query language descriptor payload is invalid",
                            "ERR_INVALID_INJECTION_RESULT");

  struct Resolution {
    std::string alias;
    bool recognized = false;
    ParsedGrammarDescriptor grammar;
  };
  std::vector<Resolution> resolutions;
  std::set<std::string> seen;
  Napi::Array descriptors = payload.Get("descriptors").As<Napi::Array>();
  for (uint32_t index = 0; index < descriptors.Length(); index++) {
    Napi::Value value = descriptors.Get(index);
    if (!value.IsObject())
      return throw_type_error(env, "Query language descriptor must be an object",
                              "ERR_INVALID_INJECTION_RESULT");
    Napi::Object item = value.As<Napi::Object>();
    if (!item.Get("alias").IsString())
      return throw_type_error(env, "Query language alias must be a string",
                              "ERR_INVALID_INJECTION_RESULT");
    Resolution resolution;
    resolution.alias = item.Get("alias").As<Napi::String>().Utf8Value();
    if (resolution.alias.empty() || !seen.insert(resolution.alias).second)
      return throw_type_error(env, "Query language aliases must be unique",
                              "ERR_INVALID_INJECTION_RESULT");
    Napi::Value grammar = item.Get("grammar");
    if (!grammar.IsNull() && !grammar.IsUndefined()) {
      if (!parse_grammar_descriptor(env, grammar, &resolution.grammar))
        return env.Undefined();
      if (resolution.grammar.runtime != "wasm" ||
          resolution.grammar.wasm_path.empty())
        return throw_type_error(
            env, "Query language grammar must provide a Wasm descriptor",
            "ERR_INVALID_INJECTION_RESULT");
      resolution.recognized = true;
    }
    resolutions.push_back(std::move(resolution));
  }

  std::lock_guard<std::mutex> lock(session->mutex);
  const auto request_found = impl_->requests.find(request_id);
  if (request_found == impl_->requests.end() ||
      !tags_equal(request_found->second.tags, tags) ||
      request_found->second.injection_point_generation != generation ||
      !current_tags_locked(*session, tags))
    return stale_result(env, tags, "stale-query-language-results",
                        request_id);
  InjectionRequestState &request = request_found->second;
  if (!request.staged_index || request.child_parse_active)
    return stale_result(env, tags, "stale-query-language-results",
                        request_id);

  std::set<std::string> resolved_aliases;
  std::set<std::string> rejected_aliases;
  for (const Resolution &resolution : resolutions) {
    if (resolution.recognized) {
      impl_->query_grammars[resolution.alias] = resolution.grammar;
      impl_->query_rejected_aliases.erase(resolution.alias);
      resolved_aliases.insert(resolution.alias);
      impl_->query_language_resolution_count++;
    } else {
      impl_->query_grammars.erase(resolution.alias);
      impl_->query_rejected_aliases.insert(resolution.alias);
      rejected_aliases.insert(resolution.alias);
      impl_->query_language_rejection_count++;
    }
  }
  auto &layers = request.staged_index->layers;
  for (InjectionLayerRecord &layer : layers) {
    if (!layer.query_defined)
      continue;
    const auto grammar = impl_->query_grammars.find(layer.language_name);
    if (grammar != impl_->query_grammars.end()) {
      if (layer.syntax_parsed && !same_query_grammar(layer, grammar->second)) {
        layer.layer_id = impl_->next_layer_id++;
        layer.reuse_slot_id = impl_->next_reuse_slot_id++;
        layer.reuse_with_edits = false;
        layer.syntax_parsed = false;
        layer.syntax_root_has_error = false;
        layer.syntax_error_code.clear();
        layer.syntax_error_message.clear();
        layer.syntax.reset();
        layer.queries.reset();
      }
      apply_query_grammar(layer, grammar->second);
    }
  }
  layers.erase(
      std::remove_if(layers.begin(), layers.end(),
                     [&](const InjectionLayerRecord &layer) {
                       return layer.query_defined &&
                              rejected_aliases.contains(layer.language_name);
                     }),
      layers.end());
  finalize_index_counts(*request.staged_index);

  Napi::Object response = Napi::Object::New(env);
  response.Set("accepted", Napi::Boolean::New(env, true));
  set_tags(response, tags);
  response.Set("requestId", Napi::Number::New(env, request_id));
  response.Set("injectionPointGeneration",
               Napi::Number::New(env, generation));
  Napi::Array resolved = Napi::Array::New(env, resolved_aliases.size());
  uint32_t resolved_index = 0;
  for (const std::string &alias : resolved_aliases)
    resolved.Set(resolved_index++, Napi::String::New(env, alias));
  response.Set("resolvedAliases", resolved);
  Napi::Array rejected = Napi::Array::New(env, rejected_aliases.size());
  uint32_t rejected_index = 0;
  for (const std::string &alias : rejected_aliases)
    rejected.Set(rejected_index++, Napi::String::New(env, alias));
  response.Set("rejectedAliases", rejected);
  return response;
}

Napi::Value InjectionEngine::abort_request(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &session) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !info[0].IsObject())
    return throw_type_error(env, "abortInjectionRequest expects a request",
                            "ERR_INVALID_INJECTION_REQUEST");
  Napi::Object request = info[0].As<Napi::Object>();
  uint64_t request_id = 0;
  if (!read_uint64(request.Get("requestId"), &request_id))
    return throw_type_error(env, "Injection requestId is invalid",
                            "ERR_INVALID_INJECTION_REQUEST");
  std::lock_guard<std::mutex> lock(session->mutex);
  const bool removed = impl_->requests.erase(request_id) != 0;
  if (removed) {
    impl_->aborted_request_count++;
    impl_->candidate_count = 0;
  }
  Napi::Object response = Napi::Object::New(env);
  response.Set("accepted", Napi::Boolean::New(env, removed));
  response.Set("requestId", Napi::Number::New(env, request_id));
  if (!removed)
    response.Set("reason", Napi::String::New(env, "stale-request"));
  return response;
}

struct PublishedSyntaxSnapshot::Impl {
  TSTree *tree = nullptr;
  std::shared_ptr<const NativeQueryEngine> query_engine;
  uint64_t identity = 0;
  uint64_t buffer_revision = 0;
  uint64_t language_generation = 0;
  mutable std::mutex mutex;
  mutable std::vector<TSNode> nodes;
  mutable std::unordered_map<NodeKey, uint32_t, NodeKeyHash> handles;
  mutable std::map<std::string, std::vector<uint32_t>> handles_by_type;
  mutable std::set<std::string> indexed_types;

  ~Impl() { ts_tree_delete(tree); }

  TSNode node(uint32_t handle) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (handle == 0 || handle > nodes.size())
      return TSNode{};
    return nodes[handle - 1];
  }

  uint32_t handle(TSNode node_value) const {
    if (ts_node_is_null(node_value))
      return 0;
    std::lock_guard<std::mutex> lock(mutex);
    auto found = handles.find(node_key(node_value));
    if (found != handles.end())
      return found->second;
    if (nodes.size() >= std::numeric_limits<uint32_t>::max())
      return 0;
    const uint32_t result = static_cast<uint32_t>(nodes.size() + 1);
    nodes.push_back(node_value);
    handles.emplace(node_key(node_value), result);
    return result;
  }

  uint32_t handle_for_range(uint32_t start_index, uint32_t end_index,
                            uint32_t symbol) const {
    const uint32_t start_byte = start_index * 2u;
    const uint32_t end_byte = end_index * 2u;
    TSNode candidate = ts_node_descendant_for_byte_range(
        ts_tree_root_node(tree), start_byte, end_byte);
    while (!ts_node_is_null(candidate)) {
      if (ts_node_start_byte(candidate) == start_byte &&
          ts_node_end_byte(candidate) == end_byte &&
          static_cast<uint32_t>(ts_node_symbol(candidate)) == symbol)
        return handle(candidate);
      candidate = ts_node_parent(candidate);
    }
    return 0;
  }
};

PublishedSyntaxSnapshot::PublishedSyntaxSnapshot(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

PublishedSyntaxSnapshot::~PublishedSyntaxSnapshot() = default;

std::shared_ptr<const PublishedSyntaxSnapshot> PublishedSyntaxSnapshot::build(
    const TSTree *tree,
    std::shared_ptr<const NativeQueryEngine> query_engine,
    uint64_t buffer_revision, uint64_t language_generation) {
  if (tree == nullptr)
    return nullptr;
  auto impl = std::make_unique<Impl>();
  impl->tree = ts_tree_copy(tree);
  if (impl->tree == nullptr)
    return nullptr;
  impl->query_engine = std::move(query_engine);
  impl->identity =
      next_published_syntax_identity.fetch_add(1, std::memory_order_relaxed);
  impl->buffer_revision = buffer_revision;
  impl->language_generation = language_generation;
  return std::shared_ptr<const PublishedSyntaxSnapshot>(
      new PublishedSyntaxSnapshot(std::move(impl)));
}

uint64_t PublishedSyntaxSnapshot::buffer_revision() const {
  return impl_->buffer_revision;
}

uint64_t PublishedSyntaxSnapshot::language_generation() const {
  return impl_->language_generation;
}

uint64_t PublishedSyntaxSnapshot::identity() const { return impl_->identity; }

uint64_t PublishedSyntaxSnapshot::node_count() const {
  return ts_node_descendant_count(ts_tree_root_node(impl_->tree));
}

const TSTree *PublishedSyntaxSnapshot::tree() const { return impl_->tree; }

std::shared_ptr<const NativeQueryEngine>
PublishedSyntaxSnapshot::query_engine() const {
  return impl_->query_engine;
}

TSNode PublishedSyntaxSnapshot::node(uint32_t handle) const {
  return impl_->node(handle);
}

uint32_t PublishedSyntaxSnapshot::handle(TSNode node_value) const {
  return impl_->handle(node_value);
}

bool PublishedSyntaxSnapshot::collect_handles_for_types(
    const std::set<std::string> &types,
    std::map<std::string, std::vector<uint32_t>> &result,
    SyntaxSnapshotCancellationFunction cancellation,
    void *cancellation_payload) const {
  std::set<std::string> missing;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const std::string &type : types) {
      if (impl_->indexed_types.contains(type))
        result[type] = impl_->handles_by_type[type];
      else
        missing.insert(type);
    }
  }
  if (missing.empty())
    return true;

  const TSLanguage *language = ts_tree_language(impl_->tree);
  std::unordered_map<TSSymbol, std::vector<const std::string *>>
      missing_by_symbol;
  missing_by_symbol.reserve(missing.size() * 2);
  for (const std::string &type : missing) {
    for (bool named : {false, true}) {
      const TSSymbol symbol = ts_language_symbol_for_name(
          language, type.data(), static_cast<uint32_t>(type.size()), named);
      if (symbol != 0)
        missing_by_symbol[symbol].push_back(&type);
    }
  }

  std::map<std::string, std::vector<TSNode>> found_nodes;
  std::vector<TSNode> pending;
  pending.reserve(64);
  pending.push_back(ts_tree_root_node(impl_->tree));
  uint64_t visited = 0;
  while (!pending.empty()) {
    if ((visited++ & UINT64_C(4095)) == 0 && cancellation != nullptr &&
        cancellation(cancellation_payload))
      return false;
    const TSNode node_value = pending.back();
    pending.pop_back();
    const auto requested = missing_by_symbol.find(ts_node_symbol(node_value));
    if (requested != missing_by_symbol.end()) {
      const std::string_view actual_type = ts_node_type(node_value);
      for (const std::string *type : requested->second) {
        if (actual_type == *type)
          found_nodes[*type].push_back(node_value);
      }
    }
    const uint32_t count = ts_node_child_count(node_value);
    for (uint32_t index = count; index > 0; index--)
      pending.push_back(ts_node_child(node_value, index - 1));
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  size_t new_handle_count = 0;
  for (const auto &[_, nodes] : found_nodes)
    new_handle_count += nodes.size();
  impl_->nodes.reserve(impl_->nodes.size() + new_handle_count);
  impl_->handles.reserve(impl_->handles.size() + new_handle_count);
  for (const std::string &type : missing) {
    if (impl_->indexed_types.contains(type)) {
      result[type] = impl_->handles_by_type[type];
      continue;
    }
    std::vector<uint32_t> &cached = impl_->handles_by_type[type];
    for (TSNode node_value : found_nodes[type]) {
      const NodeKey key = node_key(node_value);
      auto existing = impl_->handles.find(key);
      uint32_t handle = 0;
      if (existing != impl_->handles.end()) {
        handle = existing->second;
      } else {
        if (impl_->nodes.size() >= std::numeric_limits<uint32_t>::max())
          return false;
        handle = static_cast<uint32_t>(impl_->nodes.size() + 1);
        impl_->nodes.push_back(node_value);
        impl_->handles.emplace(key, handle);
      }
      cached.push_back(handle);
    }
    impl_->indexed_types.insert(type);
    result[type] = cached;
  }
  return true;
}

uint32_t PublishedSyntaxSnapshot::handle_for_range(uint32_t start_index,
                                                   uint32_t end_index,
                                                   uint32_t symbol) const {
  return impl_->handle_for_range(start_index, end_index, symbol);
}

namespace {

bool node_is_current(
    const std::weak_ptr<SessionState> &weak_session,
    const std::shared_ptr<const PublishedSyntaxSnapshot> &tree,
    const InjectionRevisionTags &expected) {
  std::shared_ptr<SessionState> session = weak_session.lock();
  if (!session)
    return false;
  std::lock_guard<std::mutex> lock(session->mutex);
  return !session->destroyed &&
         session->buffer_revision == expected.buffer_revision &&
         session->syntax_revision == expected.syntax_revision &&
         session->language_generation == expected.language_generation &&
         session->injection_engine &&
         session->injection_engine->owns_syntax_snapshot(tree, expected);
}

Napi::Value stale_node(Napi::Env env) {
  return throw_error(env, "Syntax node belongs to a stale syntax revision",
                     "ERR_STALE_INJECTION_NODE");
}

Napi::Object node_object(
    Napi::Env env, const std::weak_ptr<SessionState> &session,
    const std::shared_ptr<const PublishedSyntaxSnapshot> &tree, TSNode node,
    const InjectionRevisionTags &tags,
    const std::shared_ptr<SnapshotLeaseReference> &lease) {
  const uint32_t handle = tree->handle(node);
  if (handle == 0)
    return Napi::Object::New(env);
  return InjectionNodeWrapper::new_instance(env, session, tree, handle, tags,
                                             lease);
}

} // namespace

Napi::Function InjectionNodeWrapper::init(Napi::Env env) {
  return DefineClass(
      env, "InjectionNode",
      {InstanceAccessor<&InjectionNodeWrapper::get_id>("id"),
       InstanceAccessor<&InjectionNodeWrapper::get_type>("type"),
       InstanceAccessor<&InjectionNodeWrapper::get_text>("text"),
       InstanceAccessor<&InjectionNodeWrapper::get_start_index>("startIndex"),
       InstanceAccessor<&InjectionNodeWrapper::get_end_index>("endIndex"),
       InstanceAccessor<&InjectionNodeWrapper::get_start_position>(
           "startPosition"),
       InstanceAccessor<&InjectionNodeWrapper::get_end_position>(
           "endPosition"),
       InstanceAccessor<&InjectionNodeWrapper::get_range>("range"),
       InstanceAccessor<&InjectionNodeWrapper::get_parent>("parent"),
       InstanceAccessor<&InjectionNodeWrapper::get_children>("children"),
       InstanceAccessor<&InjectionNodeWrapper::get_named_children>(
           "namedChildren"),
       InstanceAccessor<&InjectionNodeWrapper::get_child_count>("childCount"),
       InstanceAccessor<&InjectionNodeWrapper::get_named_child_count>(
           "namedChildCount"),
       InstanceAccessor<&InjectionNodeWrapper::get_first_child>("firstChild"),
       InstanceAccessor<&InjectionNodeWrapper::get_last_child>("lastChild"),
       InstanceAccessor<&InjectionNodeWrapper::get_first_named_child>(
           "firstNamedChild"),
       InstanceAccessor<&InjectionNodeWrapper::get_last_named_child>(
           "lastNamedChild"),
       InstanceAccessor<&InjectionNodeWrapper::get_previous_sibling>(
           "previousSibling"),
       InstanceAccessor<&InjectionNodeWrapper::get_next_sibling>(
           "nextSibling"),
       InstanceAccessor<&InjectionNodeWrapper::get_previous_named_sibling>(
           "previousNamedSibling"),
       InstanceAccessor<&InjectionNodeWrapper::get_next_named_sibling>(
           "nextNamedSibling"),
       InstanceAccessor<&InjectionNodeWrapper::get_is_named>("isNamed"),
       InstanceAccessor<&InjectionNodeWrapper::get_is_missing>("isMissing"),
       InstanceAccessor<&InjectionNodeWrapper::get_is_error>("isError"),
       InstanceAccessor<&InjectionNodeWrapper::get_has_error>("hasError"),
       InstanceMethod<&InjectionNodeWrapper::is_current>("isCurrent"),
       InstanceMethod<&InjectionNodeWrapper::assert_current>("assertCurrent"),
       InstanceMethod<&InjectionNodeWrapper::child>("child"),
       InstanceMethod<&InjectionNodeWrapper::named_child>("namedChild"),
       InstanceMethod<&InjectionNodeWrapper::child_for_field_name>(
           "childForFieldName"),
       InstanceMethod<&InjectionNodeWrapper::descendants_of_type>(
           "descendantsOfType"),
       InstanceMethod<&InjectionNodeWrapper::get_snapshot_lease_id>(
           "_snapshotLeaseId"),
       InstanceMethod<&InjectionNodeWrapper::release_snapshot_lease>(
           "_releaseSnapshotLease")});
}

InjectionNodeWrapper::InjectionNodeWrapper(const Napi::CallbackInfo &info)
    : Napi::ObjectWrap<InjectionNodeWrapper>(info) {}

Napi::Object InjectionNodeWrapper::new_instance(
    Napi::Env env, const std::weak_ptr<SessionState> &session,
    std::shared_ptr<const PublishedSyntaxSnapshot> tree, uint32_t node_handle,
    const InjectionRevisionTags &tags,
    const SuperstringSnapshotLease *lease) {
  std::shared_ptr<SnapshotLeaseReference> reference =
      SnapshotLeaseReference::acquire(lease);
  if (!reference) {
    throw_error(env, "Unable to retain snapshot for syntax node handle",
                "ERR_SYNTAX_NODE_LEASE");
    return Napi::Object::New(env);
  }
  if (std::shared_ptr<SessionState> state = session.lock()) {
    if (state->addon_data != nullptr)
      state->addon_data->register_node_lease(reference->id(), reference);
  }
  return new_instance(env, session, std::move(tree), node_handle, tags,
                      std::move(reference));
}

Napi::Object InjectionNodeWrapper::new_instance(
    Napi::Env env, const std::weak_ptr<SessionState> &session,
    std::shared_ptr<const PublishedSyntaxSnapshot> tree, uint32_t node_handle,
    const InjectionRevisionTags &tags,
    std::shared_ptr<SnapshotLeaseReference> lease) {
  AddonData *data = env.GetInstanceData<AddonData>();
  Napi::Object object = data->injection_node_constructor.New({});
  auto *wrapper = Napi::ObjectWrap<InjectionNodeWrapper>::Unwrap(object);
  wrapper->session_ = session;
  wrapper->tree_ = std::move(tree);
  wrapper->node_handle_ = node_handle;
  wrapper->tags_ = tags;
  wrapper->lease_ = std::move(lease);
  return object;
}

#define READ_NODE_OR_THROW()                                                  \
  TSNode node = tree_ ? tree_->node(node_handle_) : TSNode{};                 \
  if (ts_node_is_null(node))                                                  \
    return throw_error(info.Env(), "Syntax node handle is invalid",          \
                       "ERR_INVALID_INJECTION_NODE_HANDLE")

Napi::Value InjectionNodeWrapper::get_id(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  return Napi::Number::New(info.Env(), node_handle_);
}

Napi::Value InjectionNodeWrapper::get_type(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  return Napi::String::New(info.Env(), ts_node_type(node));
}

Napi::Value InjectionNodeWrapper::get_text(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  SnapshotReader reader(lease_ ? lease_->lease() : nullptr);
  std::u16string text;
  const uint64_t start = start_index_for(node);
  const uint64_t end = end_index_for(node);
  if (!reader.valid() || !reader.append_range(start, end, text))
    return throw_error(info.Env(), "Unable to read syntax node text",
                       "ERR_INJECTION_NODE_TEXT");
  return Napi::String::New(info.Env(), text.data(), text.size());
}

Napi::Value
InjectionNodeWrapper::get_start_index(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  return Napi::Number::New(info.Env(), start_index_for(node));
}

Napi::Value InjectionNodeWrapper::get_end_index(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  return Napi::Number::New(info.Env(), end_index_for(node));
}

Napi::Value
InjectionNodeWrapper::get_start_position(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  const Point point = point_from_ts(ts_node_start_point(node));
  return point_object(info.Env(), point.row, point.column);
}

Napi::Value
InjectionNodeWrapper::get_end_position(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  const Point point = point_from_ts(ts_node_end_point(node));
  return point_object(info.Env(), point.row, point.column);
}

Napi::Value InjectionNodeWrapper::get_range(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  return range_object(info.Env(), node);
}

Napi::Value InjectionNodeWrapper::get_parent(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  const TSNode parent = ts_node_parent(node);
  if (ts_node_is_null(parent))
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_, parent, tags_, lease_);
}

Napi::Value InjectionNodeWrapper::get_children(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  const uint32_t count = ts_node_child_count(node);
  Napi::Array children = Napi::Array::New(info.Env(), count);
  for (uint32_t index = 0; index < count; index++)
    children.Set(index, node_object(info.Env(), session_, tree_,
                                    ts_node_child(node, index), tags_, lease_));
  return children;
}

Napi::Value
InjectionNodeWrapper::get_named_children(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  const uint32_t count = ts_node_named_child_count(node);
  Napi::Array children = Napi::Array::New(info.Env(), count);
  for (uint32_t index = 0; index < count; index++)
    children.Set(index,
                 node_object(info.Env(), session_, tree_,
                             ts_node_named_child(node, index), tags_, lease_));
  return children;
}

Napi::Value
InjectionNodeWrapper::get_child_count(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  return Napi::Number::New(info.Env(), ts_node_child_count(node));
}

Napi::Value
InjectionNodeWrapper::get_named_child_count(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  return Napi::Number::New(info.Env(), ts_node_named_child_count(node));
}

Napi::Value
InjectionNodeWrapper::get_first_child(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  if (ts_node_child_count(node) == 0)
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_, ts_node_child(node, 0),
                     tags_, lease_);
}

Napi::Value InjectionNodeWrapper::get_last_child(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  const uint32_t count = ts_node_child_count(node);
  if (count == 0)
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_, ts_node_child(node, count - 1),
                     tags_, lease_);
}

Napi::Value InjectionNodeWrapper::get_first_named_child(
    const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  if (ts_node_named_child_count(node) == 0)
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_, ts_node_named_child(node, 0),
                     tags_, lease_);
}

Napi::Value InjectionNodeWrapper::get_last_named_child(
    const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  const uint32_t count = ts_node_named_child_count(node);
  if (count == 0)
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_,
                     ts_node_named_child(node, count - 1), tags_, lease_);
}

Napi::Value
InjectionNodeWrapper::get_previous_sibling(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  const TSNode sibling = ts_node_prev_sibling(node);
  if (ts_node_is_null(sibling))
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_, sibling, tags_, lease_);
}

Napi::Value
InjectionNodeWrapper::get_next_sibling(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  const TSNode sibling = ts_node_next_sibling(node);
  if (ts_node_is_null(sibling))
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_, sibling, tags_, lease_);
}

Napi::Value InjectionNodeWrapper::get_previous_named_sibling(
    const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  const TSNode sibling = ts_node_prev_named_sibling(node);
  if (ts_node_is_null(sibling))
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_, sibling, tags_, lease_);
}

Napi::Value InjectionNodeWrapper::get_next_named_sibling(
    const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  const TSNode sibling = ts_node_next_named_sibling(node);
  if (ts_node_is_null(sibling))
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_, sibling, tags_, lease_);
}

Napi::Value InjectionNodeWrapper::get_is_named(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  return Napi::Boolean::New(info.Env(), ts_node_is_named(node));
}

Napi::Value
InjectionNodeWrapper::get_is_missing(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  return Napi::Boolean::New(info.Env(), ts_node_is_missing(node));
}

Napi::Value InjectionNodeWrapper::get_is_error(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  return Napi::Boolean::New(info.Env(), ts_node_is_error(node));
}

Napi::Value InjectionNodeWrapper::get_has_error(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  return Napi::Boolean::New(info.Env(), ts_node_has_error(node));
}

Napi::Value InjectionNodeWrapper::is_current(const Napi::CallbackInfo &info) {
  return Napi::Boolean::New(info.Env(),
                            node_is_current(session_, tree_, tags_));
}

Napi::Value
InjectionNodeWrapper::assert_current(const Napi::CallbackInfo &info) {
  if (!node_is_current(session_, tree_, tags_))
    return stale_node(info.Env());
  return info.This();
}

Napi::Value InjectionNodeWrapper::child(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  uint32_t index = 0;
  if (info.Length() == 0 || !read_uint32(info[0], &index))
    return throw_type_error(info.Env(), "child(index) requires a uint32 index",
                            "ERR_INVALID_INJECTION_NODE_INDEX");
  if (index >= ts_node_child_count(node))
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_, ts_node_child(node, index),
                     tags_, lease_);
}

Napi::Value InjectionNodeWrapper::named_child(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  uint32_t index = 0;
  if (info.Length() == 0 || !read_uint32(info[0], &index))
    return throw_type_error(
        info.Env(), "namedChild(index) requires a uint32 index",
        "ERR_INVALID_INJECTION_NODE_INDEX");
  if (index >= ts_node_named_child_count(node))
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_,
                     ts_node_named_child(node, index), tags_, lease_);
}

Napi::Value InjectionNodeWrapper::child_for_field_name(
    const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  if (info.Length() == 0 || !info[0].IsString())
    return throw_type_error(
        info.Env(), "childForFieldName(name) requires a string",
        "ERR_INVALID_INJECTION_NODE_FIELD");
  const std::string field = info[0].As<Napi::String>().Utf8Value();
  if (field.size() > std::numeric_limits<uint32_t>::max())
    return throw_type_error(info.Env(), "Syntax node field name is too long",
                            "ERR_INVALID_INJECTION_NODE_FIELD");
  const TSNode child = ts_node_child_by_field_name(
      node, field.data(), static_cast<uint32_t>(field.size()));
  if (ts_node_is_null(child))
    return info.Env().Null();
  return node_object(info.Env(), session_, tree_, child, tags_, lease_);
}

Napi::Value
InjectionNodeWrapper::descendants_of_type(const Napi::CallbackInfo &info) {
  READ_NODE_OR_THROW();
  if (info.Length() == 0)
    return throw_type_error(
        info.Env(), "descendantsOfType expects a string or string array",
        "ERR_INVALID_INJECTION_NODE_TYPE");
  std::unordered_set<std::string> accepted;
  if (info[0].IsString()) {
    accepted.insert(info[0].As<Napi::String>().Utf8Value());
  } else if (info[0].IsArray()) {
    Napi::Array values = info[0].As<Napi::Array>();
    for (uint32_t index = 0; index < values.Length(); index++) {
      Napi::Value value = values.Get(index);
      if (!value.IsString())
        return throw_type_error(
            info.Env(), "descendantsOfType array entries must be strings",
            "ERR_INVALID_INJECTION_NODE_TYPE");
      accepted.insert(value.As<Napi::String>().Utf8Value());
    }
  } else {
    return throw_type_error(
        info.Env(), "descendantsOfType expects a string or string array",
        "ERR_INVALID_INJECTION_NODE_TYPE");
  }

  const TSLanguage *language = ts_tree_language(tree_->tree());
  std::unordered_map<TSSymbol, std::vector<const std::string *>>
      accepted_by_symbol;
  accepted_by_symbol.reserve(accepted.size() * 2);
  for (const std::string &type : accepted) {
    for (bool named : {false, true}) {
      const TSSymbol symbol = ts_language_symbol_for_name(
          language, type.data(), static_cast<uint32_t>(type.size()), named);
      if (symbol != 0)
        accepted_by_symbol[symbol].push_back(&type);
    }
  }

  std::vector<TSNode> matches;
  std::vector<TSNode> pending;
  pending.reserve(64);
  const uint32_t root_child_count = ts_node_child_count(node);
  for (uint32_t index = root_child_count; index > 0; index--)
    pending.push_back(ts_node_child(node, index - 1));
  while (!pending.empty()) {
    const TSNode candidate = pending.back();
    pending.pop_back();
    const auto accepted_types =
        accepted_by_symbol.find(ts_node_symbol(candidate));
    if (accepted_types != accepted_by_symbol.end()) {
      const std::string_view actual_type = ts_node_type(candidate);
      for (const std::string *accepted_type : accepted_types->second) {
        if (actual_type == *accepted_type) {
          matches.push_back(candidate);
          break;
        }
      }
    }
    const uint32_t count = ts_node_child_count(candidate);
    for (uint32_t index = count; index > 0; index--)
      pending.push_back(ts_node_child(candidate, index - 1));
  }
  Napi::Array output = Napi::Array::New(info.Env(), matches.size());
  for (uint32_t index = 0; index < matches.size(); index++)
    output.Set(index,
               node_object(info.Env(), session_, tree_, matches[index], tags_,
                           lease_));
  return output;
}

Napi::Value InjectionNodeWrapper::release_snapshot_lease(
    const Napi::CallbackInfo &info) {
  if (lease_) {
    lease_->release();
    lease_.reset();
  }
  return info.Env().Undefined();
}

Napi::Value InjectionNodeWrapper::get_snapshot_lease_id(
    const Napi::CallbackInfo &info) {
  return Napi::Number::New(
      info.Env(), static_cast<double>(lease_ ? lease_->id() : 0));
}

#undef READ_NODE_OR_THROW

namespace {

bool current_tags_locked(const SessionState &state,
                         const InjectionRevisionTags &tags) {
  return !state.destroyed && state.buffer_revision == tags.buffer_revision &&
         state.syntax_revision == tags.syntax_revision &&
         state.language_generation == tags.language_generation;
}

Napi::Object stale_result(Napi::Env env, const InjectionRevisionTags &tags,
                          const char *reason, uint64_t request_id) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("accepted", Napi::Boolean::New(env, false));
  result.Set("reason", Napi::String::New(env, reason));
  set_tags(result, tags);
  if (request_id != 0)
    result.Set("requestId", Napi::Number::New(env, request_id));
  return result;
}

bool property_present(const QueryPatternMetadata &metadata,
                      std::string_view name) {
  return std::any_of(
      metadata.set_properties.begin(), metadata.set_properties.end(),
      [name](const QueryProperty &property) { return property.name == name; });
}

std::string property_value(const QueryPatternMetadata &metadata,
                           std::string_view name) {
  auto found = std::find_if(
      metadata.set_properties.begin(), metadata.set_properties.end(),
      [name](const QueryProperty &property) { return property.name == name; });
  return found != metadata.set_properties.end() && found->has_value
             ? found->value
             : std::string();
}

NodeRangeSpec node_range_spec(const PublishedSyntaxSnapshot &syntax,
                              const QueryCaptureRecord &capture,
                              bool include_children) {
  NodeRangeSpec result;
  result.start_index = capture.start_index;
  result.end_index = capture.end_index;
  result.start = Point{capture.start_row, capture.start_column};
  result.end = Point{capture.end_row, capture.end_column};
  if (include_children)
    return result;
  const uint32_t handle = syntax.handle_for_range(
      capture.start_index, capture.end_index, capture.node_symbol);
  const TSNode node = syntax.node(handle);
  if (ts_node_is_null(node))
    return result;
  const uint32_t child_count = ts_node_child_count(node);
  result.children.reserve(child_count);
  for (uint32_t index = 0; index < child_count; index++) {
    const TSNode child = ts_node_child(node, index);
    const Point start = point_from_ts(ts_node_start_point(child));
    const Point end = point_from_ts(ts_node_end_point(child));
    result.children.push_back(NodeRangeSpec{
        start_index_for(child), end_index_for(child), start, end, {}});
  }
  return result;
}

std::vector<QueryInjectionProposal> query_injection_proposals(
    const std::shared_ptr<const PublishedSyntaxSnapshot> &syntax,
    const std::shared_ptr<const SyntaxQuerySnapshot> &queries,
    const SnapshotReader &reader, const std::string &grammar_id) {
  std::vector<QueryInjectionProposal> output;
  if (!syntax || !queries)
    return output;
  auto index_found = queries->indices.find("injectionsQuery");
  if (index_found == queries->indices.end())
    return output;
  const QueryIndexSnapshot &index = index_found->second;

  struct Match {
    uint32_t pattern_index = 0;
    std::vector<const QueryCaptureRecord *> content;
    const QueryCaptureRecord *language = nullptr;
  };
  std::map<std::pair<uint32_t, uint32_t>, Match> matches;
  for (const QueryCaptureRecord &capture : index.captures) {
    if (capture.name_id >= index.capture_names.size() ||
        capture.pattern_index >= index.patterns.size())
      continue;
    const std::string &name = index.capture_names[capture.name_id];
    Match &match = matches[{capture.pattern_index, capture.match_id}];
    match.pattern_index = capture.pattern_index;
    if (name == "injection.content")
      match.content.push_back(&capture);
    else if (name == "injection.language")
      match.language = &capture;
  }

  struct CombinedKey {
    uint32_t pattern_index = 0;
    std::string language;
    bool target_self = false;
    bool target_parent = false;

    bool operator<(const CombinedKey &other) const {
      return std::tie(pattern_index, language, target_self, target_parent) <
             std::tie(other.pattern_index, other.language, other.target_self,
                      other.target_parent);
    }
  };
  std::map<CombinedKey, size_t> combined_indices;

  for (const auto &[key, match] : matches) {
    (void)key;
    if (match.content.empty() || match.pattern_index >= index.patterns.size())
      continue;
    const QueryPatternMetadata &metadata = index.patterns[match.pattern_index];
    const bool target_self = property_present(metadata, "injection.self");
    const bool target_parent = property_present(metadata, "injection.parent");
    std::string language =
        trim_ascii(property_value(metadata, "injection.language"));
    if (language.empty() && match.language != nullptr) {
      std::u16string utf16;
      if (reader.append_range(match.language->start_index,
                              match.language->end_index, utf16))
        language = trim_ascii(utf16_to_utf8(utf16));
    }
    if (language.empty() && !target_self && !target_parent)
      continue;
    const bool include_children =
        property_present(metadata, "injection.include-children");
    const bool combined = property_present(metadata, "injection.combined");
    const bool include_language_scope =
        trim_ascii(property_value(metadata, "injection.language-scope")) !=
        "none";

    QueryInjectionProposal proposal;
    proposal.pattern_index = match.pattern_index;
    proposal.parent_grammar_id = grammar_id;
    proposal.language_name = language;
    proposal.target_self = target_self;
    proposal.target_parent = target_parent;
    proposal.include_children = include_children;
    proposal.combined = combined;
    proposal.include_language_scope = include_language_scope;
    for (const QueryCaptureRecord *capture : match.content)
      proposal.content.push_back(
          node_range_spec(*syntax, *capture, include_children));

    if (!combined) {
      output.push_back(std::move(proposal));
      continue;
    }
    CombinedKey combined_key{match.pattern_index, language, target_self,
                             target_parent};
    auto found = combined_indices.find(combined_key);
    if (found == combined_indices.end()) {
      combined_indices.emplace(combined_key, output.size());
      output.push_back(std::move(proposal));
    } else {
      auto &destination = output[found->second].content;
      destination.insert(destination.end(), proposal.content.begin(),
                         proposal.content.end());
    }
  }
  return output;
}

Napi::Object node_spec_object(Napi::Env env, const NodeRangeSpec &spec) {
  Napi::Object object = Napi::Object::New(env);
  object.Set("startIndex", Napi::Number::New(env, spec.start_index));
  object.Set("endIndex", Napi::Number::New(env, spec.end_index));
  object.Set("startPosition",
             point_object(env, spec.start.row, spec.start.column));
  object.Set("endPosition", point_object(env, spec.end.row, spec.end.column));
  if (!spec.children.empty()) {
    Napi::Array children = Napi::Array::New(env, spec.children.size());
    for (uint32_t index = 0; index < spec.children.size(); index++)
      children.Set(index, node_spec_object(env, spec.children[index]));
    object.Set("children", children);
  }
  return object;
}

bool parse_manifest(Napi::Env env, Napi::Value value,
                    std::vector<GrammarManifestEntry> *manifest) {
  if (!value.IsArray()) {
    throw_type_error(env, "Injection manifest grammars must be an array",
                     "ERR_INVALID_INJECTION_MANIFEST");
    return false;
  }
  Napi::Array grammars = value.As<Napi::Array>();
  for (uint32_t index = 0; index < grammars.Length(); index++) {
    Napi::Value grammar_value = grammars.Get(index);
    if (!grammar_value.IsObject()) {
      throw_type_error(env, "Injection manifest entries must be objects",
                       "ERR_INVALID_INJECTION_MANIFEST");
      return false;
    }
    Napi::Object grammar = grammar_value.As<Napi::Object>();
    if (!grammar.Get("grammarId").IsString() ||
        !grammar.Get("types").IsArray()) {
      throw_type_error(env, "Injection manifest entry fields are invalid",
                       "ERR_INVALID_INJECTION_MANIFEST");
      return false;
    }
    GrammarManifestEntry entry;
    entry.grammar_id = grammar.Get("grammarId").As<Napi::String>().Utf8Value();
    Napi::Array types = grammar.Get("types").As<Napi::Array>();
    for (uint32_t type_index = 0; type_index < types.Length(); type_index++) {
      Napi::Value type = types.Get(type_index);
      if (!type.IsString()) {
        throw_type_error(env, "Injection manifest types must be strings",
                         "ERR_INVALID_INJECTION_MANIFEST");
        return false;
      }
      entry.types.push_back(type.As<Napi::String>().Utf8Value());
    }
    std::vector<std::pair<uint32_t, std::string>> registrations;
    Napi::Value registrations_value = grammar.Get("registrations");
    if (registrations_value.IsArray()) {
      Napi::Array values = registrations_value.As<Napi::Array>();
      registrations.reserve(values.Length());
      for (uint32_t registration_index = 0;
           registration_index < values.Length(); registration_index++) {
        Napi::Value registration_value = values.Get(registration_index);
        if (!registration_value.IsObject()) {
          throw_type_error(env,
                           "Injection manifest registrations must be objects",
                           "ERR_INVALID_INJECTION_MANIFEST");
          return false;
        }
        Napi::Object registration = registration_value.As<Napi::Object>();
        uint32_t id = 0;
        if (!read_uint32(registration.Get("id"), &id) ||
            !registration.Get("type").IsString()) {
          throw_type_error(env,
                           "Injection manifest registration fields are invalid",
                           "ERR_INVALID_INJECTION_MANIFEST");
          return false;
        }
        registrations.emplace_back(
            id, registration.Get("type").As<Napi::String>().Utf8Value());
      }
    }
    std::sort(registrations.begin(), registrations.end());
    constexpr uint64_t offset_basis = UINT64_C(14695981039346656037);
    constexpr uint64_t prime = UINT64_C(1099511628211);
    uint64_t fingerprint = offset_basis;
    const auto hash_bytes = [&](std::string_view bytes) {
      for (unsigned char byte : bytes) {
        fingerprint ^= byte;
        fingerprint *= prime;
      }
    };
    hash_bytes(entry.grammar_id);
    if (registrations.empty()) {
      std::sort(entry.types.begin(), entry.types.end());
      for (const std::string &type : entry.types)
        hash_bytes(type);
    } else {
      for (const auto &[id, type] : registrations) {
        hash_bytes(std::string_view(
            reinterpret_cast<const char *>(&id), sizeof(id)));
        hash_bytes(type);
      }
    }
    entry.registration_fingerprint = fingerprint;
    manifest->push_back(std::move(entry));
  }
  return true;
}

bool parse_query_paths(Napi::Env env, Napi::Value value,
                       std::map<std::string, std::vector<std::string>> *paths) {
  if (value.IsUndefined() || value.IsNull())
    return true;
  if (!value.IsObject()) {
    throw_type_error(env, "Injected grammar queryPaths must be an object",
                     "ERR_INVALID_INJECTION_RESULT");
    return false;
  }
  Napi::Object object = value.As<Napi::Object>();
  Napi::Array keys = object.GetPropertyNames();
  for (uint32_t index = 0; index < keys.Length(); index++) {
    Napi::Value key = keys.Get(index);
    Napi::Value values = object.Get(key);
    if (!key.IsString() || !values.IsArray()) {
      throw_type_error(env, "Injected grammar queryPaths entries are invalid",
                       "ERR_INVALID_INJECTION_RESULT");
      return false;
    }
    std::vector<std::string> parsed;
    Napi::Array array = values.As<Napi::Array>();
    for (uint32_t value_index = 0; value_index < array.Length();
         value_index++) {
      if (!array.Get(value_index).IsString()) {
        throw_type_error(env, "Injected grammar query paths must be strings",
                         "ERR_INVALID_INJECTION_RESULT");
        return false;
      }
      parsed.push_back(array.Get(value_index).As<Napi::String>().Utf8Value());
    }
    (*paths)[key.As<Napi::String>().Utf8Value()] = std::move(parsed);
  }
  return true;
}

bool parse_grammar_descriptor(Napi::Env env, Napi::Value value,
                              ParsedGrammarDescriptor *descriptor) {
  if (!value.IsObject()) {
    throw_type_error(env, "Injection result grammar must be an object",
                     "ERR_INVALID_INJECTION_RESULT");
    return false;
  }
  Napi::Object object = value.As<Napi::Object>();
  if (!object.Get("languageId").IsString()) {
    throw_type_error(env, "Injected grammar requires languageId",
                     "ERR_INVALID_INJECTION_RESULT");
    return false;
  }
  descriptor->language_id =
      object.Get("languageId").As<Napi::String>().Utf8Value();
  if (object.Get("runtime").IsString())
    descriptor->runtime = object.Get("runtime").As<Napi::String>().Utf8Value();
  if (object.Get("wasmPath").IsString())
    descriptor->wasm_path =
        object.Get("wasmPath").As<Napi::String>().Utf8Value();
  if (object.Get("languageName").IsString())
    descriptor->language_name =
        object.Get("languageName").As<Napi::String>().Utf8Value();
  if (object.Get("languageSegment").IsString())
    descriptor->language_segment =
        object.Get("languageSegment").As<Napi::String>().Utf8Value();
  return parse_query_paths(env, object.Get("queryPaths"),
                           &descriptor->query_paths);
}

Point point_for_index(const SnapshotAnalysis &analysis, uint64_t index) {
  if (analysis.line_starts.empty())
    return Point{};
  index = std::min(index, analysis.utf16_length);
  auto found = std::upper_bound(analysis.line_starts.begin(),
                                analysis.line_starts.end(), index);
  size_t row = found == analysis.line_starts.begin()
                   ? 0
                   : static_cast<size_t>(found - analysis.line_starts.begin() -
                                         1);
  return Point{row, index - analysis.line_starts[row]};
}

bool read_range_index(Napi::Value value, const SnapshotAnalysis &analysis,
                      bool allow_infinity, uint64_t *result) {
  if (allow_infinity && value.IsNumber() &&
      std::isinf(value.As<Napi::Number>().DoubleValue())) {
    *result = analysis.utf16_length;
    return true;
  }
  return read_uint64(value, result) && *result <= analysis.utf16_length;
}

bool parse_content_spec(Napi::Env env, Napi::Value value,
                        const SnapshotAnalysis &analysis,
                        NodeRangeSpec *spec) {
  if (!value.IsObject()) {
    throw_type_error(env, "Injection content entries must be objects",
                     "ERR_INVALID_INJECTION_RESULT");
    return false;
  }
  Napi::Object object = value.As<Napi::Object>();
  if (!read_range_index(object.Get("startIndex"), analysis, false,
                        &spec->start_index) ||
      !read_range_index(object.Get("endIndex"), analysis, true,
                        &spec->end_index) ||
      spec->start_index > spec->end_index) {
    throw_type_error(env, "Injection content indices are invalid",
                     "ERR_INVALID_INJECTION_RESULT");
    return false;
  }
  spec->start = point_for_index(analysis, spec->start_index);
  spec->end = point_for_index(analysis, spec->end_index);
  Napi::Value children_value = object.Get("children");
  if (!children_value.IsUndefined()) {
    if (!children_value.IsArray()) {
      throw_type_error(env, "Injection content children must be an array",
                       "ERR_INVALID_INJECTION_RESULT");
      return false;
    }
    Napi::Array children = children_value.As<Napi::Array>();
    spec->children.reserve(children.Length());
    for (uint32_t index = 0; index < children.Length(); index++) {
      NodeRangeSpec child;
      if (!parse_content_spec(env, children.Get(index), analysis, &child))
        return false;
      spec->children.push_back(std::move(child));
    }
  }
  return true;
}

std::vector<InjectionRangeRecord>
included_ranges(const std::vector<NodeRangeSpec> &content,
                bool include_children, const SnapshotAnalysis &analysis) {
  std::vector<InjectionRangeRecord> ranges;
  for (const NodeRangeSpec &spec : content) {
    if (include_children || spec.children.empty()) {
      if (spec.start_index < spec.end_index)
        ranges.push_back(InjectionRangeRecord{
            spec.start_index, spec.end_index, spec.start, spec.end, {}});
      continue;
    }
    std::vector<NodeRangeSpec> children = spec.children;
    std::sort(children.begin(), children.end(),
              [](const NodeRangeSpec &left, const NodeRangeSpec &right) {
                return std::tie(left.start_index, left.end_index) <
                       std::tie(right.start_index, right.end_index);
              });
    uint64_t cursor = spec.start_index;
    for (const NodeRangeSpec &child : children) {
      const uint64_t child_start =
          std::clamp(child.start_index, spec.start_index, spec.end_index);
      const uint64_t child_end =
          std::clamp(child.end_index, spec.start_index, spec.end_index);
      if (cursor < child_start) {
        ranges.push_back(InjectionRangeRecord{
            cursor, child_start, point_for_index(analysis, cursor),
            point_for_index(analysis, child_start), {}});
      }
      cursor = std::max(cursor, child_end);
    }
    if (cursor < spec.end_index) {
      ranges.push_back(InjectionRangeRecord{
          cursor, spec.end_index, point_for_index(analysis, cursor),
          point_for_index(analysis, spec.end_index), {}});
    }
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const InjectionRangeRecord &left,
               const InjectionRangeRecord &right) {
              return std::tie(left.start_index, left.end_index) <
                     std::tie(right.start_index, right.end_index);
            });
  return ranges;
}

bool whitespace_between(const SnapshotReader &reader, uint64_t start,
                        uint64_t end) {
  for (uint64_t index = start; index < end; index++) {
    const char16_t character = reader.character_at(index);
    if (character != u' ' && character != u'\t' && character != u'\r' &&
        character != u'\n' && character != u'\f' && character != u'\v')
      return false;
  }
  return true;
}

void merge_adjacent_whitespace(std::vector<InjectionRangeRecord> &ranges,
                               const SnapshotReader &reader,
                               const SnapshotAnalysis &analysis) {
  if (ranges.size() < 2)
    return;
  std::vector<InjectionRangeRecord> merged;
  merged.push_back(std::move(ranges.front()));
  for (size_t index = 1; index < ranges.size(); index++) {
    InjectionRangeRecord &previous = merged.back();
    InjectionRangeRecord &current = ranges[index];
    if (previous.end_index <= current.start_index &&
        whitespace_between(reader, previous.end_index, current.start_index)) {
      previous.end_index = current.end_index;
      previous.end = point_for_index(analysis, current.end_index);
    } else {
      merged.push_back(std::move(current));
    }
  }
  ranges = std::move(merged);
}

std::vector<std::string> parse_scopes(Napi::Value value) {
  std::vector<std::string> scopes;
  if (value.IsString()) {
    scopes.push_back(value.As<Napi::String>().Utf8Value());
  } else if (value.IsArray()) {
    Napi::Array values = value.As<Napi::Array>();
    for (uint32_t index = 0; index < values.Length(); index++) {
      if (values.Get(index).IsString())
        scopes.push_back(values.Get(index).As<Napi::String>().Utf8Value());
    }
  }
  return scopes;
}

} // namespace

} // namespace document_engine
