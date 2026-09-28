#ifndef LUMINE_DOCUMENT_ENGINE_LAYERED_QUERY_H_
#define LUMINE_DOCUMENT_ENGINE_LAYERED_QUERY_H_

#include "syntax/injection-engine.h"
#include "syntax/query-engine.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace document_engine {

class PublishedSyntaxSnapshot;
class QuerySnapshotCache;
class SnapshotReader;

struct LayeredQueryContext {
  std::string root_grammar_id;
  QueryResolutionContext defaults;
  std::map<std::string, QueryResolutionContext> by_grammar;
  bool include_language_scopes = true;
};

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
    std::shared_ptr<const QueryIndexSnapshot> &snapshot,
    QueryErrorInfo &error, QueryRunStatistics &statistics);

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_LAYERED_QUERY_H_
