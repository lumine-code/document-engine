#ifndef LUMINE_DOCUMENT_ENGINE_JS_REGEX_H_
#define LUMINE_DOCUMENT_ENGINE_JS_REGEX_H_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace document_engine {

// A small immutable wrapper around PCRE2's 16-bit engine. Query files use
// JavaScript regular-expression syntax because the legacy implementation runs
// them through RegExp. PCRE2 is intentionally configured for UTF-16 code-unit
// matching (without Unicode mode), which preserves the indices expected by
// Tree-sitter and Superstring.
class JsRegex {
public:
  struct Match {
    size_t start = 0;
    size_t end = 0;
  };

  JsRegex();
  explicit JsRegex(const std::string &pattern);
  ~JsRegex();

  JsRegex(const JsRegex &) = default;
  JsRegex &operator=(const JsRegex &) = default;
  JsRegex(JsRegex &&) noexcept = default;
  JsRegex &operator=(JsRegex &&) noexcept = default;

  bool valid() const;
  const std::string &error() const;
  bool search(std::u16string_view subject, Match &match) const;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  std::string error_;
};

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_JS_REGEX_H_
