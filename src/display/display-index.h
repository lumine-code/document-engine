#ifndef LUMINE_DOCUMENT_ENGINE_DISPLAY_INDEX_H_
#define LUMINE_DOCUMENT_ENGINE_DISPLAY_INDEX_H_

#include "core-types.h"
#include "text-bridge/snapshot-reader.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace document_engine {

enum class ClipDirection { Backward, Forward, Closest };
enum class WrapBoundaryMode { None, Word, Standard };

struct CharacterWidthProfile {
  double default_ratio = 1;
  double double_width_ratio = 1;
  double half_width_ratio = 1;
  double korean_ratio = 1;
};

struct DisplayIndexOptions {
  uint32_t wrap_column = 0;
  uint32_t tab_length = 4;
  uint32_t soft_wrap_hanging_indent = 0;
  bool atomic_soft_tabs = true;
  WrapBoundaryMode wrap_boundary_mode = WrapBoundaryMode::Word;
  CharacterWidthProfile character_widths;
  std::u16string fold_character = u"\u22ef";
};

struct DisplaySpan {
  // HardTab remains representable, but compact indexes keep tabs inside Source
  // spans and expand them only while rendering or mapping a viewport row.
  enum class Kind { Source, HardTab, FoldPlaceholder };

  Kind kind = Kind::Source;
  uint64_t start_offset = 0;
  uint64_t end_offset = 0;
  Point start;
  Point end;
  uint64_t screen_start = 0;
  uint64_t screen_end = 0;
};

struct ScreenRow {
  std::vector<DisplaySpan> spans;
  Point buffer_start;
  Point buffer_end;
  uint64_t visual_width = 0;
  uint32_t leading_indent = 0;
  int64_t soft_wrap_indent = -1;
  bool starts_in_leading_whitespace = true;
  // True when wrapping this row can restart without hidden preferred-boundary
  // state carried from the preceding screen row.
  bool is_layout_checkpoint = true;
  bool wraps_to_next = false;
  Point wrap_boundary;
  Point wrap_predecessor;
};

struct DisplayIndexDiagnostics {
  uint64_t screen_row_count = 0;
  uint64_t display_span_count = 0;
  uint64_t retained_bytes = 0;
  uint64_t source_utf16_length = 0;
  uint64_t layout_units_scanned = 0;
  uint64_t peak_logical_segments = 0;
  uint64_t peak_row_spans = 0;
};

struct IndexedDisplaySummary {
  uint64_t screen_row_count = 0;
  Point rightmost_screen_position;
};

struct DisplayIndexUpdateDiagnostics {
  uint64_t screen_rows_rebuilt = 0;
  uint64_t screen_rows_reused = 0;
  uint64_t layout_units_scanned = 0;
  uint64_t replaced_screen_row_start = 0;
  uint64_t replaced_screen_row_count = 0;
  bool updated_in_place = false;
};

class DisplayIndex {
public:
  explicit DisplayIndex(DisplayIndexOptions options);

  bool rebuild(const SnapshotReader &reader,
               std::shared_ptr<const SnapshotAnalysis> analysis,
               const std::unordered_map<uint32_t, Fold> &folds);
  bool update(const SnapshotReader &reader,
              std::shared_ptr<const SnapshotAnalysis> analysis,
              const std::unordered_map<uint32_t, Fold> &folds,
              const std::vector<RevisionEditBatch> &edits,
              DisplayIndexUpdateDiagnostics *diagnostics = nullptr);

  Point buffer_to_screen(const SnapshotReader &reader, Point point,
                         ClipDirection clip) const;
  Point screen_to_buffer(const SnapshotReader &reader, Point point,
                         ClipDirection clip) const;
  std::u16string render_line(const SnapshotReader &reader, uint64_t screen_row,
                             uint64_t &copied_utf16) const;

  uint64_t row_count() const;
  const ScreenRow *row(uint64_t screen_row) const;
  const DisplayIndexOptions &options() const;
  const std::vector<Fold> &folds() const;
  uint64_t line_length_for_row(uint64_t buffer_row) const;
  uint64_t line_start_offset(uint64_t buffer_row) const;
  uint64_t leading_whitespace_end(uint64_t buffer_row) const;
  uint64_t trailing_whitespace_start(uint64_t buffer_row) const;
  IndexedDisplaySummary indexed_summary(uint64_t buffer_row_count) const;
  DisplayIndexDiagnostics diagnostics() const;

private:
  struct LogicalSegment {
    DisplaySpan::Kind kind = DisplaySpan::Kind::Source;
    uint64_t start_offset = 0;
    uint64_t end_offset = 0;
    Point start;
    Point end;
  };

  struct LogicalLine {
    std::vector<LogicalSegment> segments;
    Point start;
    Point end;
  };

  struct UnitCursor {
    size_t segment = 0;
    uint64_t position = 0;
  };

  struct DisplayUnit {
    DisplaySpan::Kind kind = DisplaySpan::Kind::Source;
    uint64_t start_offset = 0;
    uint64_t end_offset = 0;
    Point start;
    Point end;
    char16_t character = 0;
    char16_t previous_character = 0;
  };

  static std::vector<Fold>
  normalized_folds(const SnapshotAnalysis &analysis,
                   const std::unordered_map<uint32_t, Fold> &folds);
  void rebuild_in_place(const SnapshotReader &reader,
                        std::shared_ptr<const SnapshotAnalysis> analysis,
                        const std::unordered_map<uint32_t, Fold> &folds);
  void adopt_state(DisplayIndex &&replacement);
  LogicalLine build_logical_line(uint64_t &row, size_t &fold_index) const;
  void append_source_segment(LogicalLine &line, Point start, Point end) const;

  UnitCursor begin_cursor(const LogicalLine &line) const;
  UnitCursor end_cursor(const LogicalLine &line) const;
  bool same_cursor(const UnitCursor &left, const UnitCursor &right) const;
  void normalize_cursor(const LogicalLine &line, UnitCursor &cursor) const;
  bool next_unit(const SnapshotReader &reader, const LogicalLine &line,
                 UnitCursor &cursor, DisplayUnit &unit);
  void wrap_logical_line(const SnapshotReader &reader,
                         const LogicalLine &line);
  void wrap_logical_line_from(const SnapshotReader &reader,
                              const LogicalLine &line,
                              UnitCursor logical_start,
                              uint32_t leading_indent,
                              bool starts_in_leading_whitespace,
                              bool continuation_indent_is_fixed,
                              uint32_t fixed_continuation_indent);
  void append_screen_row(const SnapshotReader &reader,
                         const LogicalLine &line, UnitCursor start,
                         UnitCursor end, uint32_t leading_indent,
                         bool starts_in_leading_whitespace,
                         bool is_layout_checkpoint,
                         bool wraps_to_next, uint32_t next_indent,
                         uint64_t known_visual_width);

  Point clamp_buffer_point(Point point) const;
  Point clip_buffer_point(const SnapshotReader &reader, Point point,
                          ClipDirection clip) const;
  uint64_t candidate_screen_row(Point point) const;
  Point screen_for_visible_point(const SnapshotReader &reader,
                                 Point point) const;
  uint64_t source_screen_column(const SnapshotReader &reader,
                                const DisplaySpan &span, Point point) const;
  uint64_t offset_for_point(Point point) const;
  uint64_t line_length(uint64_t row) const;
  char16_t character_at_point(const SnapshotReader &reader, Point point) const;
  bool is_character_pair(char16_t previous, char16_t character) const;
  bool is_wrap_boundary(char16_t previous, char16_t character) const;
  double ratio_for_character(char16_t character) const;
  uint64_t screen_width(const DisplayUnit &unit,
                        uint64_t current_column) const;
  double layout_width(const DisplayUnit &unit, uint64_t current_column) const;
  uint32_t continuation_indent(int64_t first_non_whitespace_column) const;
  uint64_t tab_width(uint64_t current_column) const;

  DisplayIndexOptions options_;
  std::shared_ptr<const SnapshotAnalysis> analysis_;
  std::vector<Fold> folds_;
  std::vector<ScreenRow> rows_;
  std::vector<uint64_t> leading_whitespace_ends_;
  std::vector<uint64_t> trailing_whitespace_starts_;
  uint64_t layout_units_scanned_ = 0;
  uint64_t peak_logical_segments_ = 0;
  uint64_t peak_row_spans_ = 0;
};

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_DISPLAY_INDEX_H_
