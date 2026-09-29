#include "syntax/query-engine.h"

#include "syntax/js-regex.h"
#include "syntax/scope-resolver.h"
#include "text-bridge/snapshot-reader.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace document_engine {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<std::string_view, 6> QUERY_TYPES = {
    "highlightsQuery", "foldsQuery", "indentsQuery", "localsQuery",
    "tagsQuery", "injectionsQuery"};

struct SourceSpan {
  std::string file_path;
  uint32_t start = 0;
  uint32_t end = 0;
  std::string contents;
};

enum class TextPredicateKind {
  Equal,
  Match,
  AnyOf,
};

struct TextPredicate {
  TextPredicateKind kind = TextPredicateKind::Equal;
  bool positive = true;
  bool match_all = true;
  uint32_t first_capture = 0;
  bool second_is_capture = false;
  uint32_t second_capture = 0;
  std::string value;
  std::vector<std::string> values;
  std::optional<JsRegex> regex;
  bool resolved = true;
};

struct Program {
  std::string query_type;
  std::string source;
  TSQuery *query = nullptr;
  std::vector<std::string> capture_names;
  std::vector<QueryPatternMetadata> patterns;
  std::vector<std::vector<TextPredicate>> text_predicates;
  std::vector<SourceSpan> source_spans;

  Program() = default;
  Program(const Program &) = delete;
  Program &operator=(const Program &) = delete;
  Program(Program &&other) noexcept
      : query_type(std::move(other.query_type)), source(std::move(other.source)),
        query(other.query),
        capture_names(std::move(other.capture_names)),
        patterns(std::move(other.patterns)),
        text_predicates(std::move(other.text_predicates)),
        source_spans(std::move(other.source_spans)) {
    other.query = nullptr;
  }
  Program &operator=(Program &&other) noexcept {
    if (this == &other)
      return *this;
    ts_query_delete(query);
    query_type = std::move(other.query_type);
    source = std::move(other.source);
    query = other.query;
    other.query = nullptr;
    capture_names = std::move(other.capture_names);
    patterns = std::move(other.patterns);
    text_predicates = std::move(other.text_predicates);
    source_spans = std::move(other.source_spans);
    return *this;
  }
  ~Program() { ts_query_delete(query); }
};

std::filesystem::path path_from_utf8(const std::string &value) {
  std::u8string encoded;
  encoded.reserve(value.size());
  for (const unsigned char character : value)
    encoded.push_back(static_cast<char8_t>(character));
  return std::filesystem::path(encoded);
}

bool read_text_file(const std::string &path, std::string &contents,
                    QueryErrorInfo &error) {
  std::ifstream stream(path_from_utf8(path), std::ios::binary | std::ios::ate);
  if (!stream) {
    error.code = "ERR_SYNTAX_QUERY_READ_FAILED";
    error.message = "Unable to open Tree-sitter query file: " + path;
    error.file_path = path;
    return false;
  }
  const std::streamoff length = stream.tellg();
  if (length < 0 ||
      static_cast<uint64_t>(length) >
          static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    error.code = "ERR_SYNTAX_QUERY_READ_FAILED";
    error.message = "Tree-sitter query file exceeds the uint32 size limit: " +
                    path;
    error.file_path = path;
    return false;
  }
  contents.resize(static_cast<size_t>(length));
  stream.seekg(0, std::ios::beg);
  if (length > 0 && !stream.read(contents.data(), length)) {
    error.code = "ERR_SYNTAX_QUERY_READ_FAILED";
    error.message = "Unable to read Tree-sitter query file: " + path;
    error.file_path = path;
    return false;
  }
  return true;
}

uint64_t replace_language_segment(std::string &contents,
                                  const std::string &language_segment) {
  constexpr std::string_view token = "._LANG_";
  if (language_segment.empty())
    return 0;
  const std::string replacement = "." + language_segment;
  uint64_t replacements = 0;
  size_t offset = 0;
  while ((offset = contents.find(token, offset)) != std::string::npos) {
    contents.replace(offset, token.size(), replacement);
    offset += replacement.size();
    replacements++;
  }
  return replacements;
}

bool has_language_token(const std::string &contents) {
  return contents.find("._LANG_") != std::string::npos;
}

std::string query_error_name(TSQueryError error) {
  switch (error) {
  case TSQueryErrorNone:
    return "none";
  case TSQueryErrorSyntax:
    return "syntax";
  case TSQueryErrorNodeType:
    return "node type";
  case TSQueryErrorField:
    return "field";
  case TSQueryErrorCapture:
    return "capture";
  case TSQueryErrorStructure:
    return "structure";
  case TSQueryErrorLanguage:
    return "language";
  }
  return "unknown";
}

void locate_query_error(const Program &program, uint32_t offset,
                        QueryErrorInfo &error) {
  error.byte_offset = offset;
  const SourceSpan *selected = nullptr;
  for (const SourceSpan &span : program.source_spans) {
    if (offset >= span.start && offset <= span.end) {
      selected = &span;
      break;
    }
    if (span.start <= offset)
      selected = &span;
  }
  if (selected == nullptr)
    return;
  error.file_path = selected->file_path;
  const uint32_t local_offset =
      offset <= selected->start ? 0 : offset - selected->start;
  const size_t bounded =
      std::min<size_t>(local_offset, selected->contents.size());
  error.line = 1;
  error.column = 1;
  size_t line_start = 0;
  for (size_t index = 0; index < bounded; index++) {
    if (selected->contents[index] == '\n') {
      error.line++;
      line_start = index + 1;
    }
  }
  error.column = static_cast<uint32_t>(bounded - line_start + 1);
}

bool append_program_source(
    const std::string &query_type,
    const std::vector<std::string> *configured_paths,
    const std::string *configured_source,
    const std::string &language_segment, Program &program,
    std::string &source, QueryErrorInfo &error,
    QueryRunStatistics &statistics) {
  auto append = [&](std::string contents, const std::string &file_path) {
    if (contents.empty())
      contents = "; (empty)";
    if (has_language_token(contents)) {
      if (language_segment.empty()) {
        statistics.language_segment_warnings++;
      } else {
        statistics.language_segment_substitutions +=
            replace_language_segment(contents, language_segment);
      }
    }
    if (source.size() >= std::numeric_limits<uint32_t>::max())
      return false;
    const uint32_t start = static_cast<uint32_t>(source.size() + 1);
    source.push_back('\n');
    source.append(contents);
    if (source.size() > std::numeric_limits<uint32_t>::max())
      return false;
    program.source_spans.push_back(SourceSpan{
        file_path, start, static_cast<uint32_t>(source.size()), contents});
    return true;
  };

  if (configured_paths != nullptr) {
    for (const std::string &path : *configured_paths) {
      std::string contents;
      if (!read_text_file(path, contents, error)) {
        error.query_type = query_type;
        return false;
      }
      statistics.files_loaded++;
      if (!append(std::move(contents), path)) {
        error.code = "ERR_SYNTAX_QUERY_TOO_LARGE";
        error.message = "Concatenated Tree-sitter query exceeds uint32 size";
        error.query_type = query_type;
        error.file_path = path;
        return false;
      }
    }
  }
  if (configured_source != nullptr) {
    if (!append(*configured_source, "<inline:" + query_type + ">")) {
      error.code = "ERR_SYNTAX_QUERY_TOO_LARGE";
      error.message = "Inline Tree-sitter query exceeds uint32 size";
      error.query_type = query_type;
      error.file_path = "<inline:" + query_type + ">";
      return false;
    }
  }
  return true;
}

std::string query_capture_name(const TSQuery *query, uint32_t id) {
  uint32_t length = 0;
  const char *name = ts_query_capture_name_for_id(query, id, &length);
  return name == nullptr ? std::string() : std::string(name, length);
}

std::string query_string_value(const TSQuery *query, uint32_t id) {
  uint32_t length = 0;
  const char *value = ts_query_string_value_for_id(query, id, &length);
  return value == nullptr ? std::string() : std::string(value, length);
}

bool is_operator(const std::string &value,
                 std::initializer_list<std::string_view> candidates) {
  return std::any_of(candidates.begin(), candidates.end(),
                     [&](std::string_view candidate) {
                       return value == candidate;
                     });
}

bool predicate_error(const Program &program, uint32_t pattern_index,
                     const std::string &message, QueryErrorInfo &error) {
  error.code = "ERR_SYNTAX_QUERY_PREDICATE_FAILED";
  error.query_type = program.query_type;
  error.message = message;
  if (program.query != nullptr) {
    const uint32_t offset =
        ts_query_start_byte_for_pattern(program.query, pattern_index);
    locate_query_error(program, offset, error);
  }
  return false;
}

bool parse_predicates(Program &program, QueryErrorInfo &error,
                      QueryRunStatistics &statistics) {
  const uint32_t capture_count = ts_query_capture_count(program.query);
  const uint32_t pattern_count = ts_query_pattern_count(program.query);
  program.capture_names.reserve(capture_count);
  for (uint32_t id = 0; id < capture_count; id++)
    program.capture_names.push_back(query_capture_name(program.query, id));
  program.patterns.resize(pattern_count);
  program.text_predicates.resize(pattern_count);

  for (uint32_t pattern_index = 0; pattern_index < pattern_count;
       pattern_index++) {
    uint32_t step_count = 0;
    const TSQueryPredicateStep *steps = ts_query_predicates_for_pattern(
        program.query, pattern_index, &step_count);
    std::vector<TSQueryPredicateStep> predicate;
    for (uint32_t step_index = 0; step_index < step_count; step_index++) {
      const TSQueryPredicateStep &step = steps[step_index];
      if (step.type != TSQueryPredicateStepTypeDone) {
        predicate.push_back(step);
        continue;
      }
      if (predicate.empty())
        continue;
      if (predicate.front().type != TSQueryPredicateStepTypeString)
        return predicate_error(program, pattern_index,
                               "Tree-sitter predicates must begin with a "
                               "literal operator",
                               error);
      const std::string operator_name =
          query_string_value(program.query, predicate.front().value_id);
      QueryPatternMetadata &metadata = program.patterns[pattern_index];

      if (is_operator(operator_name,
                      {"eq?", "not-eq?", "any-eq?", "any-not-eq?"})) {
        if (predicate.size() != 3 ||
            predicate[1].type != TSQueryPredicateStepTypeCapture)
          return predicate_error(program, pattern_index,
                                 "Invalid #" + operator_name +
                                     " predicate operands",
                                 error);
        TextPredicate output;
        output.kind = TextPredicateKind::Equal;
        output.positive = operator_name == "eq?" || operator_name == "any-eq?";
        output.match_all = !operator_name.starts_with("any-");
        output.first_capture = predicate[1].value_id;
        if (predicate[2].type == TSQueryPredicateStepTypeCapture) {
          output.second_is_capture = true;
          output.second_capture = predicate[2].value_id;
        } else if (predicate[2].type == TSQueryPredicateStepTypeString) {
          output.value = query_string_value(program.query,
                                            predicate[2].value_id);
        } else {
          return predicate_error(program, pattern_index,
                                 "Invalid #" + operator_name +
                                     " second operand",
                                 error);
        }
        program.text_predicates[pattern_index].push_back(std::move(output));
        statistics.text_predicates++;
      } else if (is_operator(operator_name,
                             {"match?", "not-match?", "any-match?",
                              "any-not-match?"})) {
        if (predicate.size() != 3 ||
            predicate[1].type != TSQueryPredicateStepTypeCapture ||
            predicate[2].type != TSQueryPredicateStepTypeString)
          return predicate_error(program, pattern_index,
                                 "Invalid #" + operator_name +
                                     " predicate operands",
                                 error);
        TextPredicate output;
        output.kind = TextPredicateKind::Match;
        output.positive =
            operator_name == "match?" || operator_name == "any-match?";
        output.match_all = !operator_name.starts_with("any-");
        output.first_capture = predicate[1].value_id;
        output.value =
            query_string_value(program.query, predicate[2].value_id);
        output.regex.emplace(output.value);
        if (!output.regex->valid()) {
          return predicate_error(
              program, pattern_index,
              "Invalid JavaScript-compatible regular expression in #" +
                  operator_name + ": " + output.regex->error(),
              error);
        }
        program.text_predicates[pattern_index].push_back(std::move(output));
        statistics.text_predicates++;
      } else if (operator_name == "any-of?" ||
                 operator_name == "not-any-of?") {
        if (predicate.size() < 2 ||
            predicate[1].type != TSQueryPredicateStepTypeCapture)
          return predicate_error(program, pattern_index,
                                 "Invalid #" + operator_name +
                                     " predicate operands",
                                 error);
        TextPredicate output;
        output.kind = TextPredicateKind::AnyOf;
        output.positive = operator_name == "any-of?";
        output.first_capture = predicate[1].value_id;
        for (size_t index = 2; index < predicate.size(); index++) {
          if (predicate[index].type != TSQueryPredicateStepTypeString)
            return predicate_error(program, pattern_index,
                                   "Invalid #" + operator_name +
                                       " string operand",
                                   error);
          output.values.push_back(
              query_string_value(program.query, predicate[index].value_id));
        }
        program.text_predicates[pattern_index].push_back(std::move(output));
        statistics.text_predicates++;
      } else if (operator_name == "set!" || operator_name == "is?" ||
                 operator_name == "is-not?") {
        if (predicate.size() < 2 || predicate.size() > 3 ||
            predicate[1].type != TSQueryPredicateStepTypeString ||
            (predicate.size() == 3 &&
             predicate[2].type != TSQueryPredicateStepTypeString))
          return predicate_error(program, pattern_index,
                                 "Invalid #" + operator_name +
                                     " property operands",
                                 error);
        QueryProperty property;
        property.name =
            query_string_value(program.query, predicate[1].value_id);
        if (predicate.size() == 3) {
          property.has_value = true;
          property.value =
              query_string_value(program.query, predicate[2].value_id);
        }
        std::vector<QueryProperty> *properties =
            operator_name == "set!"
                ? &metadata.set_properties
                : operator_name == "is?" ? &metadata.asserted_properties
                                           : &metadata.refuted_properties;
        const auto existing = std::find_if(
            properties->begin(), properties->end(),
            [&](const QueryProperty &candidate) {
              return candidate.name == property.name;
            });
        if (existing == properties->end())
          properties->push_back(std::move(property));
        else
          *existing = std::move(property);
        if (operator_name != "set!")
          statistics.scope_predicates++;
      } else {
        QueryPredicate custom;
        custom.operator_name = operator_name;
        for (size_t index = 1; index < predicate.size(); index++) {
          const TSQueryPredicateStep &operand = predicate[index];
          custom.operands.push_back(QueryOperand{
              operand.type == TSQueryPredicateStepTypeCapture,
              operand.type == TSQueryPredicateStepTypeCapture
                  ? query_capture_name(program.query, operand.value_id)
                  : query_string_value(program.query, operand.value_id)});
        }
        metadata.custom_predicates.push_back(std::move(custom));
        statistics.custom_predicates++;
        // Unknown directives are data in web-tree-sitter. Consumers such as
        // tags and injections inspect them explicitly; preserving them is the
        // complete behavior, not an unresolved approximation.
      }
      predicate.clear();
    }
  }
  for (QueryPatternMetadata &metadata : program.patterns) {
    auto validate = [&](const std::vector<QueryProperty> &properties) {
      for (const QueryProperty &property : properties) {
        if (!property.has_value)
          continue;
        std::string pattern;
        const std::string_view name = property.name.starts_with("adjust.")
                                          ? std::string_view(property.name).substr(7)
                                          : std::string_view(property.name);
        if (name == "startAndEndAroundFirstMatchOf" ||
            name == "startBeforeFirstMatchOf" ||
            name == "startAfterFirstMatchOf" ||
            name == "endBeforeFirstMatchOf" ||
            name == "endAfterFirstMatchOf") {
          pattern = property.value;
        } else if (name == "test.matchAt" || name == "matchAt") {
          const size_t separator = property.value.find(' ');
          if (separator != std::string::npos)
            pattern = property.value.substr(separator + 1);
        }
        if (pattern.empty())
          continue;
        JsRegex regex(pattern);
        if (!regex.valid()) {
          metadata.scope_regex_complete = false;
          metadata.unresolved_predicates.push_back(QueryPredicate{
              "scope-regex",
              {QueryOperand{false, property.name},
               QueryOperand{false, pattern}}});
          statistics.unresolved_predicates++;
          statistics.unresolved_regex_predicates++;
        }
      }
    };
    validate(metadata.set_properties);
    validate(metadata.asserted_properties);
    validate(metadata.refuted_properties);
  }
  return true;
}

std::string utf16_to_utf8(const std::u16string &source) {
  std::string output;
  output.reserve(source.size());
  for (size_t index = 0; index < source.size(); index++) {
    uint32_t codepoint = source[index];
    if (codepoint >= 0xd800 && codepoint <= 0xdbff &&
        index + 1 < source.size()) {
      const uint32_t trailing = source[index + 1];
      if (trailing >= 0xdc00 && trailing <= 0xdfff) {
        codepoint = UINT32_C(0x10000) + ((codepoint - 0xd800) << 10) +
                    (trailing - 0xdc00);
        index++;
      }
    }
    if (codepoint <= 0x7f) {
      output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
      output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
      output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
      output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
      output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
      output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
      output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
  }
  return output;
}

bool node_text(const SnapshotReader &reader, TSNode node,
               std::string &output) {
  const uint32_t start_byte = ts_node_start_byte(node);
  const uint32_t end_byte = ts_node_end_byte(node);
  if ((start_byte & 1u) != 0 || (end_byte & 1u) != 0 ||
      start_byte > end_byte)
    return false;
  std::u16string utf16;
  if (!reader.append_range(start_byte / 2u, end_byte / 2u, utf16))
    return false;
  output = utf16_to_utf8(utf16);
  return true;
}

bool node_text_utf16(const SnapshotReader &reader, TSNode node,
                     std::u16string &output) {
  const uint32_t start_byte = ts_node_start_byte(node);
  const uint32_t end_byte = ts_node_end_byte(node);
  return (start_byte & 1u) == 0 && (end_byte & 1u) == 0 &&
         start_byte <= end_byte &&
         reader.append_range(start_byte / 2u, end_byte / 2u, output);
}

bool text_predicate_passes(const TextPredicate &predicate,
                           const SnapshotReader &reader,
                           const TSQueryMatch &match) {
  if (!predicate.resolved)
    return false;
  if (predicate.kind == TextPredicateKind::Match) {
    bool found = false;
    std::u16string text;
    for (uint16_t index = 0; index < match.capture_count; index++) {
      const TSQueryCapture &capture = match.captures[index];
      if (capture.index != predicate.first_capture)
        continue;
      found = true;
      text.clear();
      if (!node_text_utf16(reader, capture.node, text))
        return false;
      JsRegex::Match regex_match;
      const bool matched = predicate.regex &&
                           predicate.regex->search(text, regex_match);
      const bool accepted = predicate.positive ? matched : !matched;
      if (predicate.match_all && !accepted)
        return false;
      if (!predicate.match_all && accepted)
        return true;
    }
    if (!found)
      return !predicate.positive;
    return predicate.match_all;
  }
  std::vector<std::string> first;
  std::vector<std::string> second;
  for (uint16_t index = 0; index < match.capture_count; index++) {
    const TSQueryCapture &capture = match.captures[index];
    if (capture.index != predicate.first_capture &&
        (!predicate.second_is_capture ||
         capture.index != predicate.second_capture))
      continue;
    std::string text;
    if (!node_text(reader, capture.node, text))
      return false;
    if (capture.index == predicate.first_capture)
      first.push_back(text);
    if (predicate.second_is_capture &&
        capture.index == predicate.second_capture)
      second.push_back(std::move(text));
  }

  if (predicate.kind == TextPredicateKind::Equal) {
    auto compare = [&](const std::string &left, const std::string &right) {
      const bool equal = left == right;
      return predicate.positive ? equal : !equal;
    };
    auto matches = [&](const std::string &left) {
      if (!predicate.second_is_capture)
        return compare(left, predicate.value);
      return std::any_of(second.begin(), second.end(),
                         [&](const std::string &right) {
                           return compare(left, right);
                         });
    };
    return predicate.match_all
               ? std::all_of(first.begin(), first.end(), matches)
               : std::any_of(first.begin(), first.end(), matches);
  }

  if (first.empty())
    return !predicate.positive;
  const bool all_in_set =
      std::all_of(first.begin(), first.end(), [&](const std::string &text) {
        return std::find(predicate.values.begin(), predicate.values.end(),
                         text) != predicate.values.end();
      });
  return all_in_set == predicate.positive;
}

bool match_passes(const Program &program, const SnapshotReader &reader,
                  const TSQueryMatch &match) {
  if (match.pattern_index >= program.text_predicates.size())
    return false;
  for (const TextPredicate &predicate :
       program.text_predicates[match.pattern_index]) {
    if (!text_predicate_passes(predicate, reader, match))
      return false;
  }
  return true;
}

struct QueryCancellationPayload {
  QueryCancellationFunction function = nullptr;
  void *payload = nullptr;
};

bool cancel_query(TSQueryCursorState *state) {
  if (state == nullptr || state->payload == nullptr)
    return false;
  const auto *payload =
      static_cast<const QueryCancellationPayload *>(state->payload);
  return payload->function != nullptr && payload->function(payload->payload);
}

bool execute_program(const Program &program, const TSTree *tree,
                      const SnapshotReader &reader,
                      const SnapshotAnalysis &analysis, uint64_t revision,
                      uint64_t generation, const QueryRange &range,
                      const QueryResolutionContext &resolution,
                      QueryCancellationFunction cancellation,
                     void *cancellation_payload, QueryIndexSnapshot &output,
                     QueryErrorInfo &error) {
  output.query_type = program.query_type;
  output.buffer_revision = revision;
  output.language_generation = generation;
  output.capture_names = program.capture_names;
  output.patterns = program.patterns;
  output.resolution_complete = std::all_of(
      program.patterns.begin(), program.patterns.end(),
      [](const QueryPatternMetadata &metadata) {
        return metadata.scope_regex_complete;
      });
  std::unordered_map<std::string, uint32_t> capture_name_ids;
  for (uint32_t index = 0; index < output.capture_names.size(); index++)
    capture_name_ids.emplace(output.capture_names[index], index);
  NativeScopeResolver scope_resolver(reader, analysis, resolution);

  TSQueryCursor *cursor = ts_query_cursor_new();
  if (cursor == nullptr) {
    error.code = "ERR_SYNTAX_QUERY_EXECUTION_FAILED";
    error.query_type = program.query_type;
    error.message = "Tree-sitter could not allocate a query cursor";
    return false;
  }
  QueryCancellationPayload payload{cancellation, cancellation_payload};
  TSQueryCursorOptions options{&payload, cancel_query};
  const uint64_t start_column_bytes =
      static_cast<uint64_t>(range.start_column) * UINT64_C(2);
  const uint64_t end_column_bytes =
      static_cast<uint64_t>(range.end_column) * UINT64_C(2);
  const TSPoint start_point{
      range.start_row,
      static_cast<uint32_t>(std::min<uint64_t>(
          start_column_bytes, std::numeric_limits<uint32_t>::max()))};
  const TSPoint end_point{
      range.end_row,
      static_cast<uint32_t>(std::min<uint64_t>(
          end_column_bytes, std::numeric_limits<uint32_t>::max()))};
  ts_query_cursor_set_point_range(cursor, start_point, end_point);
  ts_query_cursor_exec_with_options(cursor, program.query,
                                    ts_tree_root_node(tree), &options);

  TSQueryMatch match{};
  uint32_t capture_index = 0;
  while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
    output.raw_capture_count++;
    if (cancellation != nullptr && cancellation(cancellation_payload)) {
      ts_query_cursor_delete(cursor);
      error.code = "ERR_SYNTAX_QUERY_CANCELLED";
      error.query_type = program.query_type;
      error.message = "Tree-sitter query execution was cancelled";
      return false;
    }
    if (!match_passes(program, reader, match))
      continue;
    if (capture_index >= match.capture_count)
      continue;
    const TSQueryCapture &capture = match.captures[capture_index];
    const uint32_t start_byte = ts_node_start_byte(capture.node);
    const uint32_t end_byte = ts_node_end_byte(capture.node);
    const TSPoint start_point = ts_node_start_point(capture.node);
    const TSPoint end_point = ts_node_end_point(capture.node);
    if ((start_byte & 1u) != 0 || (end_byte & 1u) != 0 ||
        (start_point.column & 1u) != 0 || (end_point.column & 1u) != 0)
      continue;
    ResolvedQueryCapture resolved;
    if (resolution.resolve_scopes) {
      if (match.pattern_index >= program.patterns.size() ||
          capture.index >= program.capture_names.size() ||
          !scope_resolver.resolve(capture.node,
                                  program.capture_names[capture.index],
                                  program.patterns[match.pattern_index],
                                  resolved))
        continue;
    } else {
      resolved.accepted = true;
      resolved.name = program.capture_names[capture.index];
      resolved.start_row = start_point.row;
      resolved.start_column = start_point.column / 2u;
      resolved.end_row = end_point.row;
      resolved.end_column = end_point.column / 2u;
      resolved.start_index = start_byte / 2u;
      resolved.end_index = end_byte / 2u;
    }
    uint32_t resolved_name_id = capture.index;
    const auto existing_name = capture_name_ids.find(resolved.name);
    if (existing_name != capture_name_ids.end()) {
      resolved_name_id = existing_name->second;
    } else {
      resolved_name_id = static_cast<uint32_t>(output.capture_names.size());
      output.capture_names.push_back(resolved.name);
      capture_name_ids.emplace(resolved.name, resolved_name_id);
    }
    output.captures.push_back(QueryCaptureRecord{
        resolved_name_id,
        match.pattern_index,
        match.pattern_index,
        resolved.start_row,
        resolved.start_column,
        resolved.end_row,
        resolved.end_column,
        resolved.start_index,
        resolved.end_index,
        match.id,
        static_cast<uint32_t>(ts_node_symbol(capture.node)),
        0,
        0,
        0,
        resolved.name == program.capture_names[capture.index]
            ? 0u
            : QUERY_CAPTURE_NAME_INTERPOLATED,
        start_byte / 2u,
        end_byte / 2u});
    output.accepted_capture_count++;
  }
  output.exceeded_match_limit = ts_query_cursor_did_exceed_match_limit(cursor);
  output.scope_statistics = scope_resolver.statistics();
  ts_query_cursor_delete(cursor);
  return true;
}

} // namespace

struct NativeQueryEngine::Impl {
  std::vector<Program> programs;
  uint64_t text_predicates = 0;
  uint64_t custom_predicates = 0;
  uint64_t unresolved_predicates = 0;
  uint64_t unresolved_regex_predicates = 0;
  uint64_t scope_predicates = 0;
};

const char *canonical_query_type(const std::string &query_type) {
  for (std::string_view candidate : QUERY_TYPES) {
    if (query_type == candidate)
      return candidate.data();
  }
  if (query_type == "highlights" || query_type == "highlight")
    return "highlightsQuery";
  if (query_type == "folds" || query_type == "fold")
    return "foldsQuery";
  if (query_type == "indents" || query_type == "indent")
    return "indentsQuery";
  if (query_type == "locals" || query_type == "local")
    return "localsQuery";
  if (query_type == "tags" || query_type == "tag")
    return "tagsQuery";
  if (query_type == "injections" || query_type == "injection")
    return "injectionsQuery";
  return nullptr;
}

NativeQueryEngine::NativeQueryEngine(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

NativeQueryEngine::~NativeQueryEngine() = default;

std::unique_ptr<NativeQueryEngine> NativeQueryEngine::compile(
    const TSLanguage *language, const QueryConfiguration &configuration,
    QueryErrorInfo &error, QueryRunStatistics &statistics) {
  error = QueryErrorInfo{};
  const auto started_at = Clock::now();
  auto impl = std::make_shared<Impl>();

  std::set<std::string> query_types;
  for (const auto &[query_type, _] : configuration.paths) {
    if (const char *canonical = canonical_query_type(query_type))
      query_types.insert(canonical);
  }
  for (const auto &[query_type, _] : configuration.sources) {
    if (const char *canonical = canonical_query_type(query_type))
      query_types.insert(canonical);
  }

  for (const std::string &query_type : query_types) {
    Program program;
    program.query_type = query_type;
    const auto paths = configuration.paths.find(query_type);
    const auto source = configuration.sources.find(query_type);
    const std::vector<std::string> *path_values =
        paths == configuration.paths.end() ? nullptr : &paths->second;
    const std::string *source_value =
        source == configuration.sources.end() ? nullptr : &source->second;
    std::string query_source;
    if (!append_program_source(query_type, path_values, source_value,
                               configuration.language_segment, program,
                               query_source, error, statistics))
      return nullptr;
    if (query_source.empty())
      continue;
    program.source = std::move(query_source);
    impl->programs.push_back(std::move(program));
  }

  std::ostringstream cache_key;
  cache_key << configuration.grammar_identity << '\n'
            << configuration.grammar_generation << '\n'
            << configuration.language_segment << '\n';
  for (const Program &program : impl->programs) {
    cache_key << program.query_type.size() << ':' << program.query_type
              << program.source.size() << ':' << program.source;
  }
  const std::string key = cache_key.str();

  struct CacheEntry {
    std::shared_ptr<const Impl> impl;
    uint64_t last_used = 0;
  };
  struct CacheState {
    std::mutex mutex;
    std::unordered_map<std::string, CacheEntry> entries;
    uint64_t clock = 0;
  };
  static CacheState *cache = new CacheState();
  std::unique_lock<std::mutex> cache_lock(cache->mutex);
  auto existing = cache->entries.find(key);
  if (existing != cache->entries.end()) {
    existing->second.last_used = ++cache->clock;
    statistics.cache_hits++;
    statistics.text_predicates += existing->second.impl->text_predicates;
    statistics.custom_predicates += existing->second.impl->custom_predicates;
    statistics.unresolved_predicates +=
        existing->second.impl->unresolved_predicates;
    statistics.unresolved_regex_predicates +=
        existing->second.impl->unresolved_regex_predicates;
    statistics.scope_predicates += existing->second.impl->scope_predicates;
    statistics.compile_milliseconds +=
        std::chrono::duration<double, std::milli>(Clock::now() - started_at)
            .count();
    return std::unique_ptr<NativeQueryEngine>(
        new NativeQueryEngine(existing->second.impl));
  }
  statistics.cache_misses++;

  for (Program &program : impl->programs) {

    uint32_t error_offset = 0;
    TSQueryError query_error = TSQueryErrorNone;
    program.query = ts_query_new(
        language, program.source.data(),
        static_cast<uint32_t>(program.source.size()),
        &error_offset, &query_error);
    if (program.query == nullptr) {
      error.code = "ERR_SYNTAX_QUERY_COMPILE_FAILED";
      error.query_type = program.query_type;
      error.message = "Tree-sitter " + query_error_name(query_error) +
                      " error while compiling " + program.query_type;
      locate_query_error(program, error_offset, error);
      if (!error.file_path.empty()) {
        std::ostringstream message;
        message << error.message << " at " << error.file_path << ':'
                << error.line << ':' << error.column;
        error.message = message.str();
      }
      return nullptr;
    }
    if (!parse_predicates(program, error, statistics))
      return nullptr;
    statistics.programs_compiled++;
  }
  impl->text_predicates = statistics.text_predicates;
  impl->custom_predicates = statistics.custom_predicates;
  impl->unresolved_predicates = statistics.unresolved_predicates;
  impl->unresolved_regex_predicates =
      statistics.unresolved_regex_predicates;
  impl->scope_predicates = statistics.scope_predicates;
  if (cache->entries.size() >= NATIVE_QUERY_CACHE_CAPACITY) {
    auto least_recent = std::min_element(
        cache->entries.begin(), cache->entries.end(),
        [](const auto &left, const auto &right) {
          return left.second.last_used < right.second.last_used;
        });
    if (least_recent != cache->entries.end()) {
      cache->entries.erase(least_recent);
      statistics.cache_evictions++;
    }
  }
  cache->entries.emplace(key, CacheEntry{impl, ++cache->clock});
  cache_lock.unlock();
  statistics.compile_milliseconds +=
      std::chrono::duration<double, std::milli>(Clock::now() - started_at)
          .count();
  return std::unique_ptr<NativeQueryEngine>(
      new NativeQueryEngine(std::move(impl)));
}

bool NativeQueryEngine::execute(
    const TSTree *tree, const SnapshotReader &reader,
    const SnapshotAnalysis &analysis, uint64_t buffer_revision,
    uint64_t language_generation, QueryCancellationFunction cancellation,
    void *cancellation_payload,
    std::shared_ptr<const SyntaxQuerySnapshot> &snapshot,
    QueryErrorInfo &error, QueryRunStatistics &statistics) const {
  error = QueryErrorInfo{};
  const auto started_at = Clock::now();
  auto output = std::make_shared<SyntaxQuerySnapshot>();
  output->buffer_revision = buffer_revision;
  output->language_generation = language_generation;
  for (std::string_view query_type : QUERY_TYPES) {
    QueryIndexSnapshot empty;
    empty.query_type = query_type;
    empty.buffer_revision = buffer_revision;
    empty.language_generation = language_generation;
    output->indices.emplace(std::string(query_type), std::move(empty));
  }
  for (const Program &program : impl_->programs) {
    statistics.patterns += program.patterns.size();
    QueryIndexSnapshot index;
    QueryResolutionContext resolution;
    if (program.query_type == "injectionsQuery")
      resolution.resolve_scopes = false;
    if (!execute_program(program, tree, reader, analysis, buffer_revision,
                         language_generation, QueryRange{}, resolution, cancellation,
                         cancellation_payload, index, error))
      return false;
    statistics.programs_executed++;
    statistics.captures += index.accepted_capture_count;
    statistics.scope_captures_tested += index.scope_statistics.tested;
    statistics.scope_captures_rejected += index.scope_statistics.rejected;
    statistics.scope_captures_adjusted += index.scope_statistics.adjusted;
    output->indices[program.query_type] = std::move(index);
  }
  statistics.execute_milliseconds +=
      std::chrono::duration<double, std::milli>(Clock::now() - started_at)
          .count();
  snapshot = std::move(output);
  return true;
}

bool NativeQueryEngine::execute_one(
    const std::string &query_type, const TSTree *tree,
    const SnapshotReader &reader, const SnapshotAnalysis &analysis,
    uint64_t buffer_revision, uint64_t language_generation,
    const QueryRange &range, const QueryResolutionContext &resolution,
    QueryCancellationFunction cancellation, void *cancellation_payload,
    std::shared_ptr<const QueryIndexSnapshot> &snapshot,
    QueryErrorInfo &error, QueryRunStatistics &statistics) const {
  error = QueryErrorInfo{};
  const char *canonical = canonical_query_type(query_type);
  if (canonical == nullptr) {
    error.code = "ERR_UNKNOWN_QUERY_TYPE";
    error.query_type = query_type;
    error.message = "Unknown Tree-sitter query type: " + query_type;
    return false;
  }
  const auto started_at = Clock::now();
  const auto program = std::find_if(
      impl_->programs.begin(), impl_->programs.end(),
      [&](const Program &candidate) { return candidate.query_type == canonical; });
  auto output = std::make_shared<QueryIndexSnapshot>();
  output->query_type = canonical;
  output->buffer_revision = buffer_revision;
  output->language_generation = language_generation;
  if (program != impl_->programs.end()) {
    statistics.patterns += program->patterns.size();
    if (!execute_program(*program, tree, reader, analysis, buffer_revision,
                         language_generation, range, resolution, cancellation,
                         cancellation_payload, *output, error))
      return false;
    statistics.programs_executed++;
    statistics.captures += output->accepted_capture_count;
    statistics.scope_captures_tested += output->scope_statistics.tested;
    statistics.scope_captures_rejected += output->scope_statistics.rejected;
    statistics.scope_captures_adjusted += output->scope_statistics.adjusted;
  }
  statistics.execute_milliseconds +=
      std::chrono::duration<double, std::milli>(Clock::now() - started_at)
          .count();
  snapshot = std::move(output);
  return true;
}

std::vector<std::string>
NativeQueryEngine::scope_config_keys(const std::string &query_type) const {
  const char *canonical = canonical_query_type(query_type);
  if (canonical == nullptr)
    return {};
  const auto program = std::find_if(
      impl_->programs.begin(), impl_->programs.end(),
      [&](const Program &candidate) { return candidate.query_type == canonical; });
  if (program == impl_->programs.end())
    return {};
  std::set<std::string> keys;
  auto collect = [&](const std::vector<QueryProperty> &properties) {
    for (const QueryProperty &property : properties) {
      const std::string_view name = property.name.starts_with("test.")
                                        ? std::string_view(property.name).substr(5)
                                        : std::string_view(property.name);
      if (name != "config" || !property.has_value)
        continue;
      const size_t separator = property.value.find(' ');
      const std::string key = property.value.substr(0, separator);
      if (!key.empty())
        keys.insert(key);
    }
  };
  for (const QueryPatternMetadata &metadata : program->patterns) {
    collect(metadata.set_properties);
    collect(metadata.asserted_properties);
    collect(metadata.refuted_properties);
  }
  return {keys.begin(), keys.end()};
}

} // namespace document_engine
