#include "revision-projection.h"

#include <algorithm>

namespace document_engine {

namespace {

struct OffsetEdit {
  uint64_t old_start = 0;
  uint64_t old_end = 0;
  uint64_t new_start = 0;
  uint64_t new_end = 0;
};

std::vector<OffsetEdit> offset_edits(const RevisionEditBatch &batch) {
  std::vector<OffsetEdit> result;
  if (!batch.before || !batch.after || batch.edits.size() % 8 != 0)
    return result;
  result.reserve(batch.edits.size() / 8);
  for (size_t index = 0; index < batch.edits.size(); index += 8) {
    result.push_back(OffsetEdit{
        analysis_offset_for_point(
            *batch.before,
            Point{batch.edits[index], batch.edits[index + 1]}),
        analysis_offset_for_point(
            *batch.before,
            Point{batch.edits[index + 2], batch.edits[index + 3]}),
        analysis_offset_for_point(
            *batch.after,
            Point{batch.edits[index + 4], batch.edits[index + 5]}),
        analysis_offset_for_point(
            *batch.after,
            Point{batch.edits[index + 6], batch.edits[index + 7]})});
  }
  std::stable_sort(result.begin(), result.end(),
                   [](const OffsetEdit &left, const OffsetEdit &right) {
                     return left.old_start < right.old_start;
                   });
  return result;
}

uint64_t project_offset(const std::vector<OffsetEdit> &edits, uint64_t offset,
                        ProjectionAffinity affinity) {
  for (auto iterator = edits.rbegin(); iterator != edits.rend(); iterator++) {
    if (iterator->old_end < offset ||
        (iterator->old_end == offset &&
         affinity == ProjectionAffinity::Forward)) {
      return iterator->new_end + (offset - iterator->old_end);
    }
  }
  return offset;
}

bool intersects(const OffsetEdit &edit, uint64_t start, uint64_t end) {
  if (edit.old_start == edit.old_end)
    return start < edit.old_start && edit.old_start < end;
  return start < edit.old_end && edit.old_start < end;
}

} // namespace

uint64_t analysis_offset_for_point(const SnapshotAnalysis &analysis,
                                   Point point) {
  if (analysis.line_starts.empty())
    return 0;
  point.row = std::min<uint64_t>(point.row, analysis.line_starts.size() - 1);
  const uint64_t length =
      analysis.line_ends[point.row] - analysis.line_starts[point.row];
  point.column = std::min<uint64_t>(point.column, length);
  return analysis.line_starts[point.row] + point.column;
}

Point analysis_point_for_offset(const SnapshotAnalysis &analysis,
                                uint64_t offset) {
  if (analysis.line_starts.empty())
    return {};
  offset = std::min<uint64_t>(offset, analysis.utf16_length);
  auto iterator = std::upper_bound(analysis.line_starts.begin(),
                                   analysis.line_starts.end(), offset);
  size_t row = iterator == analysis.line_starts.begin()
                   ? 0
                   : static_cast<size_t>(iterator -
                                         analysis.line_starts.begin() - 1);
  const uint64_t column = std::min<uint64_t>(
      offset - analysis.line_starts[row],
      analysis.line_ends[row] - analysis.line_starts[row]);
  return Point{row, column};
}

bool project_offset_range(const std::vector<RevisionEditBatch> &batches,
                          uint64_t *start, uint64_t *end) {
  if (start == nullptr || end == nullptr || *end < *start)
    return false;
  for (const RevisionEditBatch &batch : batches) {
    const std::vector<OffsetEdit> edits = offset_edits(batch);
    if (!batch.edits.empty() && edits.empty())
      return false;
    for (const OffsetEdit &edit : edits) {
      if (intersects(edit, *start, *end))
        return false;
    }
    *start = project_offset(edits, *start, ProjectionAffinity::Forward);
    *end = project_offset(edits, *end, ProjectionAffinity::Backward);
    if (*end < *start)
      return false;
  }
  return true;
}

} // namespace document_engine
