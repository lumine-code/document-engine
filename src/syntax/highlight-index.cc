#include "syntax/highlight-index.h"

#include <algorithm>

namespace document_engine {

uint32_t highlight_shard_start(uint32_t row) {
  return row - row % HIGHLIGHT_SHARD_ROWS;
}

uint32_t highlight_shard_end(uint32_t start_row) {
  return start_row > UINT32_MAX - HIGHLIGHT_SHARD_ROWS
             ? UINT32_MAX
             : start_row + HIGHLIGHT_SHARD_ROWS;
}

void NativeHighlightIndex::clear_shards() {
  shards_.clear();
  retained_bytes_ = 0;
}

void NativeHighlightIndex::invalidate() {
  clear_shards();
  scope_ids_.clear();
  scope_id_bytes_ = 0;
  cohort_shards_.clear();
  tags_initialized_ = false;
  generation_++;
}

void NativeHighlightIndex::activate(const HighlightIndexTags &tags) {
  if (tags_initialized_ && tags_ == tags)
    return;
  clear_shards();
  scope_ids_.clear();
  scope_id_bytes_ = 0;
  cohort_shards_.clear();
  tags_ = tags;
  tags_initialized_ = true;
  generation_++;
}

void NativeHighlightIndex::prepare_request(
    const HighlightIndexTags &tags,
    const std::vector<uint32_t> &requested_shards, bool exclusive_cohort) {
  activate(tags);
  std::vector<uint32_t> desired;
  if (exclusive_cohort) {
    desired.reserve(requested_shards.size());
    for (uint32_t start : requested_shards)
      desired.push_back(highlight_shard_start(start));
    std::sort(desired.begin(), desired.end());
    desired.erase(std::unique(desired.begin(), desired.end()), desired.end());
  }
  if (cohort_shards_ == desired)
    return;
  clear_shards();
  cohort_shards_ = std::move(desired);
  generation_++;
}

const HighlightIndexTags &NativeHighlightIndex::tags() const { return tags_; }

bool NativeHighlightIndex::active_for(const HighlightIndexTags &tags) const {
  return tags_initialized_ && tags_ == tags;
}

uint64_t NativeHighlightIndex::generation() const { return generation_; }

bool NativeHighlightIndex::has_shard(uint32_t start_row) const {
  return shards_.find(start_row) != shards_.end();
}

void NativeHighlightIndex::append_shard_ranges(
    const Shard &shard, std::set<RangeIdentityKey> &seen,
    std::vector<ScopedRange> &ranges) const {
  using BaseIdentity =
      std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint64_t, uint32_t,
                 uint32_t, uint32_t>;
  std::map<BaseIdentity, uint32_t> occurrences;
  for (const IndexedRange &indexed : shard.ranges) {
    const ScopedRange &range = indexed.range;
    const BaseIdentity base =
        std::make_tuple(range.scope_id, range.start_offset, range.end_offset,
                        range.depth, indexed.layer_id, indexed.pattern_id,
                        indexed.node_start_offset, indexed.node_end_offset);
    const uint32_t occurrence = occurrences[base]++;
    const RangeIdentityKey identity =
        std::make_tuple(range.scope_id, range.start_offset, range.end_offset,
                        range.depth, indexed.layer_id, indexed.pattern_id,
                        indexed.node_start_offset, indexed.node_end_offset,
                        occurrence);
    if (seen.insert(identity).second)
      ranges.push_back(range);
  }
}

std::vector<uint32_t> NativeHighlightIndex::missing_shard_starts(
    uint32_t start_row, uint32_t end_row,
    const HighlightIndexTags &tags) const {
  std::vector<uint32_t> result;
  if (!active_for(tags) || end_row <= start_row)
    return result;
  uint32_t shard = highlight_shard_start(start_row);
  while (shard < end_row) {
    if (!has_shard(shard))
      result.push_back(shard);
    const uint32_t next = highlight_shard_end(shard);
    if (next <= shard)
      break;
    shard = next;
  }
  return result;
}

bool NativeHighlightIndex::covers(uint32_t start_row, uint32_t end_row,
                                  const HighlightIndexTags &tags) {
  if (!active_for(tags) || end_row <= start_row)
    return false;
  uint32_t shard = highlight_shard_start(start_row);
  while (shard < end_row) {
    auto found = shards_.find(shard);
    if (found == shards_.end())
      return false;
    found->second.last_used = ++clock_;
    const uint32_t next = highlight_shard_end(shard);
    if (next <= shard)
      break;
    shard = next;
  }
  return true;
}

bool NativeHighlightIndex::covers_shards(
    const std::vector<uint32_t> &shard_starts,
    const HighlightIndexTags &tags) {
  if (!active_for(tags) || shard_starts.empty())
    return false;
  std::vector<uint32_t> normalized;
  normalized.reserve(shard_starts.size());
  for (uint32_t raw_start : shard_starts)
    normalized.push_back(highlight_shard_start(raw_start));
  std::sort(normalized.begin(), normalized.end());
  normalized.erase(std::unique(normalized.begin(), normalized.end()),
                   normalized.end());
  if (!cohort_shards_.empty() && cohort_shards_ != normalized)
    return false;
  for (uint32_t start : normalized) {
    auto found = shards_.find(start);
    if (found == shards_.end())
      return false;
    found->second.last_used = ++clock_;
  }
  return true;
}

bool NativeHighlightIndex::collect(uint32_t start_row, uint32_t end_row,
                                   const HighlightIndexTags &tags,
                                   std::vector<ScopedRange> &ranges) {
  ranges.clear();
  if (!covers(start_row, end_row, tags))
    return false;
  std::set<RangeIdentityKey> seen;
  uint32_t shard = highlight_shard_start(start_row);
  while (shard < end_row) {
    const auto found = shards_.find(shard);
    if (found == shards_.end()) {
      ranges.clear();
      return false;
    }
    append_shard_ranges(found->second, seen, ranges);
    const uint32_t next = highlight_shard_end(shard);
    if (next <= shard)
      break;
    shard = next;
  }
  return true;
}

bool NativeHighlightIndex::collect_shards(
    const std::vector<uint32_t> &shard_starts,
    const HighlightIndexTags &tags, std::vector<ScopedRange> &ranges) {
  ranges.clear();
  if (!covers_shards(shard_starts, tags))
    return false;
  std::set<RangeIdentityKey> seen;
  for (uint32_t raw_start : shard_starts) {
    const uint32_t start = highlight_shard_start(raw_start);
    auto found = shards_.find(start);
    if (found == shards_.end()) {
      ranges.clear();
      return false;
    }
    append_shard_ranges(found->second, seen, ranges);
  }
  return true;
}

bool NativeHighlightIndex::commit(
    const HighlightCandidateBatch &candidate,
    const std::vector<uint32_t> &scope_ids) {
  if (!active_for(candidate.tags) || candidate.shards.empty() ||
      candidate.requested_shards.empty() ||
      scope_ids.size() != candidate.capture_names.size() ||
      candidate.capture_grammar_ids.size() != candidate.capture_names.size())
    return false;

  auto next_scope_ids = scope_ids_;
  for (size_t index = 0; index < candidate.capture_names.size(); index++) {
    const uint32_t scope_id = scope_ids[index];
    if (scope_id > 0 && scope_id <= static_cast<uint32_t>(INT32_MAX))
      next_scope_ids[{candidate.capture_grammar_ids[index],
                      candidate.capture_names[index]}] = scope_id;
  }

  size_t next_scope_id_bytes = 0;
  for (const auto &[key, _] : next_scope_ids) {
    next_scope_id_bytes += sizeof(key) + sizeof(uint32_t) +
                           key.first.capacity() + key.second.capacity();
  }

  std::map<uint32_t, Shard> replacements;
  uint64_t next_clock = clock_;
  if (candidate.partition_by_final_range) {
    if (candidate.requested_shard_bounds.size() !=
        candidate.requested_shards.size())
      return false;
    for (const HighlightShardBounds &bounds :
         candidate.requested_shard_bounds) {
      Shard shard;
      shard.start_row = bounds.start_row;
      shard.end_row = bounds.end_row;
      shard.last_used = ++next_clock;
      replacements.emplace(shard.start_row, std::move(shard));
    }
  }
  for (const HighlightQueryShard &source : candidate.shards) {
    if (!source.query)
      return false;
    if (!candidate.partition_by_final_range) {
      Shard shard;
      shard.start_row = source.start_row;
      shard.end_row = source.end_row;
      shard.last_used = ++next_clock;
      shard.ranges.reserve(source.query->captures.size());
      replacements.emplace(shard.start_row, std::move(shard));
    }
    for (const QueryCaptureRecord &capture : source.query->captures) {
      if (capture.name_id >= source.query->capture_names.size())
        continue;
      const std::string grammar_id =
          capture.layer_index < source.query->layers.size()
              ? source.query->layers[capture.layer_index].grammar_id
              : std::string{};
      const uint64_t layer_id =
          capture.layer_index < source.query->layers.size()
              ? source.query->layers[capture.layer_index].layer_id
              : 0;
      const auto scope = next_scope_ids.find(
          {grammar_id, source.query->capture_names[capture.name_id]});
      if (scope == next_scope_ids.end() || scope->second == 0)
        continue;
      const uint64_t stable_order =
          (capture.flags & QUERY_CAPTURE_LANGUAGE_SCOPE) != 0
              ? 0
              : 1 +
                    ((static_cast<uint64_t>(capture.node_start_index) << 32u) |
                     (static_cast<uint64_t>(capture.pattern_index &
                                            UINT32_C(0xffff))
                      << 16u) |
                     static_cast<uint64_t>(capture.order &
                                           UINT32_C(0xffff)));
      const IndexedRange indexed{
          ScopedRange{scope->second, capture.start_index, capture.end_index,
                      stable_order, capture.depth},
          layer_id, capture.pattern_index, capture.node_start_index,
          capture.node_end_index};
      if (!candidate.partition_by_final_range) {
        replacements.at(source.start_row).ranges.push_back(indexed);
        continue;
      }
      for (const HighlightShardBounds &bounds :
           candidate.requested_shard_bounds) {
        const uint32_t start =
            std::max(capture.start_index, bounds.start_offset);
        const uint32_t end = std::min(capture.end_index, bounds.end_offset);
        if (capture.start_index == capture.end_index) {
          if (capture.start_index < bounds.start_offset ||
              capture.start_index > bounds.end_offset ||
              (capture.start_index == bounds.end_offset &&
               !bounds.includes_end))
            continue;
        } else if (start >= end) {
          continue;
        }
        replacements.at(bounds.start_row).ranges.push_back(indexed);
      }
    }
  }
  for (auto &[_, shard] : replacements) {
    shard.ranges.shrink_to_fit();
    shard.retained_bytes =
        sizeof(Shard) + shard.ranges.capacity() * sizeof(IndexedRange);
  }

  std::set<uint32_t> protected_shards;
  for (uint32_t start : candidate.requested_shards)
    protected_shards.insert(highlight_shard_start(start));
  for (uint32_t start : protected_shards) {
    if (replacements.find(start) == replacements.end() &&
        shards_.find(start) == shards_.end())
      return false;
  }

  size_t next_retained_bytes = retained_bytes_;
  size_t next_shard_count = shards_.size();
  for (const auto &[start, replacement] : replacements) {
    const auto existing = shards_.find(start);
    if (existing == shards_.end()) {
      next_shard_count++;
    } else {
      next_retained_bytes -= existing->second.retained_bytes;
    }
    next_retained_bytes += replacement.retained_bytes;
  }

  std::vector<std::pair<uint64_t, uint32_t>> eviction_candidates;
  for (const auto &[start, shard] : shards_) {
    if (protected_shards.find(start) == protected_shards.end() &&
        replacements.find(start) == replacements.end())
      eviction_candidates.emplace_back(shard.last_used, start);
  }
  std::sort(eviction_candidates.begin(), eviction_candidates.end());
  std::vector<uint32_t> planned_evictions;
  size_t eviction_index = 0;
  while ((next_shard_count > HIGHLIGHT_SHARD_CAPACITY ||
          next_retained_bytes + next_scope_id_bytes >
              HIGHLIGHT_SHARD_BYTE_CAPACITY) &&
         eviction_index < eviction_candidates.size()) {
    const uint32_t start = eviction_candidates[eviction_index++].second;
    const auto existing = shards_.find(start);
    if (existing == shards_.end())
      continue;
    next_retained_bytes -= existing->second.retained_bytes;
    next_shard_count--;
    planned_evictions.push_back(start);
  }
  if (next_shard_count > HIGHLIGHT_SHARD_CAPACITY ||
      next_retained_bytes + next_scope_id_bytes >
          HIGHLIGHT_SHARD_BYTE_CAPACITY)
    return false;

  scope_ids_ = std::move(next_scope_ids);
  scope_id_bytes_ = next_scope_id_bytes;
  clock_ = next_clock;

  for (uint32_t start : planned_evictions) {
    shards_.erase(start);
    evictions_++;
  }

  for (auto &[start_row, replacement] : replacements) {
    auto existing = shards_.find(start_row);
    if (existing != shards_.end())
      shards_.erase(existing);
    shards_.emplace(start_row, std::move(replacement));
  }
  retained_bytes_ = next_retained_bytes;
  generation_++;
  return true;
}

HighlightIndexDiagnostics NativeHighlightIndex::diagnostics() const {
  HighlightIndexDiagnostics result;
  result.generation = generation_;
  result.shard_count = shards_.size();
  result.retained_bytes = retained_bytes_ + scope_id_bytes_;
  result.evictions = evictions_;
  for (const auto &[_, shard] : shards_) {
    result.range_count += shard.ranges.size();
    result.zero_length_range_count +=
        std::count_if(shard.ranges.begin(), shard.ranges.end(),
                      [](const IndexedRange &indexed) {
                        return indexed.range.start_offset ==
                               indexed.range.end_offset;
                      });
  }
  return result;
}

} // namespace document_engine
