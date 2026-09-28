#ifndef LUMINE_DOCUMENT_ENGINE_RENDER_PLAN_H_
#define LUMINE_DOCUMENT_ENGINE_RENDER_PLAN_H_

#include "core-types.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace document_engine {

class DisplayIndex;
class SnapshotReader;

enum BuiltInScopeFlag : uint32_t {
  BUILT_IN_HARD_TAB = 1u << 0u,
  BUILT_IN_LEADING_WHITESPACE = 1u << 2u,
  BUILT_IN_TRAILING_WHITESPACE = 1u << 3u,
  BUILT_IN_INVISIBLE_CHARACTER = 1u << 4u,
  BUILT_IN_LINE_ENDING = 1u << 6u,
  BUILT_IN_FOLD = 1u << 7u,
};

struct ScopedRange {
  uint32_t scope_id = 0;
  uint64_t start_offset = 0;
  uint64_t end_offset = 0;
  uint64_t order = 0;
  uint32_t depth = 0;
};

struct RenderStyle {
  std::u16string invisible_tab;
  std::u16string invisible_space;
  std::u16string invisible_eol;
  std::u16string invisible_cr;
  std::map<uint32_t, uint32_t> built_in_scope_ids;
};

struct RenderedLine {
  std::u16string text;
  std::vector<int32_t> tags;
};

class RenderPlanBuilder {
public:
  RenderPlanBuilder(const SnapshotReader &reader, const DisplayIndex &index,
                    const RenderStyle &style,
                    const std::vector<ScopedRange> &scopes);

  RenderedLine build_line(uint64_t screen_row,
                          uint64_t &copied_utf16) const;

private:
  struct Boundary {
    std::vector<const ScopedRange *> opens;
    std::vector<const ScopedRange *> closes;
  };

  std::vector<uint32_t> scopes_at(uint64_t offset) const;
  uint32_t built_in_scope(uint32_t flags) const;
  std::u16string line_ending_invisible(uint64_t buffer_row) const;

  const SnapshotReader &reader_;
  const DisplayIndex &index_;
  const RenderStyle &style_;
  std::vector<ScopedRange> scopes_;
  std::map<uint64_t, Boundary> boundaries_;
};

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_RENDER_PLAN_H_
