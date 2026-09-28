#include "syntax/scope-resolver.h"

#include "syntax/js-regex.h"
#include "syntax/query-engine.h"
#include "text-bridge/snapshot-reader.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace document_engine {

namespace {

struct ResolvedRange {
  Point start_position;
  Point end_position;
  uint32_t start_index = 0;
  uint32_t end_index = 0;
};

struct RangeKey {
  uint32_t start = 0;
  uint32_t end = 0;
  bool operator==(const RangeKey &) const = default;
};

struct RangeKeyHash {
  size_t operator()(const RangeKey &value) const {
    return (static_cast<size_t>(value.start) << 1u) ^ value.end;
  }
};

bool has_prefix(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

std::string normalize_property(std::string_view value,
                               std::string_view prefix) {
  return has_prefix(value, prefix) ? std::string(value.substr(prefix.size()))
                                   : std::string(value);
}

std::vector<std::string_view> split_words(std::string_view value) {
  std::vector<std::string_view> words;
  size_t offset = 0;
  while (offset < value.size()) {
    while (offset < value.size() &&
           (value[offset] == ' ' || value[offset] == '\t' ||
            value[offset] == '\r' || value[offset] == '\n'))
      offset++;
    const size_t start = offset;
    while (offset < value.size() && value[offset] != ' ' &&
           value[offset] != '\t' && value[offset] != '\r' &&
           value[offset] != '\n')
      offset++;
    if (offset > start)
      words.push_back(value.substr(start, offset - start));
  }
  return words;
}

std::pair<std::string, std::optional<std::string>>
split_key_value(const std::string &value) {
  const size_t separator = value.find(' ');
  if (separator == std::string::npos)
    return {value, std::nullopt};
  return {value.substr(0, separator), value.substr(separator + 1)};
}

bool node_is_null(TSNode node) { return ts_node_is_null(node); }

TSNode related_node(TSNode node, std::string_view part) {
  if (node_is_null(node))
    return TSNode{};
  if (part == "parent")
    return ts_node_parent(node);
  if (part == "firstChild")
    return ts_node_child(node, 0);
  if (part == "lastChild") {
    const uint32_t count = ts_node_child_count(node);
    return count == 0 ? TSNode{} : ts_node_child(node, count - 1);
  }
  if (part == "firstNamedChild")
    return ts_node_named_child(node, 0);
  if (part == "lastNamedChild") {
    const uint32_t count = ts_node_named_child_count(node);
    return count == 0 ? TSNode{} : ts_node_named_child(node, count - 1);
  }
  if (part == "nextSibling")
    return ts_node_next_sibling(node);
  if (part == "previousSibling")
    return ts_node_prev_sibling(node);
  if (part == "nextNamedSibling")
    return ts_node_next_named_sibling(node);
  if (part == "previousNamedSibling")
    return ts_node_prev_named_sibling(node);
  return TSNode{};
}

TSNode resolve_node_descriptor(TSNode node, std::string_view descriptor) {
  size_t offset = 0;
  while (offset < descriptor.size()) {
    const size_t separator = descriptor.find('.', offset);
    const size_t end = separator == std::string_view::npos
                           ? descriptor.size()
                           : separator;
    const std::string_view part = descriptor.substr(offset, end - offset);
    node = related_node(node, part);
    if (node_is_null(node))
      return TSNode{};
    if (separator == std::string_view::npos)
      break;
    offset = separator + 1;
  }
  return node;
}

Point point_from_ts(TSPoint point) {
  return Point{point.row, point.column / 2u};
}

std::optional<Point> resolve_node_position(TSNode node,
                                           std::string_view descriptor) {
  const size_t separator = descriptor.rfind('.');
  const std::string_view property =
      separator == std::string_view::npos
          ? descriptor
          : descriptor.substr(separator + 1);
  if (separator != std::string_view::npos) {
    node = resolve_node_descriptor(node, descriptor.substr(0, separator));
    if (node_is_null(node))
      return std::nullopt;
  }
  if (property == "startPosition")
    return point_from_ts(ts_node_start_point(node));
  if (property == "endPosition")
    return point_from_ts(ts_node_end_point(node));
  return std::nullopt;
}

bool node_type_is(TSNode node, std::string_view expected) {
  if (node_is_null(node))
    return false;
  const char *type = ts_node_type(node);
  return type != nullptr && expected == type;
}

bool node_type_in(TSNode node, std::string_view expected) {
  for (std::string_view value : split_words(expected)) {
    if (node_type_is(node, value))
      return true;
  }
  return false;
}

bool node_text_utf16(const SnapshotReader &reader, TSNode node,
                     std::u16string &text) {
  const uint32_t start = ts_node_start_byte(node);
  const uint32_t end = ts_node_end_byte(node);
  return (start & 1u) == 0 && (end & 1u) == 0 && start <= end &&
         reader.append_range(start / 2u, end / 2u, text);
}

std::string utf16_to_utf8(std::u16string_view source) {
  std::string output;
  output.reserve(source.size());
  for (size_t index = 0; index < source.size(); index++) {
    uint32_t codepoint = source[index];
    if (codepoint >= 0xd800u && codepoint <= 0xdbffu &&
        index + 1 < source.size()) {
      const uint32_t trailing = source[index + 1];
      if (trailing >= 0xdc00u && trailing <= 0xdfffu) {
        codepoint = 0x10000u + ((codepoint - 0xd800u) << 10u) +
                    trailing - 0xdc00u;
        index++;
      }
    }
    if (codepoint <= 0x7fu) {
      output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffu) {
      output.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
      output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else if (codepoint <= 0xffffu) {
      output.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
      output.push_back(
          static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
      output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else {
      output.push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
      output.push_back(
          static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
      output.push_back(
          static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
      output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    }
  }
  return output;
}

bool is_js_whitespace(char16_t character) {
  if ((character >= u'\t' && character <= u'\r') || character == u' ' ||
      character == 0x00a0 || character == 0x1680 || character == 0x2028 ||
      character == 0x2029 || character == 0x202f || character == 0x205f ||
      character == 0x3000 || character == 0xfeff)
    return true;
  return character >= 0x2000 && character <= 0x200a;
}

const QueryProperty *find_property(const std::vector<QueryProperty> &values,
                                   std::string_view name) {
  for (auto iterator = values.rbegin(); iterator != values.rend(); ++iterator) {
    if (iterator->name == name)
      return &*iterator;
  }
  return nullptr;
}

bool has_property(const std::vector<QueryProperty> &values,
                  std::string_view name) {
  return find_property(values, name) != nullptr;
}

ResolvedRange inherent_range(TSNode node) {
  const TSPoint start = ts_node_start_point(node);
  const TSPoint end = ts_node_end_point(node);
  return ResolvedRange{point_from_ts(start), point_from_ts(end),
                       ts_node_start_byte(node) / 2u,
                       ts_node_end_byte(node) / 2u};
}

bool is_regex_adjustment(std::string_view name) {
  const std::string normalized = normalize_property(name, "adjust.");
  return normalized == "startAndEndAroundFirstMatchOf" ||
         normalized == "startBeforeFirstMatchOf" ||
         normalized == "startAfterFirstMatchOf" ||
         normalized == "endBeforeFirstMatchOf" ||
         normalized == "endAfterFirstMatchOf";
}

bool is_adjustment(std::string_view name) {
  const std::string normalized = normalize_property(name, "adjust.");
  return normalized == "startAt" || normalized == "endAt" ||
         normalized == "offsetStart" || normalized == "offsetEnd" ||
         is_regex_adjustment(name);
}

bool is_scope_test(std::string_view name) {
  const std::string normalized = normalize_property(name, "test.");
  static const std::set<std::string, std::less<>> tests = {
      "type",          "hasError",       "injection",
      "root",          "first",          "last",
      "firstOfType",   "lastOfType",     "lastTextOnRow",
      "firstTextOnRow", "descendantOfType", "childOfType",
      "field",         "typeAt",         "textAt",
      "matchAt",       "ancestorTypeNearerThan",
      "ancestorOfType", "parentOfType",   "rangeWithData",
      "descendantOfNodeWithData", "startsOnSameRowAs",
      "endsOnSameRowAs", "config"};
  return tests.contains(normalized);
}

bool scope_test_requires_value(std::string_view name) {
  const std::string normalized = normalize_property(name, "test.");
  static const std::set<std::string, std::less<>> tests = {
      "type", "descendantOfType", "childOfType", "field", "typeAt",
      "textAt", "matchAt", "ancestorTypeNearerThan", "ancestorOfType",
      "parentOfType", "rangeWithData", "descendantOfNodeWithData",
      "startsOnSameRowAs", "endsOnSameRowAs", "config"};
  return tests.contains(normalized);
}

bool is_capture_setting(std::string_view name) {
  if (name.find('.') != std::string_view::npos &&
      !has_prefix(name, "capture."))
    return false;
  const std::string normalized = normalize_property(name, "capture.");
  return normalized == "final" || normalized == "shy";
}

} // namespace

struct NativeScopeResolver::Impl {
  Impl(const SnapshotReader &reader_value,
       const SnapshotAnalysis &analysis_value,
       const QueryResolutionContext &context_value)
      : reader(reader_value), analysis(analysis_value), context(context_value) {}

  const SnapshotReader &reader;
  const SnapshotAnalysis &analysis;
  const QueryResolutionContext &context;
  ScopeResolutionStatistics statistics;
  std::unordered_map<RangeKey, std::vector<QueryProperty>, RangeKeyHash>
      range_data;
  std::unordered_map<std::string, JsRegex> regexes;

  uint32_t position_to_index(Point point) const {
    if (analysis.line_starts.empty())
      return 0;
    const uint64_t row =
        std::min<uint64_t>(point.row, analysis.line_starts.size() - 1);
    const uint64_t start = analysis.line_starts[row];
    const uint64_t end = analysis.line_ends[row];
    return static_cast<uint32_t>(
        std::min<uint64_t>(start + std::min<uint64_t>(point.column, end - start),
                           std::numeric_limits<uint32_t>::max()));
  }

  Point index_to_position(uint32_t raw_index) const {
    if (analysis.line_starts.empty())
      return Point{};
    const uint64_t index = std::min<uint64_t>(raw_index, analysis.utf16_length);
    auto iterator =
        std::upper_bound(analysis.line_starts.begin(), analysis.line_starts.end(),
                         index);
    size_t row = iterator == analysis.line_starts.begin()
                     ? 0
                     : static_cast<size_t>(iterator - analysis.line_starts.begin() - 1);
    const uint64_t column = std::min<uint64_t>(
        index - analysis.line_starts[row],
        analysis.line_ends[row] - analysis.line_starts[row]);
    return Point{row, column};
  }

  JsRegex *regex_for(const std::string &pattern) {
    auto [iterator, inserted] = regexes.try_emplace(pattern, pattern);
    return iterator->second.valid() ? &iterator->second : nullptr;
  }

  bool apply_adjustment(const QueryProperty &property, TSNode node,
                        ResolvedRange &range) {
    const std::string name = normalize_property(property.name, "adjust.");
    if (name == "startAt" || name == "endAt") {
      if (!property.has_value)
        return false;
      const std::optional<Point> point =
          resolve_node_position(node, property.value);
      if (!point)
        return false;
      const uint32_t index = position_to_index(*point);
      if (name == "startAt") {
        range.start_position = *point;
        range.start_index = index;
      } else {
        range.end_position = *point;
        range.end_index = index;
      }
      return true;
    }
    if (name == "offsetStart" || name == "offsetEnd") {
      char *end = nullptr;
      const std::string raw_value = property.has_value ? property.value : "0";
      const double number = std::strtod(raw_value.c_str(), &end);
      if (end == raw_value.c_str() || *end != '\0' ||
          !std::isfinite(number))
        return false;
      const int64_t offset = static_cast<int64_t>(number);
      const uint32_t current =
          name == "offsetStart" ? range.start_index : range.end_index;
      const int64_t moved = std::clamp<int64_t>(
          static_cast<int64_t>(current) + offset, 0,
          static_cast<int64_t>(std::min<uint64_t>(
              analysis.utf16_length, std::numeric_limits<uint32_t>::max())));
      const Point point = index_to_position(static_cast<uint32_t>(moved));
      if (name == "offsetStart") {
        range.start_index = static_cast<uint32_t>(moved);
        range.start_position = point;
      } else {
        range.end_index = static_cast<uint32_t>(moved);
        range.end_position = point;
      }
      return true;
    }

    JsRegex *regex = regex_for(property.has_value ? property.value : "null");
    if (regex == nullptr)
      return false;
    std::u16string text;
    if (!node_text_utf16(reader, node, text))
      return false;
    JsRegex::Match match;
    if (!regex->search(text, match))
      return false;
    const uint32_t base = ts_node_start_byte(node) / 2u;
    const uint32_t match_start = base + static_cast<uint32_t>(match.start);
    const uint32_t match_end = base + static_cast<uint32_t>(match.end);
    if (name == "startAndEndAroundFirstMatchOf") {
      range.start_index = match_start;
      range.start_position = index_to_position(match_start);
      range.end_index = match_end;
      range.end_position = index_to_position(match_end);
      return true;
    }
    if (name == "startBeforeFirstMatchOf") {
      range.start_index = match_start;
      range.start_position = index_to_position(match_start);
      return true;
    }
    if (name == "startAfterFirstMatchOf") {
      range.start_index = match_end;
      range.start_position = index_to_position(match_end);
      return true;
    }
    if (name == "endBeforeFirstMatchOf") {
      range.end_index = match_start;
      range.end_position = index_to_position(match_start);
      return true;
    }
    if (name == "endAfterFirstMatchOf") {
      range.end_index = match_end;
      range.end_position = index_to_position(match_end);
      return true;
    }
    return false;
  }

  bool range_is_valid(const ResolvedRange &range) const {
    return range.start_index <= range.end_index &&
           range.start_position < range.end_position;
  }

  const std::vector<QueryProperty> *data_for(const ResolvedRange &range) const {
    const auto iterator =
        range_data.find(RangeKey{range.start_index, range.end_index});
    return iterator == range_data.end() ? nullptr : &iterator->second;
  }

  const std::vector<QueryProperty> *data_for_node(TSNode node) const {
    return data_for(inherent_range(node));
  }

  bool text_on_row(uint32_t row, uint32_t start_column, uint32_t end_column,
                   bool before) const {
    if (row >= analysis.line_starts.size())
      return false;
    const uint64_t line_start = analysis.line_starts[row];
    const uint64_t line_end = analysis.line_ends[row];
    uint64_t start = line_start + std::min<uint64_t>(start_column,
                                                     line_end - line_start);
    uint64_t end = line_start + std::min<uint64_t>(end_column,
                                                   line_end - line_start);
    if (before)
      start = line_start;
    else
      end = line_end;
    for (uint64_t index = start; index < end; index++) {
      if (!is_js_whitespace(reader.character_at(index)))
        return true;
    }
    return false;
  }

  bool has_descendant_of_type(TSNode node, std::string_view types) const {
    std::vector<TSNode> stack;
    const uint32_t count = ts_node_child_count(node);
    stack.reserve(count);
    for (uint32_t index = 0; index < count; index++)
      stack.push_back(ts_node_child(node, index));
    while (!stack.empty()) {
      TSNode current = stack.back();
      stack.pop_back();
      if (node_type_in(current, types))
        return true;
      const uint32_t child_count = ts_node_child_count(current);
      for (uint32_t index = 0; index < child_count; index++)
        stack.push_back(ts_node_child(current, index));
    }
    return false;
  }

  bool config_test(const std::string &raw) const {
    const auto [key, expected] = split_key_value(raw);
    if (key.empty())
      return true;
    const auto iterator = context.config.find(key);
    const QueryConfigValue value = iterator == context.config.end()
                                       ? QueryConfigValue{std::monostate{}}
                                       : iterator->second;
    if (!expected) {
      if (const bool *boolean = std::get_if<bool>(&value))
        return *boolean;
      if (const double *number = std::get_if<double>(&value))
        return *number != 0 && !std::isnan(*number);
      if (const std::string *text = std::get_if<std::string>(&value))
        return !text->empty();
      return false;
    }
    if (*expected == "true" || *expected == "false") {
      const bool wanted = *expected == "true";
      const bool *actual = std::get_if<bool>(&value);
      return actual != nullptr && *actual == wanted;
    }
    if (!expected->empty() &&
        std::all_of(expected->begin(), expected->end(),
                    [](char character) { return character >= '0' && character <= '9'; })) {
      const double *actual = std::get_if<double>(&value);
      return actual != nullptr && *actual == std::strtod(expected->c_str(), nullptr);
    }
    const std::string *actual = std::get_if<std::string>(&value);
    return actual != nullptr && *actual == *expected;
  }

  bool is_local(const ResolvedRange &range) const {
    return std::any_of(context.local_ranges.begin(), context.local_ranges.end(),
                       [&](const auto &candidate) {
                         return candidate.first == range.start_index &&
                                candidate.second == range.end_index;
                       });
  }

  bool apply_test(const QueryProperty &property, TSNode node,
                  const ResolvedRange &range,
                  const std::vector<QueryProperty> *existing) {
    const std::string name = normalize_property(property.name, "test.");
    const std::optional<std::string> value =
        property.has_value ? std::optional<std::string>(property.value)
                           : std::nullopt;
    if (name == "type")
      return value && node_type_in(node, *value);
    if (name == "hasError")
      return ts_node_has_error(node);
    if (name == "injection")
      return context.injection_depth > 0;
    if (name == "root")
      return node_is_null(ts_node_parent(node));
    if (name == "first" || name == "last") {
      const TSNode parent = ts_node_parent(node);
      if (node_is_null(parent))
        return true;
      const uint32_t count = ts_node_child_count(parent);
      if (count == 0)
        return false;
      const TSNode sibling =
          ts_node_child(parent, name == "first" ? 0 : count - 1);
      return ts_node_eq(sibling, node);
    }
    if (name == "firstOfType" || name == "lastOfType") {
      const TSNode parent = ts_node_parent(node);
      if (node_is_null(parent))
        return true;
      const uint32_t count = ts_node_child_count(parent);
      if (name == "firstOfType") {
        for (uint32_t index = 0; index < count; index++) {
          const TSNode sibling = ts_node_child(parent, index);
          if (ts_node_eq(sibling, node))
            return true;
          if (node_type_is(sibling, ts_node_type(node)))
            return false;
        }
      } else {
        for (uint32_t index = count; index > 0; index--) {
          const TSNode sibling = ts_node_child(parent, index - 1);
          if (ts_node_eq(sibling, node))
            return true;
          if (node_type_is(sibling, ts_node_type(node)))
            return false;
        }
      }
      return false;
    }
    if (name == "lastTextOnRow") {
      const Point end = point_from_ts(ts_node_end_point(node));
      return !text_on_row(static_cast<uint32_t>(end.row),
                          static_cast<uint32_t>(end.column),
                          static_cast<uint32_t>(end.column), false);
    }
    if (name == "firstTextOnRow") {
      const Point start = point_from_ts(ts_node_start_point(node));
      return !text_on_row(static_cast<uint32_t>(start.row),
                          static_cast<uint32_t>(start.column),
                          static_cast<uint32_t>(start.column), true);
    }
    if (name == "descendantOfType" || name == "childOfType") {
      if (!value)
        return false;
      TSNode current = ts_node_parent(node);
      if (name == "childOfType")
        return !node_is_null(current) && node_type_in(current, *value);
      while (!node_is_null(current)) {
        if (node_type_in(current, *value))
          return true;
        current = ts_node_parent(current);
      }
      return false;
    }
    if (name == "field") {
      if (!value)
        return false;
      const TSNode parent = ts_node_parent(node);
      if (node_is_null(parent))
        return false;
      for (std::string_view field : split_words(*value)) {
        const TSNode child = ts_node_child_by_field_name(
            parent, field.data(), static_cast<uint32_t>(field.size()));
        if (!node_is_null(child) && ts_node_eq(child, node))
          return true;
      }
      return false;
    }
    if (name == "typeAt" || name == "textAt" || name == "matchAt") {
      if (!value)
        return false;
      const auto [descriptor, operand] = split_key_value(*value);
      if (descriptor.empty() || !operand)
        return false;
      const TSNode target = resolve_node_descriptor(node, descriptor);
      if (node_is_null(target))
        return false;
      if (name == "typeAt")
        return node_type_in(target, *operand);
      std::u16string text;
      if (!node_text_utf16(reader, target, text))
        return false;
      if (name == "textAt")
        return utf16_to_utf8(text) == *operand;
      JsRegex *regex = regex_for(*operand);
      JsRegex::Match match;
      return regex != nullptr && regex->search(text, match);
    }
    if (name == "ancestorTypeNearerThan") {
      if (!value)
        return false;
      const std::vector<std::string_view> types = split_words(*value);
      if (types.empty())
        return false;
      TSNode current = ts_node_parent(node);
      while (!node_is_null(current)) {
        for (size_t index = 1; index < types.size(); index++) {
          if (node_type_is(current, types[index]))
            return false;
        }
        if (node_type_is(current, types.front()))
          return true;
        current = ts_node_parent(current);
      }
      return false;
    }
    if (name == "ancestorOfType" || name == "parentOfType") {
      if (!value)
        return false;
      if (name == "ancestorOfType")
        return has_descendant_of_type(node, *value);
      const uint32_t count = ts_node_child_count(node);
      for (uint32_t index = 0; index < count; index++) {
        if (node_type_in(ts_node_child(node, index), *value))
          return true;
      }
      return false;
    }
    if (name == "rangeWithData") {
      if (!value)
        return false;
      if (existing == nullptr)
        return false;
      const auto [key, expected] = split_key_value(*value);
      if (key.empty())
        return true;
      const QueryProperty *stored = find_property(*existing, key);
      if (stored == nullptr)
        return false;
      return !expected || (stored->has_value && stored->value == *expected);
    }
    if (name == "descendantOfNodeWithData") {
      if (!value)
        return false;
      const auto [key, expected] = split_key_value(*value);
      if (key.empty())
        return true;
      TSNode current = ts_node_parent(node);
      while (!node_is_null(current)) {
        const std::vector<QueryProperty> *data = data_for_node(current);
        if (data != nullptr) {
          const QueryProperty *stored = find_property(*data, key);
          if (stored != nullptr &&
              (!expected || (stored->has_value && stored->value == *expected)))
            return true;
        }
        current = ts_node_parent(current);
      }
      return false;
    }
    if (name == "startsOnSameRowAs" || name == "endsOnSameRowAs") {
      if (!value)
        return false;
      const std::optional<Point> other = resolve_node_position(node, *value);
      if (!other)
        return false;
      const Point own = name == "startsOnSameRowAs"
                            ? point_from_ts(ts_node_start_point(node))
                            : point_from_ts(ts_node_end_point(node));
      return own.row == other->row;
    }
    if (name == "config")
      return value && config_test(*value);
    return true;
  }

  std::string interpolate_name(const std::string &name, TSNode node) const {
    std::string result = name;
    if (result.find("_TEXT_") != std::string::npos) {
      std::u16string text;
      if (node_text_utf16(reader, node, text)) {
        const std::string utf8 = utf16_to_utf8(text);
        if (utf8.find(' ') == std::string::npos)
          result.replace(result.find("_TEXT_"), 6, utf8);
      }
    }
    const size_t type_offset = result.find("_TYPE_");
    if (type_offset != std::string::npos)
      result.replace(type_offset, 6, ts_node_type(node));
    return result;
  }

  bool resolve(TSNode node, const std::string &capture_name,
               const QueryPatternMetadata &metadata,
               ResolvedQueryCapture &capture) {
    statistics.tested++;
    ResolvedRange range = inherent_range(node);
    bool adjusted = false;
    for (const QueryProperty &property : metadata.set_properties) {
      if (!is_adjustment(property.name))
        continue;
      adjusted = true;
      if (!apply_adjustment(property, node, range)) {
        statistics.invalid_adjustments++;
        statistics.rejected++;
        return false;
      }
    }
    if (adjusted)
      statistics.adjusted++;
    if (adjusted && !range_is_valid(range)) {
      statistics.invalid_adjustments++;
      statistics.rejected++;
      return false;
    }

    const std::vector<QueryProperty> *existing = data_for(range);
    if (existing != nullptr && has_property(*existing, "capture.final")) {
      statistics.rejected++;
      return false;
    }
    for (const QueryProperty &property : metadata.set_properties) {
      if (is_capture_setting(property.name)) {
        const std::string setting =
            normalize_property(property.name, "capture.");
        if ((setting == "final" && existing != nullptr &&
             has_property(*existing, "capture.final")) ||
            (setting == "shy" && existing != nullptr)) {
          statistics.rejected++;
          return false;
        }
      } else if (is_scope_test(property.name) &&
                 (scope_test_requires_value(property.name) &&
                      !property.has_value ||
                  !apply_test(property, node, range, existing))) {
        statistics.rejected++;
        return false;
      }
    }
    for (const QueryProperty &property : metadata.asserted_properties) {
      if (context.local_ranges_enabled && property.name == "local") {
        statistics.local_predicates++;
        if (!is_local(range)) {
          statistics.rejected++;
          return false;
        }
      } else if (is_scope_test(property.name) &&
                 (scope_test_requires_value(property.name) &&
                      !property.has_value ||
                  !apply_test(property, node, range, existing))) {
        statistics.rejected++;
        return false;
      }
    }
    for (const QueryProperty &property : metadata.refuted_properties) {
      if (context.local_ranges_enabled && property.name == "local") {
        statistics.local_predicates++;
        if (is_local(range)) {
          statistics.rejected++;
          return false;
        }
      } else if (is_scope_test(property.name) &&
                 (scope_test_requires_value(property.name) &&
                      !property.has_value ||
                  apply_test(property, node, range, existing))) {
          statistics.rejected++;
          return false;
      }
    }

    range_data[RangeKey{range.start_index, range.end_index}] =
        metadata.set_properties;
    capture.name = context.interpolate_names
                       ? interpolate_name(capture_name, node)
                       : capture_name;
    if (capture.name == "_IGNORE_" || has_prefix(capture.name, "_IGNORE_.")) {
      statistics.rejected++;
      return false;
    }
    capture.accepted = true;
    capture.start_row = static_cast<uint32_t>(range.start_position.row);
    capture.start_column = static_cast<uint32_t>(range.start_position.column);
    capture.end_row = static_cast<uint32_t>(range.end_position.row);
    capture.end_column = static_cast<uint32_t>(range.end_position.column);
    capture.start_index = range.start_index;
    capture.end_index = range.end_index;
    statistics.accepted++;
    return true;
  }
};

NativeScopeResolver::NativeScopeResolver(
    const SnapshotReader &reader, const SnapshotAnalysis &analysis,
    const QueryResolutionContext &context)
    : impl_(new Impl(reader, analysis, context)) {}

NativeScopeResolver::~NativeScopeResolver() { delete impl_; }

bool NativeScopeResolver::resolve(TSNode node,
                                  const std::string &capture_name,
                                  const QueryPatternMetadata &metadata,
                                  ResolvedQueryCapture &capture) {
  return impl_->resolve(node, capture_name, metadata, capture);
}

const ScopeResolutionStatistics &NativeScopeResolver::statistics() const {
  return impl_->statistics;
}

} // namespace document_engine
