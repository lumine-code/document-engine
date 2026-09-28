#ifndef LUMINE_DOCUMENT_ENGINE_SCOPE_RESOLVER_H_
#define LUMINE_DOCUMENT_ENGINE_SCOPE_RESOLVER_H_

#include "core-types.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct TSNode;

namespace document_engine {

class SnapshotReader;
struct QueryPatternMetadata;

using QueryConfigValue =
    std::variant<std::monostate, bool, double, std::string>;

struct QueryResolutionContext {
  bool resolve_scopes = true;
  bool interpolate_names = false;
  uint32_t injection_depth = 0;
  std::map<std::string, QueryConfigValue> config;
  bool local_ranges_enabled = false;
  std::vector<std::pair<uint32_t, uint32_t>> local_ranges;
};

struct ResolvedQueryCapture {
  bool accepted = false;
  std::string name;
  uint32_t start_row = 0;
  uint32_t start_column = 0;
  uint32_t end_row = 0;
  uint32_t end_column = 0;
  uint32_t start_index = 0;
  uint32_t end_index = 0;
};

struct ScopeResolutionStatistics {
  uint64_t tested = 0;
  uint64_t accepted = 0;
  uint64_t rejected = 0;
  uint64_t adjusted = 0;
  uint64_t invalid_adjustments = 0;
  uint64_t local_predicates = 0;
};

// Resolves Lumine's query conventions while TSNode relationships are still
// available. A resolver is deliberately scoped to one cursor execution:
// capture.final, capture.shy and rangeWithData are ordering-sensitive.
class NativeScopeResolver {
public:
  NativeScopeResolver(const SnapshotReader &reader,
                      const SnapshotAnalysis &analysis,
                      const QueryResolutionContext &context);
  ~NativeScopeResolver();

  NativeScopeResolver(const NativeScopeResolver &) = delete;
  NativeScopeResolver &operator=(const NativeScopeResolver &) = delete;

  bool resolve(TSNode node, const std::string &capture_name,
               const QueryPatternMetadata &metadata,
               ResolvedQueryCapture &capture);
  const ScopeResolutionStatistics &statistics() const;

private:
  struct Impl;
  Impl *impl_ = nullptr;
};

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_SCOPE_RESOLVER_H_
