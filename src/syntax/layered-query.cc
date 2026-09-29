#include "syntax/layered-query.h"

#include "syntax/query-snapshot-cache.h"
#include "syntax/syntax-backend.h"
#include "revision-projection.h"
#include "text-bridge/snapshot-reader.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_map>

namespace document_engine {

namespace {

struct IndexedRange {
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t start_row = 0;
  uint32_t start_column = 0;
  uint32_t end_row = 0;
  uint32_t end_column = 0;
  std::vector<std::string> scopes;
};

struct Source {
  uint64_t layer_id = 0;
  uint64_t parent_layer_id = 0;
  uint32_t depth = 0;
  std::string grammar_id;
  std::shared_ptr<const PublishedSyntaxSnapshot> syntax;
  std::vector<IndexedRange> ranges;
  bool cover_shallower_scopes = false;
};

uint32_t point_to_index(const SnapshotAnalysis &analysis, Point point) {
  if (analysis.line_starts.empty())
    return 0;
  if (point.row >= analysis.line_starts.size())
    return static_cast<uint32_t>(std::min<uint64_t>(
        analysis.utf16_length, std::numeric_limits<uint32_t>::max()));
  const uint64_t row =
      std::min<uint64_t>(point.row, analysis.line_starts.size() - 1);
  const uint64_t line_start = analysis.line_starts[row];
  const uint64_t line_end = analysis.line_ends[row];
  return static_cast<uint32_t>(std::min<uint64_t>(
      line_start + std::min<uint64_t>(point.column, line_end - line_start),
      std::numeric_limits<uint32_t>::max()));
}

Point index_to_point(const SnapshotAnalysis &analysis, uint32_t raw_index) {
  if (analysis.line_starts.empty())
    return Point{};
  const uint64_t index =
      std::min<uint64_t>(raw_index, analysis.utf16_length);
  auto iterator = std::upper_bound(analysis.line_starts.begin(),
                                   analysis.line_starts.end(), index);
  const size_t row = iterator == analysis.line_starts.begin()
                         ? 0
                         : static_cast<size_t>(
                               iterator - analysis.line_starts.begin() - 1);
  return Point{
      row,
      std::min<uint64_t>(index - analysis.line_starts[row],
                         analysis.line_ends[row] - analysis.line_starts[row])};
}

IndexedRange indexed_range(const SnapshotAnalysis &analysis,
                           const Range &range,
                           std::vector<std::string> scopes = {}) {
  const uint32_t start = point_to_index(analysis, range.start);
  const uint32_t end = point_to_index(analysis, range.end);
  const Point clipped_start = index_to_point(analysis, start);
  const Point clipped_end = index_to_point(analysis, end);
  return IndexedRange{
      start, end, static_cast<uint32_t>(clipped_start.row),
      static_cast<uint32_t>(clipped_start.column),
      static_cast<uint32_t>(clipped_end.row),
      static_cast<uint32_t>(clipped_end.column), std::move(scopes)};
}

IndexedRange intersect(const IndexedRange &left, const IndexedRange &right,
                       const SnapshotAnalysis &analysis) {
  const uint32_t start = std::max(left.start, right.start);
  const uint32_t end = std::min(left.end, right.end);
  const Point start_point = index_to_point(analysis, start);
  const Point end_point = index_to_point(analysis, end);
  return IndexedRange{
      start, end, static_cast<uint32_t>(start_point.row),
      static_cast<uint32_t>(start_point.column),
      static_cast<uint32_t>(end_point.row),
      static_cast<uint32_t>(end_point.column), left.scopes};
}

bool overlaps(const IndexedRange &left, const IndexedRange &right) {
  return left.start < right.end && right.start < left.end;
}

QueryResolutionContext resolution_for(const LayeredQueryContext &context,
                                      const Source &source) {
  QueryResolutionContext result = context.defaults;
  const auto found = context.by_grammar.find(source.grammar_id);
  if (found != context.by_grammar.end())
    result = found->second;
  result.injection_depth =
      std::max(result.injection_depth, source.depth);
  return result;
}

uint32_t name_id(std::vector<std::string> &names,
                 std::unordered_map<std::string, uint32_t> &ids,
                 const std::string &name) {
  const auto found = ids.find(name);
  if (found != ids.end())
    return found->second;
  const uint32_t id = static_cast<uint32_t>(names.size());
  names.push_back(name);
  ids.emplace(name, id);
  return id;
}

QueryCaptureRecord clipped_capture(const QueryCaptureRecord &capture,
                                   uint32_t start, uint32_t end,
                                   const SnapshotAnalysis &analysis) {
  QueryCaptureRecord result = capture;
  const Point start_point = index_to_point(analysis, start);
  const Point end_point = index_to_point(analysis, end);
  result.start_index = start;
  result.end_index = end;
  result.start_row = static_cast<uint32_t>(start_point.row);
  result.start_column = static_cast<uint32_t>(start_point.column);
  result.end_row = static_cast<uint32_t>(end_point.row);
  result.end_column = static_cast<uint32_t>(end_point.column);
  return result;
}

std::vector<std::pair<uint32_t, uint32_t>> subtract_ranges(
    uint32_t start, uint32_t end,
    const std::vector<std::pair<uint32_t, uint32_t>> &covers) {
  std::vector<std::pair<uint32_t, uint32_t>> segments{{start, end}};
  for (const auto &[cover_start, cover_end] : covers) {
    std::vector<std::pair<uint32_t, uint32_t>> next;
    for (const auto &[segment_start, segment_end] : segments) {
      if (cover_end <= segment_start || cover_start >= segment_end) {
        next.emplace_back(segment_start, segment_end);
        continue;
      }
      if (segment_start < cover_start)
        next.emplace_back(segment_start, std::min(segment_end, cover_start));
      if (cover_end < segment_end)
        next.emplace_back(std::max(segment_start, cover_end), segment_end);
    }
    segments = std::move(next);
    if (segments.empty())
      break;
  }
  return segments;
}

} // namespace

bool execute_layered_query(
    const std::string &query_type,
    std::shared_ptr<const PublishedSyntaxSnapshot> root,
    const std::vector<InjectionQuerySource> &injections,
    const SnapshotReader &reader,
    const SnapshotAnalysis &syntax_analysis,
    const SnapshotAnalysis &output_analysis,
    const std::vector<RevisionEditBatch> &projection,
    uint64_t buffer_revision, uint64_t language_generation,
    const QueryRange &range, const LayeredQueryContext &context,
    QuerySnapshotCache *cache,
    QueryCancellationFunction cancellation,
    void *cancellation_payload,
    std::shared_ptr<const QueryIndexSnapshot> &snapshot,
    QueryErrorInfo &error, QueryRunStatistics &statistics) {
  error = QueryErrorInfo{};
  auto output = std::make_shared<QueryIndexSnapshot>();
  output->query_type = query_type;
  output->buffer_revision = buffer_revision;
  output->language_generation = language_generation;
  if (!root) {
    snapshot = std::move(output);
    return true;
  }

  const Point requested_start{range.start_row, range.start_column};
  const Point requested_end{range.end_row, range.end_column};
  const IndexedRange requested = indexed_range(
      output_analysis, Range{requested_start, requested_end});
  const bool projects_revision = !projection.empty();
  const QueryRange syntax_query_range = projects_revision ? QueryRange{} : range;
  const Point full_syntax_end =
      analysis_point_for_offset(syntax_analysis,
                                syntax_analysis.utf16_length);
  const IndexedRange full_syntax_range = indexed_range(
      syntax_analysis, Range{Point{}, full_syntax_end});
  IndexedRange syntax_requested;
  if (projects_revision) {
    syntax_requested = full_syntax_range;
  } else {
    syntax_requested = indexed_range(
        syntax_analysis, Range{requested_start, requested_end});
  }
  Source root_source;
  root_source.grammar_id = context.root_grammar_id;
  root_source.syntax = std::move(root);
  IndexedRange root_range = full_syntax_range;
  if (!root_source.grammar_id.empty())
    root_range.scopes.push_back(root_source.grammar_id);
  root_source.ranges.push_back(std::move(root_range));
  std::vector<Source> sources;
  sources.push_back(std::move(root_source));
  for (const InjectionQuerySource &injection : injections) {
    Source source;
    source.layer_id = injection.layer_id;
    source.parent_layer_id = injection.parent_layer_id;
    source.depth = injection.depth;
    source.grammar_id = injection.grammar_id;
    source.syntax = injection.syntax;
    source.cover_shallower_scopes = injection.cover_shallower_scopes;
    for (const InjectionQuerySource::IncludedRange &included :
         injection.included_ranges) {
      IndexedRange candidate =
          indexed_range(syntax_analysis, included.range, included.scopes);
      if (!projects_revision && !overlaps(candidate, syntax_requested))
        continue;
      source.ranges.push_back(std::move(candidate));
    }
    if (source.syntax && !source.ranges.empty())
      sources.push_back(std::move(source));
  }
  std::sort(sources.begin() + 1, sources.end(),
            [](const Source &left, const Source &right) {
              return std::tie(left.depth, left.layer_id) <
                     std::tie(right.depth, right.layer_id);
            });
  std::vector<Source> hierarchy;
  hierarchy.push_back(std::move(sources.front()));
  for (size_t source_index = 1; source_index < sources.size(); source_index++) {
    Source source = std::move(sources[source_index]);
    const auto parent = std::find_if(
        hierarchy.begin(), hierarchy.end(), [&](const Source &candidate) {
          return candidate.layer_id == source.parent_layer_id;
        });
    if (parent == hierarchy.end())
      continue;
    std::vector<IndexedRange> clipped;
    for (const IndexedRange &child_range : source.ranges) {
      for (const IndexedRange &parent_range : parent->ranges) {
        if (overlaps(child_range, parent_range))
          clipped.push_back(
              intersect(child_range, parent_range, syntax_analysis));
      }
    }
    source.ranges = std::move(clipped);
    if (!source.ranges.empty())
      hierarchy.push_back(std::move(source));
  }
  sources = std::move(hierarchy);

  std::unordered_map<std::string, uint32_t> name_ids;
  uint32_t original_order = 0;
  for (uint32_t layer_index = 0; layer_index < sources.size(); layer_index++) {
    const Source &source = sources[layer_index];
    std::shared_ptr<const QueryIndexSnapshot> index;
    const QueryResolutionContext resolution = resolution_for(context, source);
    const uint64_t source_identity = source.syntax->identity();
    const uint64_t source_revision = source.syntax->buffer_revision();
    if (cache != nullptr) {
      index = cache->get(source_revision, language_generation, source_identity,
                         query_type, syntax_query_range, resolution);
    }
    if (!index) {
      const std::shared_ptr<const NativeQueryEngine> engine =
          source.syntax->query_engine();
      if (!engine || !engine->execute_one(
                         query_type, source.syntax->tree(), reader,
                         syntax_analysis,
                         source_revision,
                         source.syntax->language_generation(),
                         syntax_query_range, resolution, cancellation,
                         cancellation_payload,
                         index, error,
                         statistics))
        return false;
      if (cache != nullptr)
        cache->put(source_revision, language_generation, source_identity,
                   query_type, syntax_query_range, resolution, index);
    }
    output->raw_capture_count += index->raw_capture_count;
    output->exceeded_match_limit |= index->exceeded_match_limit;
    output->resolution_complete &= index->resolution_complete;
    output->scope_statistics.tested += index->scope_statistics.tested;
    output->scope_statistics.accepted += index->scope_statistics.accepted;
    output->scope_statistics.rejected += index->scope_statistics.rejected;
    output->scope_statistics.adjusted += index->scope_statistics.adjusted;
    output->scope_statistics.invalid_adjustments +=
        index->scope_statistics.invalid_adjustments;
    output->scope_statistics.local_predicates +=
        index->scope_statistics.local_predicates;
    const uint32_t property_offset =
        static_cast<uint32_t>(output->patterns.size());
    output->patterns.insert(output->patterns.end(), index->patterns.begin(),
                            index->patterns.end());
    for (const QueryCaptureRecord &raw_capture : index->captures) {
      if (raw_capture.name_id >= index->capture_names.size())
        continue;
      QueryCaptureRecord capture = raw_capture;
      capture.name_id = name_id(output->capture_names, name_ids,
                                index->capture_names[raw_capture.name_id]);
      capture.property_set_id += property_offset;
      capture.layer_index = layer_index;
      capture.depth = source.depth;
      // Reserve zero for the language-scope envelope. RenderPlan sorts by
      // depth and then this order, so the grammar scope must wrap every query
      // scope that starts at the same position.
      capture.order = 1 + original_order++;
      if (context.include_node_handles) {
        capture.node_handle = source.syntax->handle_for_range(
            capture.node_start_index, capture.node_end_index,
            capture.node_symbol);
      }
      for (uint32_t range_index = 0; range_index < source.ranges.size();
           range_index++) {
        const IndexedRange &included = source.ranges[range_index];
        const uint32_t clipped_start =
            std::max(capture.start_index, included.start);
        const uint32_t clipped_end = std::min(capture.end_index, included.end);
        if (clipped_start < clipped_end ||
            (capture.start_index == capture.end_index &&
             clipped_start == clipped_end &&
             clipped_start >= included.start &&
             clipped_start <= included.end)) {
          QueryCaptureRecord clipped = clipped_capture(
              capture, clipped_start, clipped_end, syntax_analysis);
          clipped.layer_range_index = range_index;
          output->captures.push_back(std::move(clipped));
        }
      }
    }

  }

  std::vector<std::vector<IndexedRange>> current_ranges(sources.size());
  std::vector<std::vector<bool>> range_valid(sources.size());
  for (uint32_t layer_index = 0; layer_index < sources.size(); layer_index++) {
    const Source &source = sources[layer_index];
    range_valid[layer_index].resize(source.ranges.size(), false);
    if (layer_index == 0) {
      IndexedRange current_root = requested;
      if (!source.ranges.empty())
        current_root.scopes = source.ranges.front().scopes;
      current_ranges[layer_index].push_back(std::move(current_root));
      range_valid[layer_index][0] = true;
      continue;
    }
    for (uint32_t range_index = 0; range_index < source.ranges.size();
         range_index++) {
      const IndexedRange &source_range = source.ranges[range_index];
      uint64_t start = source_range.start;
      uint64_t end = source_range.end;
      if (projects_revision &&
          !project_offset_range(projection, &start, &end))
        continue;
      if (start > std::numeric_limits<uint32_t>::max() ||
          end > std::numeric_limits<uint32_t>::max())
        continue;
      IndexedRange projected = indexed_range(
          output_analysis,
          Range{analysis_point_for_offset(output_analysis, start),
                analysis_point_for_offset(output_analysis, end)},
          source_range.scopes);
      range_valid[layer_index][range_index] = true;
      if (overlaps(projected, requested))
        current_ranges[layer_index].push_back(
            intersect(projected, requested, output_analysis));
    }
  }

  std::vector<QueryCaptureRecord> projected_captures;
  projected_captures.reserve(output->captures.size());
  for (QueryCaptureRecord capture : output->captures) {
    if (capture.layer_index >= range_valid.size() ||
        capture.layer_range_index >=
            range_valid[capture.layer_index].size() ||
        !range_valid[capture.layer_index][capture.layer_range_index]) {
      output->stale_capture_count++;
      continue;
    }
    uint64_t start = capture.start_index;
    uint64_t end = capture.end_index;
    if (projects_revision &&
        !project_offset_range(projection, &start, &end)) {
      output->stale_capture_count++;
      continue;
    }
    if (start > std::numeric_limits<uint32_t>::max() ||
        end > std::numeric_limits<uint32_t>::max())
      continue;
    capture = clipped_capture(capture, static_cast<uint32_t>(start),
                              static_cast<uint32_t>(end), output_analysis);
    // Only stale projected syntax needs a final-range filter. For current
    // syntax, Tree-sitter has already selected matches by the pattern-root
    // range. Scope adjustments may move a retained capture outside the
    // requested columns; legacy web-tree-sitter still returns it, and captures
    // whose roots were outside the range must not participate in stateful
    // final/shy/rangeWithData resolution.
    if (projects_revision) {
      const bool intersects_request =
          capture.start_index < requested.end &&
          requested.start < capture.end_index;
      const bool empty_inside =
          capture.start_index == capture.end_index &&
          capture.start_index >= requested.start &&
          capture.start_index <= requested.end;
      if (!intersects_request && !empty_inside)
        continue;
      output->projected_capture_count++;
    }
    projected_captures.push_back(std::move(capture));
  }
  output->captures = std::move(projected_captures);

  output->layers.clear();
  for (uint32_t layer_index = 0; layer_index < sources.size(); layer_index++) {
    const Source &source = sources[layer_index];
    QueryLayerRecord layer;
    layer.layer_id = source.layer_id;
    layer.parent_layer_id = source.parent_layer_id;
    layer.depth = source.depth;
    layer.grammar_id = source.grammar_id;
    layer.cover_shallower_scopes = source.cover_shallower_scopes;
    for (const IndexedRange &range : current_ranges[layer_index]) {
      layer.ranges.push_back(QueryLayerRangeRecord{
          range.start_row, range.start_column, range.end_row,
          range.end_column, range.start, range.end, range.scopes});
    }
    output->layers.push_back(std::move(layer));
  }

  if (query_type == "highlightsQuery") {
    std::vector<QueryCaptureRecord> visible;
    for (const QueryCaptureRecord &capture : output->captures) {
      if (capture.start_index == capture.end_index) {
        visible.push_back(capture);
        continue;
      }
      std::vector<std::pair<uint32_t, uint32_t>> covers;
      for (uint32_t layer_index = 0; layer_index < sources.size();
           layer_index++) {
        const Source &source = sources[layer_index];
        if (!source.cover_shallower_scopes || source.depth <= capture.depth)
          continue;
        for (const IndexedRange &range : current_ranges[layer_index]) {
          if (range.start < capture.end_index &&
              capture.start_index < range.end)
            covers.emplace_back(range.start, range.end);
        }
      }
      std::sort(covers.begin(), covers.end());
      if (!covers.empty())
        output->covered_capture_count++;
      for (const auto &[start, end] : subtract_ranges(
               capture.start_index, capture.end_index, covers)) {
        if (start < end)
          visible.push_back(
              clipped_capture(capture, start, end, output_analysis));
      }
    }
    output->captures = std::move(visible);

    if (context.include_language_scopes) {
      for (uint32_t layer_index = 0; layer_index < sources.size();
           layer_index++) {
        const Source &source = sources[layer_index];
        for (const IndexedRange &included : current_ranges[layer_index]) {
          for (const std::string &scope : included.scopes) {
            if (scope.empty() || included.start >= included.end)
              continue;
            QueryCaptureRecord capture;
            capture.name_id =
                name_id(output->capture_names, name_ids, scope);
            capture.pattern_index = std::numeric_limits<uint32_t>::max();
            capture.property_set_id = std::numeric_limits<uint32_t>::max();
            capture.start_row = included.start_row;
            capture.start_column = included.start_column;
            capture.end_row = included.end_row;
            capture.end_column = included.end_column;
            capture.start_index = included.start;
            capture.end_index = included.end;
            capture.layer_index = layer_index;
            capture.depth = source.depth;
            capture.order = 0;
            capture.flags = QUERY_CAPTURE_LANGUAGE_SCOPE;
            output->captures.push_back(std::move(capture));
          }
        }
      }
    }
  }

  // Preserve each query cursor's capture order for the public API. Range
  // consumers such as RenderPlan sort their packed highlight projection by
  // coordinates separately; sorting adjusted captures here changes the order
  // observed from web-tree-sitter whenever an adjustment moves a boundary.
  output->accepted_capture_count = output->captures.size();
  snapshot = std::move(output);
  return true;
}

} // namespace document_engine
