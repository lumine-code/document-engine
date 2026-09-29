#ifndef LUMINE_DOCUMENT_ENGINE_SNAPSHOT_READER_H_
#define LUMINE_DOCUMENT_ENGINE_SNAPSHOT_READER_H_

#include "core-types.h"

#include <cstdint>
#include <string>
#include <vector>

struct SuperstringSnapshotLease;

namespace document_engine {

using SnapshotCancellationFunction = bool (*)(void *payload);

class SnapshotReader {
public:
  explicit SnapshotReader(const SuperstringSnapshotLease *lease);

  bool valid() const;
  const std::string &error() const;
  uint64_t size() const;
  uint64_t chunk_count() const;
  char16_t character_at(uint64_t offset) const;
  bool read_utf16le(uint32_t byte_offset, const char **data,
                    uint32_t *bytes_read) const;
  bool append_range(uint64_t start, uint64_t end,
                    std::u16string &output) const;
  bool content_checksum(uint64_t &checksum,
                        SnapshotCancellationFunction cancellation = nullptr,
                        void *cancellation_payload = nullptr) const;
  bool has_line_index() const;
  bool analyze_line_index(SnapshotAnalysis &analysis,
                          uint64_t &lines_read) const;
  bool analyze_line_index_incremental_batch(
      const SnapshotAnalysis &previous,
      const std::vector<uint32_t> &packed_edits,
      SnapshotAnalysis &analysis,
      uint64_t &lines_read) const;
  bool analyze_line_index_incremental(
      const SnapshotAnalysis &previous,
      const Point &old_start, const Point &old_end,
      const Point &new_start, const Point &new_end,
      SnapshotAnalysis &analysis, uint64_t &lines_read) const;
  bool analyze(SnapshotAnalysis &analysis, uint64_t &chunks_read,
               SnapshotCancellationFunction cancellation = nullptr,
               void *cancellation_payload = nullptr) const;
  bool analyze_incremental(const SnapshotAnalysis &previous,
                           const Point &old_start, const Point &old_end,
                           const Point &new_start, const Point &new_end,
                           SnapshotAnalysis &analysis,
                           uint64_t &chunks_read,
                           SnapshotCancellationFunction cancellation = nullptr,
                           void *cancellation_payload = nullptr) const;

private:
  struct Chunk {
    const uint16_t *data = nullptr;
    uint64_t start = 0;
    uint64_t length = 0;
    uint64_t index = 0;
  };

  bool ensure_chunk_count() const;
  bool read_chunk(uint64_t index, Chunk *result) const;
  bool chunk_for_offset(uint64_t offset, Chunk *result) const;
  bool validate_adjacent_chunks(const Chunk &left, const Chunk &right) const;
  void cache_chunk(const Chunk &chunk) const;
  void set_error(const char *message) const;

  const SuperstringSnapshotLease *lease_ = nullptr;
  uint64_t size_ = 0;
  mutable uint64_t chunk_count_ = 0;
  mutable bool has_chunk_count_ = false;
  mutable Chunk cached_chunk_;
  mutable bool has_cached_chunk_ = false;
  mutable std::string error_;
};

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_SNAPSHOT_READER_H_
