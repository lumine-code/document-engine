#ifndef LUMINE_DOCUMENT_ENGINE_REVISION_PROJECTION_H_
#define LUMINE_DOCUMENT_ENGINE_REVISION_PROJECTION_H_

#include "core-types.h"

#include <cstdint>
#include <vector>

namespace document_engine {

enum class ProjectionAffinity { Backward, Forward };

uint64_t analysis_offset_for_point(const SnapshotAnalysis &analysis,
                                   Point point);
Point analysis_point_for_offset(const SnapshotAnalysis &analysis,
                                uint64_t offset);

// Projects a half-open range through accepted revision batches. Returns false
// when any edit intersects the range; callers must drop stale derived state in
// that case. Boundary insertions use marker-style affinities: starts move
// forward and ends stay backward.
bool project_offset_range(const std::vector<RevisionEditBatch> &batches,
                          uint64_t *start, uint64_t *end);

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_REVISION_PROJECTION_H_
