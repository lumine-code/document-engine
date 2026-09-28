#include "syntax/syntax-backend.h"

#include "syntax/query-engine.h"
#include "text-bridge/snapshot-reader.h"

#include <tree_sitter/api.h>
#include <wasm.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace document_engine {

namespace {

using Clock = std::chrono::steady_clock;

struct Runtime {
  TSParser *parser = nullptr;
  TSTree *tree = nullptr;
  std::shared_ptr<const NativeQueryEngine> query_engine;
  uint64_t tree_revision = 0;
  std::shared_ptr<const SnapshotAnalysis> tree_analysis;
  SyntaxConfiguration configuration;
  std::vector<SyntaxConfiguration::IncludedRange> included_ranges;

  ~Runtime() {
    ts_tree_delete(tree);
    ts_parser_delete(parser);
  }
};

struct InputPayload {
  const SnapshotReader *reader = nullptr;
  const SyntaxCancellation *cancellation = nullptr;
  bool read_failed = false;
};

const char *read_snapshot(void *payload_value, uint32_t byte_index,
                          TSPoint, uint32_t *bytes_read) {
  auto *payload = static_cast<InputPayload *>(payload_value);
  if (payload == nullptr || bytes_read == nullptr)
    return nullptr;
  if (payload->cancellation != nullptr &&
      payload->cancellation->requested()) {
    *bytes_read = 0;
    return nullptr;
  }
  const char *data = nullptr;
  if (!payload->reader->read_utf16le(byte_index, &data, bytes_read)) {
    payload->read_failed = true;
    *bytes_read = 0;
    return nullptr;
  }
  return data;
}

bool cancel_parse(TSParseState *state) {
  if (state == nullptr || state->payload == nullptr)
    return false;
  return static_cast<const SyntaxCancellation *>(state->payload)->requested();
}

std::string take_wasm_error(TSWasmError &error) {
  std::string message = error.message == nullptr ? "Unknown Wasm runtime error"
                                                  : error.message;
  std::free(error.message);
  error.message = nullptr;
  return message;
}

std::filesystem::path path_from_utf8(const std::string &value) {
  std::u8string encoded;
  encoded.reserve(value.size());
  for (const unsigned char character : value)
    encoded.push_back(static_cast<char8_t>(character));
  return std::filesystem::path(encoded);
}

std::string path_key(const std::filesystem::path &value) {
  const std::u8string encoded = value.generic_u8string();
  return std::string(reinterpret_cast<const char *>(encoded.data()),
                     encoded.size());
}

bool read_wasm_file(const std::filesystem::path &file_path,
                    std::vector<char> &bytes, std::string &error) {
  std::ifstream stream(file_path, std::ios::binary | std::ios::ate);
  if (!stream) {
    error = "Unable to open grammar Wasm: " + path_key(file_path);
    return false;
  }
  const std::streamoff length = stream.tellg();
  if (length < 0 ||
      static_cast<uint64_t>(length) >
          static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    error = "Grammar Wasm exceeds the Tree-sitter uint32 size limit";
    return false;
  }
  bytes.resize(static_cast<size_t>(length));
  stream.seekg(0, std::ios::beg);
  if (length > 0 && !stream.read(bytes.data(), length)) {
    error = "Unable to read grammar Wasm: " + path_key(file_path);
    return false;
  }
  return true;
}

bool read_wasm_uleb(const std::vector<char> &bytes, size_t &offset,
                    uint32_t &value) {
  value = 0;
  unsigned shift = 0;
  while (offset < bytes.size() && shift < 35) {
    const uint8_t byte = static_cast<uint8_t>(bytes[offset++]);
    value |= static_cast<uint32_t>(byte & 0x7fu) << shift;
    if ((byte & 0x80u) == 0)
      return true;
    shift += 7;
  }
  return false;
}

std::vector<std::string>
language_exports_in_wasm(const std::vector<char> &bytes) {
  const unsigned char magic[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00,
                                 0x00};
  if (bytes.size() < sizeof(magic) ||
      !std::equal(std::begin(magic), std::end(magic),
                  reinterpret_cast<const unsigned char *>(bytes.data())))
    return {};
  size_t offset = sizeof(magic);
  while (offset < bytes.size()) {
    const uint8_t section_id = static_cast<uint8_t>(bytes[offset++]);
    uint32_t section_size = 0;
    if (!read_wasm_uleb(bytes, offset, section_size) ||
        section_size > bytes.size() - offset)
      return {};
    const size_t section_end = offset + section_size;
    if (section_id != 7) {
      offset = section_end;
      continue;
    }
    uint32_t export_count = 0;
    if (!read_wasm_uleb(bytes, offset, export_count))
      return {};
    std::vector<std::string> candidates;
    constexpr std::string_view prefix = "tree_sitter_";
    for (uint32_t index = 0; index < export_count; index++) {
      uint32_t name_size = 0;
      if (!read_wasm_uleb(bytes, offset, name_size) ||
          name_size > section_end - offset)
        return {};
      const std::string name(bytes.data() + offset, name_size);
      offset += name_size;
      if (offset >= section_end)
        return {};
      const uint8_t export_kind = static_cast<uint8_t>(bytes[offset++]);
      uint32_t export_index = 0;
      if (!read_wasm_uleb(bytes, offset, export_index))
        return {};
      (void)export_index;
      if (export_kind == 0 && name.starts_with(prefix) &&
          name.find("_external_scanner_") == std::string::npos) {
        candidates.push_back(name.substr(prefix.size()));
      }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    return candidates;
  }
  return {};
}

std::string grammar_fingerprint(const std::filesystem::path &path,
                                uintmax_t size,
                                std::filesystem::file_time_type modified_at) {
  std::ostringstream output;
  output << path_key(path) << '\n' << size << '\n'
         << modified_at.time_since_epoch().count();
  return output.str();
}

class GrammarCache {
public:
  const TSLanguage *acquire(const SyntaxConfiguration &configuration,
                            SyntaxParseResult &result) {
    std::error_code filesystem_error;
    std::filesystem::path canonical_path = std::filesystem::weakly_canonical(
        path_from_utf8(configuration.wasm_path), filesystem_error);
    if (filesystem_error)
      canonical_path = std::filesystem::absolute(
          path_from_utf8(configuration.wasm_path), filesystem_error);
    if (filesystem_error) {
      result.error_code = "ERR_SYNTAX_WASM_READ_FAILED";
      result.error_message = "Unable to resolve grammar Wasm path: " +
                             configuration.wasm_path;
      return nullptr;
    }
    const uintmax_t file_size =
        std::filesystem::file_size(canonical_path, filesystem_error);
    if (filesystem_error) {
      result.error_code = "ERR_SYNTAX_WASM_READ_FAILED";
      result.error_message = "Unable to stat grammar Wasm: " +
                             configuration.wasm_path;
      return nullptr;
    }
    const std::filesystem::file_time_type modified_at =
        std::filesystem::last_write_time(canonical_path, filesystem_error);
    if (filesystem_error) {
      result.error_code = "ERR_SYNTAX_WASM_READ_FAILED";
      result.error_message = "Unable to read grammar Wasm timestamp: " +
                             configuration.wasm_path;
      return nullptr;
    }

    const std::string key =
        path_key(canonical_path) + '\n' + configuration.language_name;
    result.grammar_fingerprint =
        grammar_fingerprint(canonical_path, file_size, modified_at);
    std::lock_guard<std::mutex> lock(mutex_);
    auto existing = entries_.find(key);
    if (existing != entries_.end() && existing->second.size == file_size &&
        existing->second.modified_at == modified_at) {
      hits_++;
      existing->second.last_used = ++access_clock_;
      result.grammar_cache_hit = true;
      result.wasm_bytes = existing->second.wasm_bytes;
      result.resolved_language_name =
          existing->second.resolved_language_name;
      return ts_language_copy(existing->second.language);
    }

    misses_++;
    if (engine_ == nullptr) {
      engine_ = wasm_engine_new();
      if (engine_ == nullptr) {
        result.error_code = "ERR_SYNTAX_RUNTIME_INIT_FAILED";
        result.error_message = "Wasmtime could not create the shared engine";
        return nullptr;
      }
    }

    std::vector<char> wasm;
    if (!read_wasm_file(canonical_path, wasm, result.error_message)) {
      result.error_code = "ERR_SYNTAX_WASM_READ_FAILED";
      return nullptr;
    }
    result.wasm_bytes = wasm.size();
    const auto started_at = Clock::now();
    TSWasmError store_error{};
    TSWasmStore *loader_store = ts_wasm_store_new(engine_, &store_error);
    if (loader_store == nullptr) {
      result.error_code = "ERR_SYNTAX_RUNTIME_INIT_FAILED";
      result.error_message = take_wasm_error(store_error);
      return nullptr;
    }
    TSWasmError language_error{};
    const TSLanguage *language = ts_wasm_store_load_language(
        loader_store, configuration.language_name.c_str(), wasm.data(),
        static_cast<uint32_t>(wasm.size()), &language_error);
    std::string primary_error;
    if (language == nullptr) {
      primary_error = take_wasm_error(language_error);
      const std::vector<std::string> candidates =
          language_exports_in_wasm(wasm);
      if (candidates.size() == 1 &&
          candidates.front() != configuration.language_name) {
        TSWasmError fallback_error{};
        language = ts_wasm_store_load_language(
            loader_store, candidates.front().c_str(), wasm.data(),
            static_cast<uint32_t>(wasm.size()), &fallback_error);
        if (language != nullptr) {
          result.resolved_language_name = candidates.front();
        } else {
          take_wasm_error(fallback_error);
        }
      }
    }
    ts_wasm_store_delete(loader_store);
    if (language == nullptr) {
      result.error_code = "ERR_SYNTAX_WASM_LOAD_FAILED";
      result.error_message = primary_error;
      return nullptr;
    }
    if (result.resolved_language_name.empty())
      result.resolved_language_name = configuration.language_name;
    result.grammar_load_milliseconds =
        std::chrono::duration<double, std::milli>(Clock::now() - started_at)
            .count();
    compilations_++;

    if (existing != entries_.end()) {
      ts_language_delete(existing->second.language);
      existing->second = Entry{file_size,
                               modified_at,
                               language,
                               wasm.size(),
                               ++access_clock_,
                               result.resolved_language_name};
    } else {
      if (entries_.size() >= SYNTAX_GRAMMAR_CACHE_CAPACITY) {
        auto least_recent = std::min_element(
            entries_.begin(), entries_.end(),
            [](const auto &left, const auto &right) {
              return left.second.last_used < right.second.last_used;
            });
        if (least_recent != entries_.end()) {
          ts_language_delete(least_recent->second.language);
          entries_.erase(least_recent);
          evictions_++;
        }
      }
      entries_.emplace(
          key, Entry{file_size,
                     modified_at,
                     language,
                     wasm.size(),
                     ++access_clock_,
                     result.resolved_language_name});
    }
    return ts_language_copy(language);
  }

  wasm_engine_t *engine() const { return engine_; }

  SyntaxGrammarCacheDiagnostics diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SyntaxGrammarCacheDiagnostics{
        static_cast<uint64_t>(entries_.size()), compilations_, hits_, misses_,
        SYNTAX_GRAMMAR_CACHE_CAPACITY, evictions_};
  }

private:
  struct Entry {
    uintmax_t size;
    std::filesystem::file_time_type modified_at;
    const TSLanguage *language;
    uint64_t wasm_bytes;
    uint64_t last_used;
    std::string resolved_language_name;
  };

  mutable std::mutex mutex_;
  wasm_engine_t *engine_ = nullptr;
  std::unordered_map<std::string, Entry> entries_;
  uint64_t compilations_ = 0;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
  uint64_t access_clock_ = 0;
  uint64_t evictions_ = 0;
};

GrammarCache &grammar_cache() {
  static GrammarCache *cache = new GrammarCache();
  return *cache;
}

bool same_runtime(const Runtime &runtime,
                  const SyntaxConfiguration &configuration) {
  return runtime.configuration.language_id == configuration.language_id &&
         runtime.configuration.runtime == configuration.runtime &&
         runtime.configuration.wasm_path == configuration.wasm_path &&
         runtime.configuration.language_name == configuration.language_name &&
         runtime.configuration.generation == configuration.generation;
}

std::unique_ptr<Runtime>
create_runtime(const SyntaxConfiguration &configuration,
               SyntaxParseResult &result) {
  auto runtime = std::make_unique<Runtime>();
  runtime->configuration = configuration;
  const TSLanguage *language = grammar_cache().acquire(configuration, result);
  if (language == nullptr)
    return nullptr;

  TSWasmError store_error{};
  TSWasmStore *store =
      ts_wasm_store_new(grammar_cache().engine(), &store_error);
  if (store == nullptr) {
    ts_language_delete(language);
    result.error_code = "ERR_SYNTAX_RUNTIME_INIT_FAILED";
    result.error_message = take_wasm_error(store_error);
    return nullptr;
  }

  runtime->parser = ts_parser_new();
  if (runtime->parser == nullptr) {
    ts_language_delete(language);
    ts_wasm_store_delete(store);
    result.error_code = "ERR_SYNTAX_RUNTIME_INIT_FAILED";
    result.error_message = "Tree-sitter could not create a parser";
    return nullptr;
  }
  ts_parser_set_wasm_store(runtime->parser, store);
  if (!ts_parser_set_language(runtime->parser, language)) {
    ts_language_delete(language);
    result.error_code = "ERR_SYNTAX_LANGUAGE_FAILED";
    result.error_message =
        "Tree-sitter rejected the language exported by the Wasm module";
    return nullptr;
  }
  QueryConfiguration query_configuration = configuration.queries;
  query_configuration.grammar_identity =
      result.grammar_fingerprint + '\n' + result.resolved_language_name;
  std::unique_ptr<NativeQueryEngine> compiled_query_engine =
      NativeQueryEngine::compile(language, query_configuration,
                                 result.query_error,
                                 result.query_statistics);
  if (!compiled_query_engine) {
    ts_language_delete(language);
    result.error_code = result.query_error.code;
    result.error_message = result.query_error.message;
    return nullptr;
  }
  runtime->query_engine =
      std::shared_ptr<const NativeQueryEngine>(std::move(compiled_query_engine));
  ts_language_delete(language);
  return runtime;
}

bool point_to_byte(const SnapshotAnalysis &analysis, const Point &point,
                   uint32_t &byte_offset, TSPoint &ts_point) {
  if (point.row >= analysis.line_starts.size())
    return false;
  const uint64_t row_start = analysis.line_starts[point.row];
  const uint64_t row_end = analysis.line_ends[point.row];
  if (point.column > row_end - row_start)
    return false;
  const uint64_t absolute = row_start + point.column;
  if (absolute > MAX_SYNTAX_UTF16_LENGTH ||
      point.column > MAX_SYNTAX_UTF16_LENGTH ||
      point.row > std::numeric_limits<uint32_t>::max())
    return false;
  byte_offset = static_cast<uint32_t>(absolute * UINT64_C(2));
  ts_point = TSPoint{static_cast<uint32_t>(point.row),
                     static_cast<uint32_t>(point.column * UINT64_C(2))};
  return true;
}

bool build_input_edit(const SyntaxEdit &edit,
                      const SnapshotAnalysis &old_analysis,
                      const SnapshotAnalysis &new_analysis,
                      TSInputEdit &input_edit) {
  if (edit.old_start != edit.new_start)
    return false;
  return point_to_byte(old_analysis, edit.old_start, input_edit.start_byte,
                       input_edit.start_point) &&
         point_to_byte(old_analysis, edit.old_end, input_edit.old_end_byte,
                       input_edit.old_end_point) &&
         point_to_byte(new_analysis, edit.new_end, input_edit.new_end_byte,
                       input_edit.new_end_point) &&
         input_edit.start_byte <= input_edit.old_end_byte &&
         input_edit.start_point.row <= input_edit.old_end_point.row;
}

bool same_included_ranges(
    const std::vector<SyntaxConfiguration::IncludedRange> &left,
    const std::vector<SyntaxConfiguration::IncludedRange> &right) {
  if (left.size() != right.size())
    return false;
  for (size_t index = 0; index < left.size(); index++) {
    if (left[index].start != right[index].start ||
        left[index].end != right[index].end ||
        left[index].start_index != right[index].start_index ||
        left[index].end_index != right[index].end_index)
      return false;
  }
  return true;
}

bool build_included_ranges(
    const std::vector<SyntaxConfiguration::IncludedRange> &source,
    std::vector<TSRange> &output) {
  output.clear();
  output.reserve(source.size());
  uint64_t previous_end = 0;
  for (const SyntaxConfiguration::IncludedRange &range : source) {
    if (range.start_index > range.end_index ||
        (!output.empty() && range.start_index < previous_end) ||
        range.end_index > MAX_SYNTAX_UTF16_LENGTH ||
        range.start.row > std::numeric_limits<uint32_t>::max() ||
        range.end.row > std::numeric_limits<uint32_t>::max() ||
        range.start.column > MAX_SYNTAX_UTF16_LENGTH ||
        range.end.column > MAX_SYNTAX_UTF16_LENGTH)
      return false;
    output.push_back(TSRange{
        TSPoint{static_cast<uint32_t>(range.start.row),
                static_cast<uint32_t>(range.start.column * UINT64_C(2))},
        TSPoint{static_cast<uint32_t>(range.end.row),
                static_cast<uint32_t>(range.end.column * UINT64_C(2))},
        static_cast<uint32_t>(range.start_index * UINT64_C(2)),
        static_cast<uint32_t>(range.end_index * UINT64_C(2))});
    previous_end = range.end_index;
  }
  return true;
}

void hash_bytes(uint64_t &checksum, const void *data, size_t length) {
  constexpr uint64_t fnv_prime = UINT64_C(1099511628211);
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (size_t index = 0; index < length; index++) {
    checksum ^= bytes[index];
    checksum *= fnv_prime;
  }
}

void hash_uint32(uint64_t &checksum, uint32_t value) {
  hash_bytes(checksum, &value, sizeof(value));
}

bool analyze_tree(TSTree *tree, const SyntaxCancellation &cancellation,
                  SyntaxParseResult &result) {
  const TSNode root = ts_tree_root_node(tree);
  result.root_type = ts_node_type(root);
  result.root_has_error = ts_node_has_error(root);
  constexpr uint64_t fnv_offset_basis = UINT64_C(14695981039346656037);
  uint64_t checksum = fnv_offset_basis;
  hash_bytes(checksum, result.root_type.data(), result.root_type.size());
  hash_uint32(checksum, ts_node_start_byte(root));
  hash_uint32(checksum, ts_node_end_byte(root));
  hash_uint32(checksum, static_cast<uint32_t>(ts_node_symbol(root)));
  hash_uint32(checksum, ts_node_child_count(root));
  hash_uint32(checksum, ts_node_named_child_count(root));
  result.node_count = ts_node_descendant_count(root);
  hash_uint32(checksum, static_cast<uint32_t>(result.node_count));
  const unsigned char flags[] = {
      static_cast<unsigned char>(ts_node_is_named(root)),
      static_cast<unsigned char>(ts_node_is_missing(root)),
      static_cast<unsigned char>(ts_node_is_error(root)),
      static_cast<unsigned char>(result.root_has_error)};
  hash_bytes(checksum, flags, sizeof(flags));
  if (cancellation.requested())
    return false;
  result.checksum = checksum;
  return true;
}

bool cancel_query(void *payload) {
  return payload != nullptr &&
         static_cast<const SyntaxCancellation *>(payload)->requested();
}

} // namespace

struct SyntaxBackend::Impl {
  std::unique_ptr<Runtime> runtime;
};

bool SyntaxCancellation::requested() const {
  return (destroyed != nullptr && destroyed->load(std::memory_order_relaxed)) ||
         (latest_revision != nullptr &&
          latest_revision->load(std::memory_order_relaxed) != revision) ||
         (language_generation != nullptr &&
          language_generation->load(std::memory_order_relaxed) !=
              expected_language_generation);
}

SyntaxBackend::SyntaxBackend() : impl_(std::make_unique<Impl>()) {}

SyntaxBackend::~SyntaxBackend() = default;

bool SyntaxBackend::parse(
    const SnapshotReader &reader,
    std::shared_ptr<const SnapshotAnalysis> analysis,
    const SyntaxConfiguration &configuration,
    const std::vector<SyntaxEdit> &edits, uint64_t revision,
    uint64_t maximum_utf16_length, const SyntaxCancellation &cancellation,
    SyntaxParseResult &result) {
  result = SyntaxParseResult{};
  if (!configuration.enabled())
    return true;
  result.attempted = true;

  const uint64_t effective_limit =
      std::min<uint64_t>(maximum_utf16_length, MAX_SYNTAX_UTF16_LENGTH);
  if (reader.size() > effective_limit) {
    result.error_code = "ERR_SYNTAX_INPUT_TOO_LARGE";
    result.error_message =
        "Document exceeds the configured Tree-sitter UTF-16 input limit";
    return false;
  }
  if (cancellation.requested()) {
    result.cancelled = true;
    return true;
  }

  if (!impl_->runtime || !same_runtime(*impl_->runtime, configuration)) {
    std::unique_ptr<Runtime> replacement =
        create_runtime(configuration, result);
    if (!replacement)
      return false;
    impl_->runtime = std::move(replacement);
  }
  Runtime &runtime = *impl_->runtime;

  std::vector<TSRange> included_ranges;
  if (!build_included_ranges(configuration.included_ranges, included_ranges) ||
      !ts_parser_set_included_ranges(runtime.parser, included_ranges.data(),
                                     static_cast<uint32_t>(
                                         included_ranges.size()))) {
    result.error_code = "ERR_SYNTAX_INCLUDED_RANGES";
    result.error_message =
        "Tree-sitter rejected the injected language included ranges";
    return false;
  }
  const bool included_ranges_unchanged =
      same_included_ranges(runtime.included_ranges,
                           configuration.included_ranges);

  TSTree *old_tree = nullptr;
  if (included_ranges_unchanged && runtime.tree != nullptr &&
      runtime.tree_revision + 1 == revision &&
      runtime.tree_analysis != nullptr) {
    if (edits.empty() && analysis->checksum_complete &&
        runtime.tree_analysis->checksum_complete &&
        runtime.tree_analysis->checksum == analysis->checksum) {
      old_tree = ts_tree_copy(runtime.tree);
      result.incremental = true;
    } else if (edits.size() == 1) {
      TSInputEdit input_edit{};
      if (build_input_edit(edits.front(), *runtime.tree_analysis, *analysis,
                           input_edit)) {
        old_tree = ts_tree_copy(runtime.tree);
        ts_tree_edit(old_tree, &input_edit);
        result.incremental = true;
      }
    }
  }

  InputPayload input_payload{&reader, &cancellation, false};
  TSInput input{&input_payload, read_snapshot, TSInputEncodingUTF16LE, nullptr};
  TSParseOptions options{const_cast<SyntaxCancellation *>(&cancellation),
                         cancel_parse};
  const auto started_at = Clock::now();
  TSTree *new_tree =
      ts_parser_parse_with_options(runtime.parser, old_tree, input, options);
  result.parse_milliseconds =
      std::chrono::duration<double, std::milli>(Clock::now() - started_at)
          .count();
  ts_tree_delete(old_tree);

  if (new_tree == nullptr) {
    ts_parser_reset(runtime.parser);
    if (cancellation.requested()) {
      result.cancelled = true;
      return true;
    }
    result.error_code = input_payload.read_failed
                            ? "ERR_SYNTAX_INPUT_READ_FAILED"
                            : "ERR_SYNTAX_PARSE_FAILED";
    result.error_message = input_payload.read_failed
                               ? "Tree-sitter could not read SnapshotLease data"
                               : "Tree-sitter returned no syntax tree";
    return false;
  }

  if (!analyze_tree(new_tree, cancellation, result)) {
    ts_tree_delete(new_tree);
    result.cancelled = true;
    return true;
  }
  if (cancellation.requested()) {
    ts_tree_delete(new_tree);
    result.cancelled = true;
    return true;
  }

  std::shared_ptr<const QueryIndexSnapshot> injection_index;
  QueryResolutionContext injection_resolution;
  injection_resolution.resolve_scopes = false;
  if (!runtime.query_engine->execute_one(
          "injectionsQuery", new_tree, reader, *analysis, revision,
          configuration.generation, QueryRange{}, injection_resolution,
          cancel_query, const_cast<SyntaxCancellation *>(&cancellation),
          injection_index, result.query_error, result.query_statistics)) {
    ts_tree_delete(new_tree);
    if (cancellation.requested() ||
        result.query_error.code == "ERR_SYNTAX_QUERY_CANCELLED") {
      result.cancelled = true;
      return true;
    }
    result.error_code = result.query_error.code;
    result.error_message = result.query_error.message;
    return false;
  }
  auto eager_queries = std::make_shared<SyntaxQuerySnapshot>();
  eager_queries->buffer_revision = revision;
  eager_queries->language_generation = configuration.generation;
  if (injection_index != nullptr)
    eager_queries->indices.emplace("injectionsQuery", *injection_index);
  result.query_snapshot = std::move(eager_queries);

  result.published_snapshot = PublishedSyntaxSnapshot::build(
      new_tree, runtime.query_engine, revision, configuration.generation);
  if (!result.published_snapshot) {
    ts_tree_delete(new_tree);
    result.error_code = "ERR_SYNTAX_SNAPSHOT_FAILED";
    result.error_message = "Unable to publish immutable syntax state";
    return false;
  }

  ts_tree_delete(runtime.tree);
  runtime.tree = new_tree;
  runtime.tree_revision = revision;
  runtime.tree_analysis = std::move(analysis);
  runtime.included_ranges = configuration.included_ranges;
  result.parsed = true;
  return true;
}

void SyntaxBackend::reset() { impl_->runtime.reset(); }

const SyntaxBackendCapabilities &syntax_backend_capabilities() {
  static const SyntaxBackendCapabilities capabilities;
  return capabilities;
}

SyntaxGrammarCacheDiagnostics syntax_grammar_cache_diagnostics() {
  return grammar_cache().diagnostics();
}

} // namespace document_engine
