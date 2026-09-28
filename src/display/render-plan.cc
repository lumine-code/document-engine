#include "display/render-plan.h"

#include "display/display-index.h"
#include "text-bridge/snapshot-reader.h"

#include <algorithm>
#include <limits>

namespace document_engine {

namespace {

class TagEmitter {
public:
  explicit TagEmitter(RenderedLine &line) : line_(line) {}

  void emit_text(const std::u16string &text, bool reopen = true) {
    if (reopen)
      reopen_scopes();
    line_.text.append(text);
    token_length_ += text.size();
  }

  void emit_character(char16_t character) {
    reopen_scopes();
    line_.text.push_back(character);
    token_length_++;
  }

  void open_scope(uint32_t scope_id, bool reopen = true) {
    if (reopen)
      reopen_scopes();
    emit_token_boundary();
    if (scope_id == 0)
      return;
    containing_scopes_.push_back(scope_id);
    line_.tags.push_back(-static_cast<int32_t>(scope_id));
  }

  void close_scope(uint32_t scope_id) {
    emit_token_boundary();
    if (scope_id == 0)
      return;

    for (auto iterator = scopes_to_reopen_.rbegin();
         iterator != scopes_to_reopen_.rend(); iterator++) {
      if (*iterator == scope_id) {
        scopes_to_reopen_.erase(std::next(iterator).base());
        return;
      }
    }

    emit_empty_token_if_needed();
    while (!containing_scopes_.empty()) {
      const uint32_t containing = containing_scopes_.back();
      containing_scopes_.pop_back();
      line_.tags.push_back(-static_cast<int32_t>(containing) - 1);
      if (containing == scope_id)
        return;
      scopes_to_reopen_.insert(scopes_to_reopen_.begin(), containing);
    }
  }

  void replace_scopes(const std::vector<uint32_t> &scopes) {
    close_containing_scopes();
    scopes_to_reopen_.clear();
    for (uint32_t scope : scopes)
      open_scope(scope, false);
  }

  void close_containing_scopes() {
    if (!containing_scopes_.empty())
      emit_empty_token_if_needed();
    for (auto iterator = containing_scopes_.rbegin();
         iterator != containing_scopes_.rend(); iterator++)
      line_.tags.push_back(-static_cast<int32_t>(*iterator) - 1);
    containing_scopes_.clear();
    scopes_to_reopen_.clear();
  }

  void finish() {
    close_containing_scopes();
    emit_token_boundary();
    if (line_.tags.empty())
      line_.tags.push_back(0);
  }

private:
  void emit_token_boundary() {
    if (token_length_ == 0)
      return;
    line_.tags.push_back(static_cast<int32_t>(std::min<size_t>(
        token_length_, static_cast<size_t>(std::numeric_limits<int32_t>::max()))));
    token_length_ = 0;
  }

  void emit_empty_token_if_needed() {
    if (!line_.tags.empty() && line_.tags.back() < 0 &&
        (line_.tags.back() & 1) != 0)
      line_.tags.push_back(0);
  }

  void reopen_scopes() {
    for (uint32_t scope : scopes_to_reopen_) {
      containing_scopes_.push_back(scope);
      line_.tags.push_back(-static_cast<int32_t>(scope));
    }
    scopes_to_reopen_.clear();
  }

  RenderedLine &line_;
  std::vector<uint32_t> containing_scopes_;
  std::vector<uint32_t> scopes_to_reopen_;
  size_t token_length_ = 0;
};

bool range_order_less(const ScopedRange *left, const ScopedRange *right) {
  if (left->depth != right->depth)
    return left->depth < right->depth;
  return left->order < right->order;
}

} // namespace

RenderPlanBuilder::RenderPlanBuilder(
    const SnapshotReader &reader, const DisplayIndex &index,
    const RenderStyle &style, const std::vector<ScopedRange> &scopes)
    : reader_(reader), index_(index), style_(style) {
  scopes_.reserve(scopes.size());
  for (const ScopedRange &scope : scopes) {
    bool completely_hidden = false;
    for (const Fold &fold : index_.folds()) {
      const uint64_t fold_start = index_.line_start_offset(fold.range.start.row) +
                                  fold.range.start.column;
      const uint64_t fold_end = index_.line_start_offset(fold.range.end.row) +
                                fold.range.end.column;
      if (fold_start <= scope.start_offset && scope.end_offset <= fold_end) {
        completely_hidden = true;
        break;
      }
    }
    if (!completely_hidden)
      scopes_.push_back(scope);
  }
  for (const ScopedRange &scope : scopes_) {
    if (scope.scope_id == 0 || scope.start_offset >= scope.end_offset)
      continue;
    boundaries_[scope.start_offset].opens.push_back(&scope);
    boundaries_[scope.end_offset].closes.push_back(&scope);
  }
  for (auto &[offset, boundary] : boundaries_) {
    std::stable_sort(boundary.opens.begin(), boundary.opens.end(),
                     range_order_less);
    std::stable_sort(boundary.closes.begin(), boundary.closes.end(),
                     [](const ScopedRange *left, const ScopedRange *right) {
                       return range_order_less(right, left);
                     });
  }
}

std::vector<uint32_t> RenderPlanBuilder::scopes_at(uint64_t offset) const {
  std::vector<const ScopedRange *> active;
  for (const ScopedRange &scope : scopes_) {
    if (scope.scope_id != 0 && scope.start_offset <= offset &&
        offset < scope.end_offset)
      active.push_back(&scope);
  }
  std::stable_sort(active.begin(), active.end(), range_order_less);
  std::vector<uint32_t> result;
  result.reserve(active.size());
  for (const ScopedRange *scope : active)
    result.push_back(scope->scope_id);
  return result;
}

uint32_t RenderPlanBuilder::built_in_scope(uint32_t flags) const {
  if (flags == 0)
    return 0;
  const auto iterator = style_.built_in_scope_ids.find(flags);
  return iterator == style_.built_in_scope_ids.end() ? 0 : iterator->second;
}

std::u16string
RenderPlanBuilder::line_ending_invisible(uint64_t buffer_row) const {
  const uint64_t ending_start = index_.line_start_offset(buffer_row) +
                                index_.line_length_for_row(buffer_row);
  if (ending_start >= reader_.size())
    return {};
  std::u16string result;
  uint64_t offset = ending_start;
  if (reader_.character_at(offset) == u'\r') {
    result.append(style_.invisible_cr);
    offset++;
  }
  if (offset < reader_.size() && reader_.character_at(offset) == u'\n')
    result.append(style_.invisible_eol);
  return result;
}

RenderedLine RenderPlanBuilder::build_line(uint64_t screen_row,
                                           uint64_t &copied_utf16) const {
  RenderedLine result;
  const ScreenRow *row = index_.row(screen_row);
  if (row == nullptr) {
    result.tags.push_back(0);
    return result;
  }

  result.text.reserve(static_cast<size_t>(std::min<uint64_t>(
      row->visual_width, std::numeric_limits<size_t>::max())));
  TagEmitter emitter(result);
  if (row->leading_indent > 0) {
    emitter.emit_text(std::u16string(row->leading_indent, u' '), false);
    copied_utf16 += row->leading_indent;
  }

  const uint64_t row_start_offset =
      index_.line_start_offset(row->buffer_start.row) + row->buffer_start.column;
  emitter.replace_scopes(scopes_at(row_start_offset));
  bool have_source_position = true;
  uint64_t initialized_scope_offset = row_start_offset;
  uint32_t current_built_in_flags = 0;
  bool in_leading_whitespace = row->starts_in_leading_whitespace;
  for (const DisplaySpan &span : row->spans) {
    if (span.kind == DisplaySpan::Kind::FoldPlaceholder) {
      emitter.close_scope(built_in_scope(current_built_in_flags));
      current_built_in_flags = 0;
      emitter.close_containing_scopes();
      const uint32_t fold_scope = built_in_scope(BUILT_IN_FOLD);
      emitter.open_scope(fold_scope);
      emitter.emit_text(index_.options().fold_character);
      copied_utf16 += index_.options().fold_character.size();
      emitter.close_scope(fold_scope);
      const uint64_t next_offset = index_.line_start_offset(span.end.row) +
                                   span.end.column;
      emitter.replace_scopes(scopes_at(next_offset));
      have_source_position = true;
      initialized_scope_offset = next_offset;
      continue;
    }

    if (!have_source_position) {
      emitter.replace_scopes(scopes_at(span.start_offset));
      have_source_position = true;
      initialized_scope_offset = span.start_offset;
    }

    uint64_t screen_column = span.screen_start;
    for (uint64_t offset = span.start_offset; offset < span.end_offset;
         offset++) {
      const uint64_t buffer_column = span.start.column +
                                     (offset - span.start_offset);
      const char16_t character = reader_.character_at(offset);
      uint32_t flags = 0;
      const bool trailing =
          buffer_column >= index_.trailing_whitespace_start(span.start.row);
      if (trailing)
        in_leading_whitespace = false;
      const bool leading = in_leading_whitespace;
      if (character == u' ' || character == u'\t') {
        if (leading)
          flags |= BUILT_IN_LEADING_WHITESPACE;
        if (trailing)
          flags |= BUILT_IN_TRAILING_WHITESPACE;
      }
      if (character == u'\t') {
        flags |= BUILT_IN_HARD_TAB;
        if (!style_.invisible_tab.empty())
          flags |= BUILT_IN_INVISIBLE_CHARACTER;
      } else if (character == u' ' && (leading || trailing) &&
                 !style_.invisible_space.empty()) {
        flags |= BUILT_IN_INVISIBLE_CHARACTER;
      }

      const bool force_boundary = character == u'\t';
      const bool built_in_boundary =
          force_boundary || flags != current_built_in_flags;
      if (built_in_boundary) {
        emitter.close_scope(built_in_scope(current_built_in_flags));
        current_built_in_flags = flags;
      }

      if (offset != initialized_scope_offset) {
        const auto boundary = boundaries_.find(offset);
        if (boundary != boundaries_.end()) {
          for (const ScopedRange *scope : boundary->second.closes)
            emitter.close_scope(scope->scope_id);
          for (const ScopedRange *scope : boundary->second.opens)
            emitter.open_scope(scope->scope_id);
        }
      }
      initialized_scope_offset = UINT64_MAX;

      if (built_in_boundary)
        emitter.open_scope(built_in_scope(flags));

      if (character == u'\t') {
        const uint64_t remainder = screen_column % index_.options().tab_length;
        const uint64_t width = remainder == 0
                                   ? index_.options().tab_length
                                   : index_.options().tab_length - remainder;
        if (!style_.invisible_tab.empty()) {
          emitter.emit_text(style_.invisible_tab);
          copied_utf16 += style_.invisible_tab.size();
          if (width > 1) {
            emitter.emit_text(std::u16string(width - 1, u' '));
            copied_utf16 += width - 1;
          }
        } else {
          emitter.emit_text(std::u16string(width, u' '));
          copied_utf16 += width;
        }
        screen_column += width;
      } else if (character == u' ' && (leading || trailing) &&
                 !style_.invisible_space.empty()) {
        emitter.emit_text(style_.invisible_space);
        copied_utf16 += style_.invisible_space.size();
        screen_column++;
      } else {
        emitter.emit_character(character);
        copied_utf16++;
        screen_column++;
      }

      if (character != u' ' && character != u'\t')
        in_leading_whitespace = false;

      if (force_boundary) {
        emitter.close_scope(built_in_scope(flags));
        current_built_in_flags = 0;
      }
    }
  }

  emitter.close_scope(built_in_scope(current_built_in_flags));
  if (have_source_position) {
    const uint64_t end_offset = index_.line_start_offset(row->buffer_end.row) +
                                row->buffer_end.column;
    const auto boundary = boundaries_.find(end_offset);
    if (boundary != boundaries_.end()) {
      for (const ScopedRange *scope : boundary->second.closes)
        emitter.close_scope(scope->scope_id);
      // A soft-wrap boundary belongs to the following screen row. Opening a
      // scope there on this row creates an empty token at the previous row's
      // end; the next row's initial scopes_at() call opens it in the same place
      // as the legacy ScreenLineBuilder.
      if (!row->wraps_to_next) {
        for (const ScopedRange *scope : boundary->second.opens)
          emitter.open_scope(scope->scope_id);
      }
    }
  }

  if (!row->wraps_to_next &&
      row->buffer_end.column ==
          index_.line_length_for_row(row->buffer_end.row)) {
    const std::u16string ending = line_ending_invisible(row->buffer_end.row);
    if (!ending.empty()) {
      const uint32_t eol_scope = built_in_scope(
          BUILT_IN_INVISIBLE_CHARACTER | BUILT_IN_LINE_ENDING);
      emitter.open_scope(eol_scope);
      emitter.emit_text(ending, false);
      copied_utf16 += ending.size();
      emitter.close_scope(eol_scope);
    }
  }
  emitter.finish();
  return result;
}

} // namespace document_engine
