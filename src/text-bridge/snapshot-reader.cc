#include "text-bridge/snapshot-reader.h"

#include "snapshot-lease.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace document_engine {

SnapshotReader::SnapshotReader(const SuperstringSnapshotLease *lease)
    : lease_(lease) {
  if (lease_ == nullptr || lease_->functions == nullptr) {
    error_ = "Snapshot lease is null";
    return;
  }

  size_ = lease_->utf16_length;
  if (lease_->chunk_count >
      static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    error_ = "Snapshot contains too many chunks for this process";
    return;
  }

  chunks_.reserve(static_cast<size_t>(lease_->chunk_count));
  uint64_t expected_start = 0;
  for (uint64_t index = 0; index < lease_->chunk_count; index++) {
    SuperstringSnapshotChunk chunk{};
    const SuperstringSnapshotLeaseStatus status =
        lease_->functions->chunk_at(lease_->context, index, &chunk);
    if (status != SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK) {
      error_ = "Snapshot lease rejected a chunk read";
      chunks_.clear();
      return;
    }
    if (chunk.start != expected_start ||
        (chunk.length > 0 && chunk.data == nullptr) ||
        chunk.length > size_ - std::min<uint64_t>(size_, chunk.start)) {
      error_ = "Snapshot lease exposed non-contiguous or invalid chunks";
      chunks_.clear();
      return;
    }
    chunks_.push_back(Chunk{chunk.data, chunk.start, chunk.length});
    expected_start += chunk.length;
  }

  if (expected_start != size_) {
    error_ = "Snapshot chunk lengths do not match the advertised length";
    chunks_.clear();
  }
}

bool SnapshotReader::valid() const { return error_.empty(); }

const std::string &SnapshotReader::error() const { return error_; }

uint64_t SnapshotReader::size() const { return size_; }

uint64_t SnapshotReader::chunk_count() const { return chunks_.size(); }

const SnapshotReader::Chunk *
SnapshotReader::chunk_for_offset(uint64_t offset) const {
  if (offset >= size_)
    return nullptr;
  auto iterator = std::upper_bound(
      chunks_.begin(), chunks_.end(), offset,
      [](uint64_t value, const Chunk &chunk) { return value < chunk.start; });
  if (iterator == chunks_.begin())
    return nullptr;
  --iterator;
  return offset < iterator->start + iterator->length ? &*iterator : nullptr;
}

char16_t SnapshotReader::character_at(uint64_t offset) const {
  const Chunk *chunk = chunk_for_offset(offset);
  return chunk == nullptr
             ? 0
             : static_cast<char16_t>(chunk->data[offset - chunk->start]);
}

bool SnapshotReader::read_utf16le(uint32_t byte_offset, const char **data,
                                  uint32_t *bytes_read) const {
  if (data == nullptr || bytes_read == nullptr)
    return false;
  *data = nullptr;
  *bytes_read = 0;
  if (!valid() || std::endian::native != std::endian::little)
    return false;

  const uint64_t total_bytes = size_ * UINT64_C(2);
  if (byte_offset >= total_bytes)
    return true;

  const uint64_t code_unit_offset = byte_offset / 2;
  const Chunk *chunk = chunk_for_offset(code_unit_offset);
  if (chunk == nullptr)
    return false;

  const uint64_t chunk_byte_start = chunk->start * UINT64_C(2);
  const uint64_t byte_within_chunk = byte_offset - chunk_byte_start;
  const uint64_t chunk_byte_length = chunk->length * UINT64_C(2);
  if (byte_within_chunk >= chunk_byte_length)
    return false;
  const uint64_t available = chunk_byte_length - byte_within_chunk;
  *data = reinterpret_cast<const char *>(chunk->data) + byte_within_chunk;
  *bytes_read = static_cast<uint32_t>(
      std::min<uint64_t>(available, std::numeric_limits<uint32_t>::max()));
  return true;
}

bool SnapshotReader::append_range(uint64_t start, uint64_t end,
                                  std::u16string &output) const {
  if (!valid() || start > end || end > size_)
    return false;
  uint64_t offset = start;
  while (offset < end) {
    const Chunk *chunk = chunk_for_offset(offset);
    if (chunk == nullptr)
      return false;
    const uint64_t available = chunk->start + chunk->length - offset;
    const uint64_t count = std::min<uint64_t>(available, end - offset);
    if (count > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
      return false;
    const uint16_t *source = chunk->data + (offset - chunk->start);
    output.append(reinterpret_cast<const char16_t *>(source),
                  static_cast<size_t>(count));
    offset += count;
  }
  return true;
}

bool SnapshotReader::content_checksum(
    uint64_t &checksum, SnapshotCancellationFunction cancellation,
    void *cancellation_payload) const {
  if (!valid())
    return false;
  constexpr uint64_t fnv_offset_basis = UINT64_C(14695981039346656037);
  constexpr uint64_t fnv_prime = UINT64_C(1099511628211);
  uint64_t value = fnv_offset_basis;
  uint64_t visited = 0;
  for (const Chunk &chunk : chunks_) {
    for (uint64_t index = 0; index < chunk.length; index++, visited++) {
      if ((visited & UINT64_C(4095)) == 0 && cancellation != nullptr &&
          cancellation(cancellation_payload))
        return false;
      const char16_t character = static_cast<char16_t>(chunk.data[index]);
      value ^= static_cast<uint8_t>(character & 0xffu);
      value *= fnv_prime;
      value ^= static_cast<uint8_t>((character >> 8u) & 0xffu);
      value *= fnv_prime;
    }
  }
  checksum = value;
  return true;
}

bool SnapshotReader::has_line_index() const {
  return valid() && superstring_snapshot_lease_has_line_index(lease_);
}

bool SnapshotReader::analyze_line_index(SnapshotAnalysis &analysis,
                                        uint64_t &lines_read) const {
  if (!has_line_index())
    return false;
  uint64_t line_count = 0;
  if (lease_->functions->line_count(lease_->context, &line_count) !=
          SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK ||
      line_count == 0 ||
      line_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    return false;

  analysis = SnapshotAnalysis{};
  analysis.utf16_length = size_;
  analysis.line_starts.reserve(static_cast<size_t>(line_count));
  analysis.line_ends.reserve(static_cast<size_t>(line_count));
  constexpr uint64_t fnv_offset_basis = UINT64_C(14695981039346656037);
  constexpr uint64_t fnv_prime = UINT64_C(1099511628211);
  uint64_t checksum = fnv_offset_basis;
  uint64_t expected_start = 0;
  for (uint64_t row = 0; row < line_count; row++) {
    SuperstringSnapshotLine line{};
    if (lease_->functions->line_at(lease_->context, row, &line) !=
            SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK ||
        line.start != expected_start || line.content_length > size_ - line.start ||
        line.ending_length > size_ - line.start - line.content_length)
      return false;
    const uint64_t end = line.start + line.content_length;
    analysis.line_starts.push_back(line.start);
    analysis.line_ends.push_back(end);
    expected_start = end + line.ending_length;
    for (uint64_t value : {line.start, line.content_length,
                           static_cast<uint64_t>(line.ending_length)}) {
      for (unsigned shift = 0; shift < 64; shift += 8) {
        checksum ^= static_cast<uint8_t>((value >> shift) & UINT64_C(0xff));
        checksum *= fnv_prime;
      }
    }
    lines_read++;
  }
  analysis.checksum = checksum;
  analysis.checksum_complete = false;
  return expected_start == size_ &&
         analysis.line_starts.size() == analysis.line_ends.size();
}

bool SnapshotReader::analyze_line_index_incremental_batch(
    const SnapshotAnalysis &previous,
    const std::vector<uint32_t> &packed_edits,
    SnapshotAnalysis &analysis,
    uint64_t &lines_read) const {
  if (!has_line_index() || packed_edits.empty() ||
      packed_edits.size() % 8 != 0 || previous.line_starts.empty())
    return false;

  uint64_t final_line_count = 0;
  if (lease_->functions->line_count(lease_->context, &final_line_count) !=
          SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK ||
      final_line_count == 0 ||
      final_line_count > static_cast<uint64_t>(
                             std::numeric_limits<size_t>::max()))
    return false;
  const size_t old_count = previous.line_starts.size();
  const size_t new_count = static_cast<size_t>(final_line_count);
  std::vector<uint8_t> affected_old(old_count, 0);
  std::vector<uint8_t> affected_new(new_count, 0);
  struct RowEdit {
    Point old_start;
    Point old_end;
    Point new_start;
    Point new_end;
  };
  std::vector<RowEdit> edits;
  edits.reserve(packed_edits.size() / 8);
  for (size_t index = 0; index < packed_edits.size(); index += 8) {
    RowEdit edit{
        Point{packed_edits[index], packed_edits[index + 1]},
        Point{packed_edits[index + 2], packed_edits[index + 3]},
        Point{packed_edits[index + 4], packed_edits[index + 5]},
        Point{packed_edits[index + 6], packed_edits[index + 7]}};
    if (edit.old_start.row >= old_count || edit.old_end.row >= old_count ||
        edit.new_start.row >= new_count || edit.new_end.row >= new_count ||
        edit.old_end < edit.old_start || edit.new_end < edit.new_start)
      return false;
    if (!edits.empty() && edit.old_start < edits.back().old_end)
      return false;
    edits.push_back(edit);
    const uint64_t old_first = edit.old_start.row == 0
                                   ? 0
                                   : edit.old_start.row - 1;
    const uint64_t old_last =
        std::min<uint64_t>(old_count - 1, edit.old_end.row + 1);
    for (uint64_t row = old_first; row <= old_last; row++)
      affected_old[row] = 1;
    const uint64_t new_first = edit.new_start.row == 0
                                   ? 0
                                   : edit.new_start.row - 1;
    const uint64_t new_last =
        std::min<uint64_t>(new_count - 1, edit.new_end.row + 1);
    for (uint64_t row = new_first; row <= new_last; row++)
      affected_new[row] = 1;
  }

  std::vector<int64_t> old_row_for_new(new_count, -1);
  for (uint64_t old_row = 0; old_row < old_count; old_row++) {
    if (affected_old[old_row])
      continue;
    uint64_t new_row = old_row;
    const Point row_start{old_row, 0};
    for (auto iterator = edits.rbegin(); iterator != edits.rend(); iterator++) {
      if (iterator->old_end <= row_start) {
        new_row = iterator->new_end.row +
                  (old_row - iterator->old_end.row);
        break;
      }
    }
    if (new_row < new_count && !affected_new[new_row])
      old_row_for_new[new_row] = static_cast<int64_t>(old_row);
  }

  analysis = SnapshotAnalysis{};
  analysis.utf16_length = size_;
  analysis.line_starts.reserve(new_count);
  analysis.line_ends.reserve(new_count);
  constexpr uint64_t fnv_offset_basis = UINT64_C(14695981039346656037);
  constexpr uint64_t fnv_prime = UINT64_C(1099511628211);
  uint64_t checksum = fnv_offset_basis;
  uint64_t expected_start = 0;
  for (uint64_t new_row = 0; new_row < new_count; new_row++) {
    uint64_t content_length = 0;
    uint64_t ending_length = 0;
    const int64_t old_row = old_row_for_new[new_row];
    if (old_row >= 0) {
      const size_t source_row = static_cast<size_t>(old_row);
      content_length = previous.line_ends[source_row] -
                       previous.line_starts[source_row];
      const uint64_t next_start =
          source_row + 1 < old_count
              ? previous.line_starts[source_row + 1]
              : previous.utf16_length;
      if (next_start < previous.line_ends[source_row])
        return false;
      ending_length = next_start - previous.line_ends[source_row];
    } else {
      SuperstringSnapshotLine line{};
      if (lease_->functions->line_at(lease_->context, new_row, &line) !=
              SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK ||
          line.start != expected_start)
        return false;
      content_length = line.content_length;
      ending_length = line.ending_length;
      lines_read++;
    }
    if (content_length > size_ - expected_start ||
        ending_length > size_ - expected_start - content_length)
      return false;
    analysis.line_starts.push_back(expected_start);
    analysis.line_ends.push_back(expected_start + content_length);
    for (uint64_t value : {expected_start, content_length, ending_length}) {
      for (unsigned shift = 0; shift < 64; shift += 8) {
        checksum ^= static_cast<uint8_t>((value >> shift) & UINT64_C(0xff));
        checksum *= fnv_prime;
      }
    }
    expected_start += content_length + ending_length;
  }
  analysis.checksum = checksum;
  analysis.checksum_complete = false;
  return expected_start == size_;
}

bool SnapshotReader::analyze(SnapshotAnalysis &analysis,
                             uint64_t &chunks_read,
                             SnapshotCancellationFunction cancellation,
                             void *cancellation_payload) const {
  if (!valid())
    return false;

  analysis = SnapshotAnalysis{};
  analysis.utf16_length = size_;
  analysis.line_starts.push_back(0);
  constexpr uint64_t fnv_offset_basis = UINT64_C(14695981039346656037);
  constexpr uint64_t fnv_prime = UINT64_C(1099511628211);
  uint64_t checksum = fnv_offset_basis;
  char16_t previous = 0;

  for (const Chunk &chunk : chunks_) {
    chunks_read++;
    for (uint64_t index = 0; index < chunk.length; index++) {
      if ((index & UINT64_C(4095)) == 0 && cancellation != nullptr &&
          cancellation(cancellation_payload))
        return false;
      const char16_t character = static_cast<char16_t>(chunk.data[index]);
      checksum ^= static_cast<uint8_t>(character & 0xffu);
      checksum *= fnv_prime;
      checksum ^= static_cast<uint8_t>((character >> 8u) & 0xffu);
      checksum *= fnv_prime;

      const uint64_t absolute_offset = chunk.start + index;
      if (character == u'\n') {
        analysis.line_ends.push_back(
            previous == u'\r' && absolute_offset > 0 ? absolute_offset - 1
                                                     : absolute_offset);
        analysis.line_starts.push_back(absolute_offset + 1);
      }
      previous = character;
    }
  }

  analysis.line_ends.push_back(size_);
  analysis.checksum = checksum;
  analysis.checksum_complete = true;
  return analysis.line_starts.size() == analysis.line_ends.size();
}

bool SnapshotReader::analyze_incremental(
    const SnapshotAnalysis &previous, const Point &old_start,
    const Point &old_end, const Point &new_start, const Point &new_end,
    SnapshotAnalysis &analysis, uint64_t &chunks_read,
    SnapshotCancellationFunction cancellation,
    void *cancellation_payload) const {
  if (!valid() || old_start != new_start || old_start.row > old_end.row ||
      new_start.row > new_end.row ||
      old_end.row >= previous.line_starts.size() ||
      old_start.row >= previous.line_starts.size())
    return false;

  const uint64_t old_start_line = previous.line_starts[old_start.row];
  const uint64_t old_end_line = previous.line_starts[old_end.row];
  if (old_start.column >
          previous.line_ends[old_start.row] - old_start_line ||
      old_end.column > previous.line_ends[old_end.row] - old_end_line)
    return false;
  const uint64_t start_offset = old_start_line + old_start.column;
  const uint64_t old_end_offset = old_end_line + old_end.column;
  if (start_offset > old_end_offset || start_offset > size_)
    return false;

  analysis = SnapshotAnalysis{};
  analysis.utf16_length = size_;
  analysis.line_starts.reserve(
      previous.line_starts.size() +
      static_cast<size_t>(new_end.row - new_start.row));
  analysis.line_ends.reserve(analysis.line_starts.capacity());
  for (uint64_t row = 0; row < old_start.row; row++) {
    analysis.line_starts.push_back(previous.line_starts[row]);
    analysis.line_ends.push_back(previous.line_ends[row]);
  }
  analysis.line_starts.push_back(old_start_line);

  constexpr uint64_t fnv_prime = UINT64_C(1099511628211);
  uint64_t checksum = previous.checksum;
  auto hash_uint64 = [&checksum](uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      checksum ^= static_cast<uint8_t>((value >> shift) & UINT64_C(0xff));
      checksum *= fnv_prime;
    }
  };
  hash_uint64(old_start.row);
  hash_uint64(old_start.column);
  hash_uint64(old_end.row);
  hash_uint64(old_end.column);
  hash_uint64(new_end.row);
  hash_uint64(new_end.column);

  const uint64_t rows_to_scan = new_end.row - new_start.row;
  uint64_t rows_scanned = 0;
  uint64_t offset = start_offset;
  const Chunk *last_chunk = nullptr;
  char16_t previous_character = 0;
  if (offset > 0) {
    last_chunk = chunk_for_offset(offset - 1);
    if (last_chunk == nullptr)
      return false;
    chunks_read++;
    previous_character = static_cast<char16_t>(
        last_chunk->data[offset - 1 - last_chunk->start]);
  }
  while (rows_scanned < rows_to_scan) {
    if ((offset & UINT64_C(4095)) == 0 && cancellation != nullptr &&
        cancellation(cancellation_payload))
      return false;
    if (offset >= size_)
      return false;
    const Chunk *chunk = chunk_for_offset(offset);
    if (chunk == nullptr)
      return false;
    if (chunk != last_chunk) {
      chunks_read++;
      last_chunk = chunk;
    }
    const char16_t character =
        static_cast<char16_t>(chunk->data[offset - chunk->start]);
    checksum ^= static_cast<uint8_t>(character & 0xffu);
    checksum *= fnv_prime;
    checksum ^= static_cast<uint8_t>((character >> 8u) & 0xffu);
    checksum *= fnv_prime;
    if (character == u'\n') {
      analysis.line_ends.push_back(
          previous_character == u'\r' && offset > 0 ? offset - 1 : offset);
      analysis.line_starts.push_back(offset + 1);
      rows_scanned++;
    }
    previous_character = character;
    offset++;
  }

  if (rows_to_scan == 0 && new_end.column < new_start.column)
    return false;
  const uint64_t final_column = rows_to_scan == 0
                                    ? new_end.column - new_start.column
                                    : new_end.column;
  if (final_column > size_ - offset)
    return false;
  const uint64_t new_end_offset = offset + final_column;
  while (offset < new_end_offset) {
    if ((offset & UINT64_C(4095)) == 0 && cancellation != nullptr &&
        cancellation(cancellation_payload))
      return false;
    const Chunk *chunk = chunk_for_offset(offset);
    if (chunk == nullptr)
      return false;
    if (chunk != last_chunk) {
      chunks_read++;
      last_chunk = chunk;
    }
    const char16_t character =
        static_cast<char16_t>(chunk->data[offset - chunk->start]);
    if (character == u'\n')
      return false;
    checksum ^= static_cast<uint8_t>(character & 0xffu);
    checksum *= fnv_prime;
    checksum ^= static_cast<uint8_t>((character >> 8u) & 0xffu);
    checksum *= fnv_prime;
    offset++;
  }

  const int64_t delta = static_cast<int64_t>(new_end_offset) -
                        static_cast<int64_t>(old_end_offset);
  const int64_t changed_line_end =
      static_cast<int64_t>(previous.line_ends[old_end.row]) + delta;
  if (changed_line_end < 0 ||
      static_cast<uint64_t>(changed_line_end) > size_)
    return false;
  analysis.line_ends.push_back(static_cast<uint64_t>(changed_line_end));

  for (uint64_t row = old_end.row + 1; row < previous.line_starts.size();
       row++) {
    const int64_t translated_start =
        static_cast<int64_t>(previous.line_starts[row]) + delta;
    const int64_t translated_end =
        static_cast<int64_t>(previous.line_ends[row]) + delta;
    if (translated_start < 0 || translated_end < translated_start ||
        static_cast<uint64_t>(translated_end) > size_)
      return false;
    analysis.line_starts.push_back(static_cast<uint64_t>(translated_start));
    analysis.line_ends.push_back(static_cast<uint64_t>(translated_end));
  }
  hash_uint64(size_);
  analysis.checksum = checksum;
  analysis.checksum_complete = false;
  return analysis.line_starts.size() == analysis.line_ends.size() &&
         !analysis.line_starts.empty() && analysis.line_ends.back() == size_;
}

} // namespace document_engine
