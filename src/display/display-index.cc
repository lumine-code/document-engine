#include "display/display-index.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace document_engine {

namespace {

bool range_is_empty(const Range &range) { return !(range.start < range.end); }

uint64_t clamp_column(const SnapshotAnalysis &analysis, uint64_t row,
                      uint64_t column) {
  if (analysis.line_starts.empty())
    return 0;
  row = std::min<uint64_t>(row, analysis.line_starts.size() - 1);
  return std::min<uint64_t>(column,
                            analysis.line_ends[row] - analysis.line_starts[row]);
}

Point clamp_point(const SnapshotAnalysis &analysis, Point point) {
  if (analysis.line_starts.empty())
    return Point{};
  point.row = std::min<uint64_t>(point.row, analysis.line_starts.size() - 1);
  point.column = clamp_column(analysis, point.row, point.column);
  return point;
}

uint64_t analysis_offset_for_point(const SnapshotAnalysis &analysis,
                                   Point point) {
  point = clamp_point(analysis, point);
  return analysis.line_starts[point.row] + point.column;
}

Point traversal(Point end, Point start) {
  if (end.row == start.row)
    return Point{0, end.column - start.column};
  return Point{end.row - start.row, end.column};
}

bool is_high_surrogate(char16_t character) {
  return character >= 0xd800 && character <= 0xdbff;
}

bool is_low_surrogate(char16_t character) {
  return character >= 0xdc00 && character <= 0xdfff;
}

bool is_variation_selector(char16_t character) {
  return character >= 0xfe00 && character <= 0xfe0f;
}

bool is_combining_character(char16_t character) {
  return (character >= 0x0300 && character <= 0x036f) ||
         (character >= 0x1ab0 && character <= 0x1aff) ||
         (character >= 0x1dc0 && character <= 0x1dff) ||
         (character >= 0x20d0 && character <= 0x20ff) ||
         (character >= 0xfe20 && character <= 0xfe2f);
}

bool is_double_width_character(char16_t character) {
  return (character >= 0x3000 && character <= 0x30ff) ||
         (character >= 0x4e00 && character <= 0x9fff) ||
         (character >= 0xff01 && character <= 0xff5e) ||
         (character >= 0xffe0 && character <= 0xffe6);
}

bool is_half_width_character(char16_t character) {
  return (character >= 0xff65 && character <= 0xffdc) ||
         (character >= 0xffe8 && character <= 0xffee);
}

bool is_korean_character(char16_t character) {
  return (character >= 0xac00 && character <= 0xd7a3) ||
         (character >= 0x1100 && character <= 0x11ff) ||
         (character >= 0x3130 && character <= 0x318f) ||
         (character >= 0xa960 && character <= 0xa97f) ||
         (character >= 0xd7b0 && character <= 0xd7ff);
}

bool is_cjk_character(char16_t character) {
  return is_double_width_character(character) ||
         is_half_width_character(character) || is_korean_character(character);
}

} // namespace

DisplayIndex::DisplayIndex(DisplayIndexOptions options)
    : options_(std::move(options)) {
  options_.tab_length = std::max<uint32_t>(1, options_.tab_length);
  if (options_.fold_character.empty())
    options_.fold_character = u"\u22ef";
}

std::vector<Fold> DisplayIndex::normalized_folds(
    const SnapshotAnalysis &analysis,
    const std::unordered_map<uint32_t, Fold> &folds) {
  std::vector<Fold> result;
  result.reserve(folds.size());
  for (const auto &[id, source] : folds) {
    Fold fold = source;
    fold.id = id;
    fold.range.start = clamp_point(analysis, fold.range.start);
    fold.range.end = clamp_point(analysis, fold.range.end);
    if (!range_is_empty(fold.range))
      result.push_back(fold);
  }

  std::sort(result.begin(), result.end(), [](const Fold &left, const Fold &right) {
    if (left.range.start != right.range.start)
      return left.range.start < right.range.start;
    return right.range.end < left.range.end;
  });

  std::vector<Fold> merged;
  merged.reserve(result.size());
  for (const Fold &fold : result) {
    if (merged.empty() || merged.back().range.end <= fold.range.start) {
      merged.push_back(fold);
    } else if (merged.back().range.end < fold.range.end) {
      merged.back().range.end = fold.range.end;
    }
  }
  return merged;
}

void DisplayIndex::append_source_segment(LogicalLine &line, Point start,
                                         Point end) const {
  if (start.row != end.row || !(start < end))
    return;
  line.segments.push_back(LogicalSegment{
      DisplaySpan::Kind::Source,
      analysis_offset_for_point(*analysis_, start),
      analysis_offset_for_point(*analysis_, end), start, end});
}

DisplayIndex::LogicalLine DisplayIndex::build_logical_line(
    uint64_t &row, size_t &fold_index) const {
  LogicalLine line;
  const uint64_t line_count = analysis_->line_starts.size();
  if (row >= line_count)
    return line;

  line.start = Point{row, 0};
  Point cursor = line.start;
  while (cursor.row < line_count) {
    cursor = clamp_point(*analysis_, cursor);
    while (fold_index < folds_.size() &&
           folds_[fold_index].range.end <= cursor) {
      fold_index++;
    }

    const Point line_end{
        cursor.row,
        analysis_->line_ends[cursor.row] - analysis_->line_starts[cursor.row]};
    const Fold *next_fold =
        fold_index < folds_.size() ? &folds_[fold_index] : nullptr;

    if (next_fold && next_fold->range.start < cursor &&
        cursor < next_fold->range.end) {
      cursor = next_fold->range.end;
      fold_index++;
      continue;
    }

    if (next_fold && next_fold->range.start.row == cursor.row &&
        cursor <= next_fold->range.start &&
        next_fold->range.start <= line_end) {
      append_source_segment(line, cursor, next_fold->range.start);
      line.segments.push_back(LogicalSegment{
          DisplaySpan::Kind::FoldPlaceholder, 0, 0, next_fold->range.start,
          next_fold->range.end});
      cursor = next_fold->range.end;
      fold_index++;
      continue;
    }

    append_source_segment(line, cursor, line_end);
    line.end = line_end;
    row = cursor.row + 1;
    break;
  }

  if (cursor.row >= line_count) {
    line.end = Point{line_count - 1,
                     analysis_->line_ends.back() -
                         analysis_->line_starts.back()};
    row = line_count;
  }
  return line;
}

DisplayIndex::UnitCursor
DisplayIndex::begin_cursor(const LogicalLine &line) const {
  UnitCursor cursor{};
  normalize_cursor(line, cursor);
  return cursor;
}

DisplayIndex::UnitCursor
DisplayIndex::end_cursor(const LogicalLine &line) const {
  return UnitCursor{line.segments.size(), 0};
}

bool DisplayIndex::same_cursor(const UnitCursor &left,
                               const UnitCursor &right) const {
  return left.segment == right.segment && left.position == right.position;
}

void DisplayIndex::normalize_cursor(const LogicalLine &line,
                                    UnitCursor &cursor) const {
  while (cursor.segment < line.segments.size()) {
    const LogicalSegment &segment = line.segments[cursor.segment];
    if (segment.kind == DisplaySpan::Kind::Source) {
      if (cursor.position == 0)
        cursor.position = segment.start_offset;
      if (cursor.position < segment.end_offset)
        return;
    } else if (cursor.position == 0) {
      return;
    }
    cursor.segment++;
    cursor.position = 0;
  }
  cursor.position = 0;
}

bool DisplayIndex::next_unit(const SnapshotReader &reader,
                             const LogicalLine &line, UnitCursor &cursor,
                             DisplayUnit &unit) {
  normalize_cursor(line, cursor);
  if (cursor.segment >= line.segments.size())
    return false;

  const LogicalSegment &segment = line.segments[cursor.segment];
  if (segment.kind == DisplaySpan::Kind::FoldPlaceholder) {
    const uint64_t start_offset =
        analysis_offset_for_point(*analysis_, segment.start);
    unit = DisplayUnit{DisplaySpan::Kind::FoldPlaceholder,
                       0,
                       0,
                       segment.start,
                       segment.end,
                       options_.fold_character.front(),
                       segment.start.column == 0
                           ? u'\0'
                           : reader.character_at(start_offset - 1)};
    cursor.position = 1;
  } else {
    const uint64_t offset = cursor.position;
    const uint64_t column =
        segment.start.column + (offset - segment.start_offset);
    const char16_t character = reader.character_at(offset);
    unit = DisplayUnit{DisplaySpan::Kind::Source,
                       offset,
                       offset + 1,
                       Point{segment.start.row, column},
                       Point{segment.start.row, column + 1},
                       character,
                       column == 0 ? u'\0'
                                   : reader.character_at(offset - 1)};
    cursor.position++;
  }
  normalize_cursor(line, cursor);
  layout_units_scanned_++;
  return true;
}

bool DisplayIndex::is_character_pair(char16_t previous,
                                     char16_t character) const {
  return (is_high_surrogate(previous) && is_low_surrogate(character)) ||
         (!is_variation_selector(previous) &&
          is_variation_selector(character)) ||
         (!is_combining_character(previous) &&
          is_combining_character(character));
}

bool DisplayIndex::is_wrap_boundary(char16_t previous,
                                    char16_t character) const {
  if (options_.wrap_boundary_mode == WrapBoundaryMode::None)
    return false;
  const bool word_start =
      (previous == u' ' || previous == u'\t' ||
       (options_.wrap_boundary_mode == WrapBoundaryMode::Standard &&
        (previous == u'-' || previous == u'/'))) &&
      character != u' ' && character != u'\t';
  return word_start ||
         (options_.wrap_boundary_mode == WrapBoundaryMode::Standard &&
          is_cjk_character(character));
}

double DisplayIndex::ratio_for_character(char16_t character) const {
  if (is_korean_character(character))
    return options_.character_widths.korean_ratio;
  if (is_half_width_character(character))
    return options_.character_widths.half_width_ratio;
  if (is_double_width_character(character))
    return options_.character_widths.double_width_ratio;
  return options_.character_widths.default_ratio;
}

uint64_t DisplayIndex::tab_width(uint64_t current_column) const {
  const uint64_t remainder = current_column % options_.tab_length;
  return remainder == 0 ? options_.tab_length
                        : options_.tab_length - remainder;
}

uint64_t DisplayIndex::screen_width(const DisplayUnit &unit,
                                    uint64_t current_column) const {
  return unit.kind == DisplaySpan::Kind::Source && unit.character == u'\t'
             ? tab_width(current_column)
             : 1;
}

double DisplayIndex::layout_width(const DisplayUnit &unit,
                                  uint64_t current_column) const {
  const char16_t measured =
      unit.kind == DisplaySpan::Kind::Source && unit.character == u'\t'
          ? u' '
          : unit.character;
  return ratio_for_character(measured) * screen_width(unit, current_column);
}

uint32_t DisplayIndex::continuation_indent(
    int64_t first_non_whitespace_column) const {
  if (options_.wrap_column == 0)
    return 0;

  uint64_t indent =
      first_non_whitespace_column >= 0 &&
              static_cast<uint64_t>(first_non_whitespace_column) <
                  options_.wrap_column
          ? static_cast<uint64_t>(first_non_whitespace_column)
          : 0;
  if (indent + options_.soft_wrap_hanging_indent < options_.wrap_column)
    indent += options_.soft_wrap_hanging_indent;
  return static_cast<uint32_t>(
      std::min<uint64_t>(indent, std::numeric_limits<uint32_t>::max()));
}

void DisplayIndex::append_screen_row(const SnapshotReader &reader,
                                     const LogicalLine &line,
                                     UnitCursor start, UnitCursor end,
                                     uint32_t leading_indent,
                                     bool starts_in_leading_whitespace,
                                     bool wraps_to_next,
                                     uint32_t next_indent,
                                     uint64_t known_visual_width) {
  ScreenRow row;
  row.leading_indent = leading_indent;
  row.starts_in_leading_whitespace = starts_in_leading_whitespace;
  row.visual_width = leading_indent;
  row.buffer_start = line.start;
  row.buffer_end = line.end;

  // The overwhelmingly common wrapped-line case is one contiguous source
  // segment. The wrapping pass already knows its visual width, so retain one
  // compact span without reading every code unit for a second time.
  if (known_visual_width != UINT64_MAX && line.segments.size() == 1 &&
      line.segments.front().kind == DisplaySpan::Kind::Source) {
    const LogicalSegment &segment = line.segments.front();
    const uint64_t start_offset = start.segment == 0
                                      ? start.position
                                      : segment.end_offset;
    const uint64_t end_offset = end.segment == 0
                                    ? end.position
                                    : segment.end_offset;
    if (start_offset < end_offset) {
      row.buffer_start =
          Point{segment.start.row,
                segment.start.column + (start_offset - segment.start_offset)};
      row.buffer_end =
          Point{segment.start.row,
                segment.start.column + (end_offset - segment.start_offset)};
      row.visual_width = known_visual_width;
      row.spans.push_back(DisplaySpan{
          DisplaySpan::Kind::Source, start_offset, end_offset,
          row.buffer_start, row.buffer_end, leading_indent, known_visual_width});
      row.wraps_to_next = wraps_to_next;
      if (wraps_to_next) {
        row.soft_wrap_indent = next_indent;
        row.wrap_boundary = row.buffer_end;
        row.wrap_predecessor = row.buffer_end;
        row.wrap_predecessor.column--;
      }
      peak_row_spans_ = std::max<uint64_t>(peak_row_spans_, 1);
      rows_.push_back(std::move(row));
      return;
    }
  }

  bool saw_unit = false;
  Point last_start = line.start;
  DisplayUnit unit;
  while (!same_cursor(start, end) && next_unit(reader, line, start, unit)) {
    if (!saw_unit) {
      row.buffer_start = unit.start;
      saw_unit = true;
    }
    row.buffer_end = unit.end;
    last_start = unit.start;
    const uint64_t width = screen_width(unit, row.visual_width);

    if (unit.kind == DisplaySpan::Kind::Source && !row.spans.empty()) {
      DisplaySpan &last = row.spans.back();
      if (last.kind == DisplaySpan::Kind::Source &&
          last.end_offset == unit.start_offset && last.end == unit.start &&
          last.screen_end == row.visual_width) {
        last.end_offset = unit.end_offset;
        last.end = unit.end;
        last.screen_end += width;
      } else {
        row.spans.push_back(DisplaySpan{
            DisplaySpan::Kind::Source, unit.start_offset, unit.end_offset,
            unit.start, unit.end, row.visual_width, row.visual_width + width});
      }
    } else {
      row.spans.push_back(DisplaySpan{
          unit.kind, unit.start_offset, unit.end_offset, unit.start, unit.end,
          row.visual_width, row.visual_width + width});
    }
    row.visual_width += width;
  }

  row.wraps_to_next = wraps_to_next;
  if (wraps_to_next) {
    row.soft_wrap_indent = next_indent;
    row.wrap_boundary = row.buffer_end;
    row.wrap_predecessor = saw_unit ? last_start : row.buffer_start;
  }
  peak_row_spans_ = std::max<uint64_t>(peak_row_spans_, row.spans.size());
  rows_.push_back(std::move(row));
}

void DisplayIndex::wrap_logical_line(const SnapshotReader &reader,
                                     const LogicalLine &line) {
  const UnitCursor logical_start = begin_cursor(line);
  const UnitCursor logical_end = end_cursor(line);
  if (same_cursor(logical_start, logical_end)) {
    append_screen_row(reader, line, logical_start, logical_end, 0, true,
                      false, 0, 0);
    return;
  }
  if (options_.wrap_column == 0) {
    append_screen_row(reader, line, logical_start, logical_end, 0, true,
                      false, 0, UINT64_MAX);
    return;
  }

  UnitCursor row_start = logical_start;
  UnitCursor cursor = logical_start;
  UnitCursor last_boundary{};
  uint64_t last_boundary_screen_column = 0;
  bool has_boundary = false;
  bool saw_non_whitespace = false;
  int64_t first_non_whitespace_screen_column = -1;
  bool builder_in_leading_whitespace = true;
  bool row_starts_in_leading_whitespace = true;
  uint32_t leading_indent = 0;
  uint64_t screen_column = 0;
  double line_width = 0;

  while (!same_cursor(cursor, logical_end)) {
    const UnitCursor before = cursor;
    DisplayUnit unit;
    if (!next_unit(reader, line, cursor, unit))
      break;

    if (!saw_non_whitespace) {
      saw_non_whitespace = unit.character != u' ' && unit.character != u'\t';
      if (saw_non_whitespace)
        first_non_whitespace_screen_column =
            static_cast<int64_t>(screen_column);
    } else if (unit.previous_character != 0 && unit.character != 0 &&
               is_wrap_boundary(unit.previous_character, unit.character)) {
      if (!same_cursor(before, row_start)) {
        last_boundary = before;
        last_boundary_screen_column = screen_column;
        has_boundary = true;
      }
    }

    uint64_t width = screen_width(unit, screen_column);
    double measured_width = layout_width(unit, screen_column);
    const bool should_wrap =
        line_width > 0 && measured_width > 0 &&
        line_width + measured_width > options_.wrap_column &&
        unit.previous_character != 0 && unit.character != 0 &&
        !is_character_pair(unit.previous_character, unit.character);

    if (should_wrap) {
      const uint32_t next_indent =
          continuation_indent(first_non_whitespace_screen_column);
      const UnitCursor split = has_boundary ? last_boundary : before;
      if (!same_cursor(split, row_start)) {
        const uint64_t split_screen_column =
            has_boundary ? last_boundary_screen_column : screen_column;
        append_screen_row(reader, line, row_start, split, leading_indent,
                          row_starts_in_leading_whitespace, true, next_indent,
                          split_screen_column);
        row_start = split;
        row_starts_in_leading_whitespace =
            builder_in_leading_whitespace;
        leading_indent = next_indent;
        screen_column = leading_indent;
        line_width =
            leading_indent * ratio_for_character(static_cast<char16_t>(u' '));

        UnitCursor carried = row_start;
        DisplayUnit carried_unit;
        while (!same_cursor(carried, before) &&
               next_unit(reader, line, carried, carried_unit)) {
          line_width += layout_width(carried_unit, screen_column);
          screen_column += screen_width(carried_unit, screen_column);
        }
        has_boundary = false;
        width = screen_width(unit, screen_column);
        measured_width = layout_width(unit, screen_column);
      }
    }

    screen_column += width;
    line_width += measured_width;
    if (unit.kind == DisplaySpan::Kind::Source && unit.character != u' ' &&
        unit.character != u'\t')
      builder_in_leading_whitespace = false;
  }

  append_screen_row(reader, line, row_start, logical_end, leading_indent,
                    row_starts_in_leading_whitespace, false, 0,
                    screen_column);
}

void DisplayIndex::rebuild(
    const SnapshotReader &reader,
    std::shared_ptr<const SnapshotAnalysis> analysis,
    const std::unordered_map<uint32_t, Fold> &folds) {
  rows_.clear();
  leading_whitespace_ends_.clear();
  trailing_whitespace_starts_.clear();
  analysis_ = std::move(analysis);
  layout_units_scanned_ = 0;
  peak_logical_segments_ = 0;
  peak_row_spans_ = 0;
  if (!analysis_ || analysis_->line_starts.empty())
    return;

  folds_ = normalized_folds(*analysis_, folds);
  leading_whitespace_ends_.resize(analysis_->line_starts.size());
  trailing_whitespace_starts_.resize(analysis_->line_starts.size());
  for (uint64_t buffer_row = 0;
       buffer_row < analysis_->line_starts.size(); buffer_row++) {
    const uint64_t start = analysis_->line_starts[buffer_row];
    const uint64_t end = analysis_->line_ends[buffer_row];
    uint64_t leading = start;
    while (leading < end) {
      const char16_t character = reader.character_at(leading);
      if (character != u' ' && character != u'\t')
        break;
      leading++;
    }
    uint64_t trailing = end;
    while (trailing > start) {
      const char16_t character = reader.character_at(trailing - 1);
      if (character != u' ' && character != u'\t')
        break;
      trailing--;
    }
    leading_whitespace_ends_[buffer_row] = leading - start;
    trailing_whitespace_starts_[buffer_row] = trailing - start;
  }
  uint64_t source_row = 0;
  size_t fold_index = 0;
  while (source_row < analysis_->line_starts.size()) {
    LogicalLine line = build_logical_line(source_row, fold_index);
    peak_logical_segments_ =
        std::max<uint64_t>(peak_logical_segments_, line.segments.size());
    wrap_logical_line(reader, line);
  }
}

uint64_t DisplayIndex::line_length(uint64_t row) const {
  if (!analysis_ || analysis_->line_starts.empty())
    return 0;
  row = std::min<uint64_t>(row, analysis_->line_starts.size() - 1);
  return analysis_->line_ends[row] - analysis_->line_starts[row];
}

uint64_t DisplayIndex::offset_for_point(Point point) const {
  return analysis_ ? analysis_offset_for_point(*analysis_, point) : 0;
}

Point DisplayIndex::clamp_buffer_point(Point point) const {
  return analysis_ ? clamp_point(*analysis_, point) : Point{};
}

char16_t DisplayIndex::character_at_point(const SnapshotReader &reader,
                                          Point point) const {
  point = clamp_buffer_point(point);
  if (point.column >= line_length(point.row))
    return 0;
  return reader.character_at(offset_for_point(point));
}

Point DisplayIndex::clip_buffer_point(const SnapshotReader &reader, Point point,
                                      ClipDirection clip) const {
  point = clamp_buffer_point(point);
  if (point.column > 0 && point.column < line_length(point.row)) {
    const char16_t character = character_at_point(reader, point);
    const char16_t previous = reader.character_at(offset_for_point(point) - 1);
    if (is_character_pair(previous, character)) {
      if (clip == ClipDirection::Forward)
        point.column++;
      else
        point.column--;
      return point;
    }
  }

  if (!options_.atomic_soft_tabs || point.column == 0 ||
      point.column >= line_length(point.row) ||
      character_at_point(reader, point) != u' ')
    return point;
  if (options_.wrap_column > 0 &&
      point.column * ratio_for_character(u' ') > options_.wrap_column)
    return point;

  const uint64_t previous_stop =
      point.column - (point.column % options_.tab_length);
  if (point.column == previous_stop)
    return point;
  const uint64_t next_stop = previous_stop + options_.tab_length;
  if (next_stop > line_length(point.row))
    return point;
  const uint64_t line_start = analysis_->line_starts[point.row];
  for (uint64_t column = 0; column < point.column; column++) {
    if (reader.character_at(line_start + column) != u' ')
      return point;
  }
  for (uint64_t column = point.column + 1; column < next_stop; column++) {
    if (reader.character_at(line_start + column) != u' ')
      return point;
  }

  if (clip == ClipDirection::Forward ||
      (clip == ClipDirection::Closest &&
       point.column - previous_stop > options_.tab_length / 2))
    point.column = next_stop;
  else
    point.column = previous_stop;
  return point;
}

uint64_t DisplayIndex::source_screen_column(const SnapshotReader &reader,
                                            const DisplaySpan &span,
                                            Point point) const {
  uint64_t screen_column = span.screen_start;
  const uint64_t target = std::min<uint64_t>(
      span.end_offset,
      span.start_offset + (point.column - span.start.column));
  for (uint64_t offset = span.start_offset; offset < target; offset++) {
    screen_column += reader.character_at(offset) == u'\t'
                         ? tab_width(screen_column)
                         : 1;
  }
  return screen_column;
}

Point DisplayIndex::screen_for_visible_point(const SnapshotReader &reader,
                                             Point point) const {
  Point candidate{};
  bool found = false;
  for (uint64_t row_index = 0; row_index < rows_.size(); row_index++) {
    const ScreenRow &screen_row = rows_[row_index];
    if (point == screen_row.buffer_start) {
      candidate = Point{row_index, screen_row.leading_indent};
      found = true;
    }
    for (const DisplaySpan &span : screen_row.spans) {
      if (point == span.start) {
        candidate = Point{row_index, span.screen_start};
        found = true;
      }
      if (span.kind == DisplaySpan::Kind::Source && span.start < point &&
          point < span.end && point.row == span.start.row) {
        candidate =
            Point{row_index, source_screen_column(reader, span, point)};
        found = true;
      }
      if (point == span.end) {
        candidate = Point{row_index, span.screen_end};
        found = true;
      }
    }
    if (point == screen_row.buffer_end) {
      candidate = Point{row_index, screen_row.visual_width};
      found = true;
    }
  }
  if (found)
    return candidate;
  if (rows_.empty())
    return Point{};
  return Point{rows_.size() - 1, rows_.back().visual_width};
}

Point DisplayIndex::buffer_to_screen(const SnapshotReader &reader, Point point,
                                     ClipDirection clip) const {
  point = clip_buffer_point(reader, point, clip);
  for (const Fold &fold : folds_) {
    if (fold.range.start < point && point < fold.range.end) {
      bool use_end = false;
      if (clip == ClipDirection::Forward) {
        use_end = true;
      } else if (clip == ClipDirection::Closest) {
        const Point from_start = traversal(point, fold.range.start);
        const Point to_end = traversal(fold.range.end, point);
        if (to_end < from_start)
          use_end = true;
      }
      for (uint64_t row_index = 0; row_index < rows_.size(); row_index++) {
        for (const DisplaySpan &span : rows_[row_index].spans) {
          if (span.kind == DisplaySpan::Kind::FoldPlaceholder &&
              span.start == fold.range.start && span.end == fold.range.end) {
            return Point{row_index,
                         use_end ? span.screen_end : span.screen_start};
          }
        }
      }
      return screen_for_visible_point(
          reader, use_end ? fold.range.end : fold.range.start);
    }
  }
  return screen_for_visible_point(reader, point);
}

Point DisplayIndex::screen_to_buffer(const SnapshotReader &reader, Point point,
                                     ClipDirection clip) const {
  if (rows_.empty())
    return Point{};

  uint64_t row_index = point.row;
  if (row_index >= rows_.size()) {
    row_index = rows_.size() - 1;
    point.column = rows_[row_index].visual_width;
  }
  if (point.column > rows_[row_index].visual_width) {
    if (clip == ClipDirection::Forward && row_index + 1 < rows_.size()) {
      row_index++;
      point.column = 0;
    } else {
      point.column = rows_[row_index].visual_width;
    }
  }

  const ScreenRow &screen_row = rows_[row_index];
  if (screen_row.wraps_to_next &&
      point.column == screen_row.visual_width) {
    const Point result = clip == ClipDirection::Forward
                             ? screen_row.wrap_boundary
                             : screen_row.wrap_predecessor;
    return clip_buffer_point(reader, result, clip);
  }

  if (point.column < screen_row.leading_indent) {
    if (clip == ClipDirection::Backward && row_index > 0)
      return clip_buffer_point(reader, rows_[row_index - 1].wrap_predecessor,
                               clip);
    return clip_buffer_point(reader, screen_row.buffer_start, clip);
  }
  if (point.column == screen_row.leading_indent)
    return clip_buffer_point(reader, screen_row.buffer_start, clip);

  for (const DisplaySpan &span : screen_row.spans) {
    if (point.column == span.screen_start)
      return clip_buffer_point(reader, span.start, clip);
    if (point.column < span.screen_end) {
      if (span.kind == DisplaySpan::Kind::FoldPlaceholder)
        return clip_buffer_point(reader, span.start, clip);
      if (span.kind == DisplaySpan::Kind::HardTab) {
        const uint64_t midpoint =
            (span.screen_start + span.screen_end + 1) / 2;
        const bool use_end =
            clip == ClipDirection::Forward ||
            (clip == ClipDirection::Closest && point.column > midpoint);
        return clip_buffer_point(reader, use_end ? span.end : span.start, clip);
      }

      uint64_t screen_column = span.screen_start;
      for (uint64_t offset = span.start_offset; offset < span.end_offset;
           offset++) {
        const uint64_t width = reader.character_at(offset) == u'\t'
                                   ? tab_width(screen_column)
                                   : 1;
        const uint64_t next_column = screen_column + width;
        const Point source_start{
            span.start.row, span.start.column + (offset - span.start_offset)};
        Point source_end = source_start;
        source_end.column++;
        if (point.column == screen_column)
          return clip_buffer_point(reader, source_start, clip);
        if (point.column < next_column) {
          const uint64_t midpoint = (screen_column + next_column + 1) / 2;
          const bool use_end =
              clip == ClipDirection::Forward ||
              (clip == ClipDirection::Closest && point.column > midpoint);
          if (use_end && screen_row.wraps_to_next &&
              next_column == screen_row.visual_width) {
            return clip_buffer_point(
                reader,
                clip == ClipDirection::Forward ? screen_row.wrap_boundary
                                               : screen_row.wrap_predecessor,
                clip);
          }
          return clip_buffer_point(reader,
                                   use_end ? source_end : source_start, clip);
        }
        if (point.column == next_column)
          return clip_buffer_point(reader, source_end, clip);
        screen_column = next_column;
      }
    }
    if (point.column == span.screen_end)
      return clip_buffer_point(reader, span.end, clip);
  }

  return clip_buffer_point(reader, screen_row.buffer_end, clip);
}

std::u16string DisplayIndex::render_line(const SnapshotReader &reader,
                                         uint64_t screen_row,
                                         uint64_t &copied_utf16) const {
  std::u16string result;
  if (screen_row >= rows_.size())
    return result;
  const ScreenRow &row = rows_[screen_row];
  result.reserve(static_cast<size_t>(std::min<uint64_t>(
      row.visual_width, std::numeric_limits<size_t>::max())));
  result.append(row.leading_indent, u' ');
  copied_utf16 += row.leading_indent;
  for (const DisplaySpan &span : row.spans) {
    if (span.kind == DisplaySpan::Kind::FoldPlaceholder) {
      result.append(options_.fold_character);
      copied_utf16 += options_.fold_character.size();
      continue;
    }
    if (span.kind == DisplaySpan::Kind::HardTab) {
      const uint64_t width = span.screen_end - span.screen_start;
      result.append(static_cast<size_t>(width), u' ');
      copied_utf16 += width;
      continue;
    }

    uint64_t screen_column = span.screen_start;
    uint64_t run_start = span.start_offset;
    for (uint64_t offset = span.start_offset; offset < span.end_offset;
         offset++) {
      if (reader.character_at(offset) != u'\t') {
        screen_column++;
        continue;
      }
      const size_t before = result.size();
      if (reader.append_range(run_start, offset, result))
        copied_utf16 += result.size() - before;
      const uint64_t width = tab_width(screen_column);
      result.append(static_cast<size_t>(width), u' ');
      copied_utf16 += width;
      screen_column += width;
      run_start = offset + 1;
    }
    const size_t before = result.size();
    if (reader.append_range(run_start, span.end_offset, result))
      copied_utf16 += result.size() - before;
  }
  return result;
}

uint64_t DisplayIndex::row_count() const { return rows_.size(); }

const ScreenRow *DisplayIndex::row(uint64_t screen_row) const {
  return screen_row < rows_.size() ? &rows_[screen_row] : nullptr;
}

const DisplayIndexOptions &DisplayIndex::options() const { return options_; }

const std::vector<Fold> &DisplayIndex::folds() const { return folds_; }

uint64_t DisplayIndex::line_length_for_row(uint64_t buffer_row) const {
  return line_length(buffer_row);
}

uint64_t DisplayIndex::line_start_offset(uint64_t buffer_row) const {
  if (!analysis_ || analysis_->line_starts.empty())
    return 0;
  buffer_row =
      std::min<uint64_t>(buffer_row, analysis_->line_starts.size() - 1);
  return analysis_->line_starts[buffer_row];
}

uint64_t DisplayIndex::leading_whitespace_end(uint64_t buffer_row) const {
  return buffer_row < leading_whitespace_ends_.size()
             ? leading_whitespace_ends_[buffer_row]
             : 0;
}

uint64_t DisplayIndex::trailing_whitespace_start(uint64_t buffer_row) const {
  return buffer_row < trailing_whitespace_starts_.size()
             ? trailing_whitespace_starts_[buffer_row]
             : line_length(buffer_row);
}

IndexedDisplaySummary
DisplayIndex::indexed_summary(uint64_t buffer_row_count) const {
  IndexedDisplaySummary result;
  if (!analysis_ || buffer_row_count == 0)
    return result;

  const uint64_t indexed_rows =
      std::min<uint64_t>(buffer_row_count, analysis_->line_starts.size());
  for (uint64_t row = 0; row < rows_.size(); row++) {
    const ScreenRow &candidate = rows_[row];
    if (indexed_rows < analysis_->line_starts.size() &&
        candidate.buffer_start.row >= indexed_rows)
      break;
    result.screen_row_count++;
    if (candidate.visual_width > result.rightmost_screen_position.column)
      result.rightmost_screen_position = Point{row, candidate.visual_width};
  }
  return result;
}

DisplayIndexDiagnostics DisplayIndex::diagnostics() const {
  DisplayIndexDiagnostics result;
  result.screen_row_count = rows_.size();
  result.source_utf16_length = analysis_ ? analysis_->utf16_length : 0;
  result.layout_units_scanned = layout_units_scanned_;
  result.peak_logical_segments = peak_logical_segments_;
  result.peak_row_spans = peak_row_spans_;
  result.retained_bytes = sizeof(*this) +
                          rows_.capacity() * sizeof(ScreenRow) +
                          folds_.capacity() * sizeof(Fold) +
                          leading_whitespace_ends_.capacity() * sizeof(uint64_t) +
                          trailing_whitespace_starts_.capacity() * sizeof(uint64_t) +
                          options_.fold_character.capacity() * sizeof(char16_t);
  for (const ScreenRow &row : rows_) {
    result.display_span_count += row.spans.size();
    result.retained_bytes += row.spans.capacity() * sizeof(DisplaySpan);
  }
  return result;
}

} // namespace document_engine
