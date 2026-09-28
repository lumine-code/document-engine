#include "syntax/js-regex.h"

#define PCRE2_CODE_UNIT_WIDTH 16
#include <pcre2.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace document_engine {

namespace {

std::u16string utf8_to_utf16(std::string_view source) {
  std::u16string output;
  output.reserve(source.size());
  for (size_t index = 0; index < source.size();) {
    const uint8_t first = static_cast<uint8_t>(source[index++]);
    uint32_t codepoint = first;
    size_t trailing = 0;
    if ((first & 0xe0u) == 0xc0u) {
      codepoint = first & 0x1fu;
      trailing = 1;
    } else if ((first & 0xf0u) == 0xe0u) {
      codepoint = first & 0x0fu;
      trailing = 2;
    } else if ((first & 0xf8u) == 0xf0u) {
      codepoint = first & 0x07u;
      trailing = 3;
    } else if (first >= 0x80u) {
      codepoint = 0xfffdu;
    }
    bool valid = index + trailing <= source.size();
    for (size_t offset = 0; valid && offset < trailing; offset++) {
      const uint8_t next = static_cast<uint8_t>(source[index + offset]);
      if ((next & 0xc0u) != 0x80u) {
        valid = false;
      } else {
        codepoint = (codepoint << 6u) | (next & 0x3fu);
      }
    }
    if (!valid) {
      codepoint = 0xfffdu;
      trailing = 0;
    }
    index += trailing;
    if (codepoint <= 0xffffu) {
      output.push_back(static_cast<char16_t>(codepoint));
    } else if (codepoint <= 0x10ffffu) {
      codepoint -= 0x10000u;
      output.push_back(static_cast<char16_t>(0xd800u + (codepoint >> 10u)));
      output.push_back(static_cast<char16_t>(0xdc00u + (codepoint & 0x3ffu)));
    } else {
      output.push_back(u'\ufffd');
    }
  }
  return output;
}

// V8 accepts variable-length lookbehind while PCRE2 deliberately bounds it.
// Every variable-length lookbehind used by Lumine's query fleet is a leading
// positive assertion. Rewriting `(?<=prefix)suffix` to
// `(?:prefix)\K(?:suffix)` preserves the reported match span and removes the
// artificial lookbehind bound. Nested assertions and character classes are
// scanned structurally so escaped parentheses do not terminate the prefix.
std::string normalize_leading_lookbehind(const std::string &pattern) {
  if (!pattern.starts_with("(?<="))
    return pattern;
  bool escaped = false;
  bool character_class = false;
  int depth = 1;
  size_t end = std::string::npos;
  for (size_t index = 4; index < pattern.size(); index++) {
    const char character = pattern[index];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (character == '\\') {
      escaped = true;
      continue;
    }
    if (character == '[') {
      character_class = true;
      continue;
    }
    if (character == ']' && character_class) {
      character_class = false;
      continue;
    }
    if (character_class)
      continue;
    if (character == '(') {
      depth++;
    } else if (character == ')' && --depth == 0) {
      end = index;
      break;
    }
  }
  if (end == std::string::npos)
    return pattern;
  std::string normalized = "(?:";
  normalized.append(pattern, 4, end - 4);
  normalized.append(")\\K(?:");
  normalized.append(pattern, end + 1, std::string::npos);
  normalized.push_back(')');
  return normalized;
}

bool ascii_digit(char character) {
  return character >= '0' && character <= '9';
}

bool valid_brace_quantifier(std::string_view pattern, size_t offset) {
  size_t index = offset + 1;
  const size_t first_digit = index;
  while (index < pattern.size() && ascii_digit(pattern[index]))
    index++;
  if (index == first_digit)
    return false;
  if (index < pattern.size() && pattern[index] == ',') {
    index++;
    while (index < pattern.size() && ascii_digit(pattern[index]))
      index++;
  }
  return index < pattern.size() && pattern[index] == '}';
}

std::string normalize_literal_braces(std::string_view pattern) {
  std::string output;
  output.reserve(pattern.size() + 4);
  bool escaped = false;
  bool character_class = false;
  for (size_t index = 0; index < pattern.size(); index++) {
    const char character = pattern[index];
    if (escaped) {
      output.push_back(character);
      escaped = false;
      continue;
    }
    if (character == '\\') {
      output.push_back(character);
      escaped = true;
      continue;
    }
    if (character == '[')
      character_class = true;
    else if (character == ']' && character_class)
      character_class = false;
    if (character == '{' && !character_class &&
        !valid_brace_quantifier(pattern, index))
      output.push_back('\\');
    output.push_back(character);
  }
  return output;
}

std::string pcre_error_message(int code, PCRE2_SIZE offset) {
  PCRE2_UCHAR16 buffer[256]{};
  const int length = pcre2_get_error_message(code, buffer, std::size(buffer));
  std::string result = "regular expression failed at UTF-16 offset " +
                       std::to_string(static_cast<uint64_t>(offset));
  if (length > 0) {
    result.append(": ");
    for (int index = 0; index < length; index++) {
      const char16_t character = static_cast<char16_t>(buffer[index]);
      result.push_back(character <= 0x7fu ? static_cast<char>(character) : '?');
    }
  }
  return result;
}

} // namespace

struct JsRegex::Impl {
  explicit Impl(pcre2_code *value) : code(value) {}
  ~Impl() { pcre2_code_free(code); }
  pcre2_code *code = nullptr;
};

JsRegex::JsRegex() = default;

JsRegex::JsRegex(const std::string &pattern) {
  const std::string normalized = normalize_literal_braces(pattern);
  const std::u16string utf16 =
      utf8_to_utf16(normalize_leading_lookbehind(normalized));
  int error_code = 0;
  PCRE2_SIZE error_offset = 0;
  // JavaScript's `$` is end-only without the multiline flag. PCRE2 otherwise
  // also accepts a position before one trailing newline.
  const uint32_t options = PCRE2_ALT_BSUX | PCRE2_MATCH_UNSET_BACKREF |
                           PCRE2_DOLLAR_ENDONLY;
  pcre2_code *code = pcre2_compile(
      reinterpret_cast<PCRE2_SPTR16>(utf16.data()), utf16.size(), options,
      &error_code, &error_offset, nullptr);
  if (code == nullptr) {
    error_ = pcre_error_message(error_code, error_offset);
    return;
  }
  impl_ = std::make_shared<Impl>(code);
}

JsRegex::~JsRegex() = default;

bool JsRegex::valid() const { return impl_ != nullptr; }

const std::string &JsRegex::error() const { return error_; }

bool JsRegex::search(std::u16string_view subject, Match &match) const {
  if (!impl_ || subject.size() > std::numeric_limits<PCRE2_SIZE>::max())
    return false;
  pcre2_match_data *data = pcre2_match_data_create_from_pattern(
      impl_->code, nullptr);
  if (data == nullptr)
    return false;
  const int result = pcre2_match(
      impl_->code, reinterpret_cast<PCRE2_SPTR16>(subject.data()),
      static_cast<PCRE2_SIZE>(subject.size()), 0, 0, data, nullptr);
  bool matched = false;
  if (result >= 0) {
    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(data);
    if (ovector != nullptr && ovector[0] != PCRE2_UNSET &&
        ovector[1] != PCRE2_UNSET && ovector[0] <= ovector[1] &&
        ovector[1] <= subject.size()) {
      match.start = static_cast<size_t>(ovector[0]);
      match.end = static_cast<size_t>(ovector[1]);
      matched = true;
    }
  }
  pcre2_match_data_free(data);
  return matched;
}

} // namespace document_engine
