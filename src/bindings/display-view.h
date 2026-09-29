#ifndef LUMINE_DOCUMENT_ENGINE_DISPLAY_VIEW_H_
#define LUMINE_DOCUMENT_ENGINE_DISPLAY_VIEW_H_

#include "bindings/document-session.h"
#include "display/display-index.h"
#include "display/render-plan.h"

#include <napi.h>

#include <memory>
#include <unordered_map>

namespace document_engine {

struct DisplayViewState {
  struct LineIdentity {
    uint64_t fingerprint = 0;
    uint64_t id = 0;
    Point buffer_start;
    Point buffer_end;
    bool initialized = false;
  };

  std::shared_ptr<SessionState> session;
  bool destroyed = false;
  uint32_t wrap_column = 0;
  uint32_t tab_length = 2;
  uint64_t fold_generation = 0;
  uint64_t target_buffer_revision = 0;
  uint64_t display_revision = 0;
  uint64_t cached_buffer_revision = UINT64_MAX;
  uint64_t cached_fold_generation = UINT64_MAX;
  uint64_t highlight_buffer_revision = 0;
  uint64_t highlight_syntax_revision = 0;
  std::unordered_map<uint32_t, Fold> folds;
  std::unique_ptr<DisplayIndex> index;
  bool index_initialized = false;
  std::vector<LineIdentity> line_identities;
  std::vector<LineIdentity> pending_line_identities;
  std::vector<RevisionEditBatch> pending_display_edits;
  bool pending_display_edits_eligible = true;
  RenderStyle render_style;
  std::vector<ScopedRange> highlight_ranges;
  uint64_t accepted_edit_count = 0;
  uint64_t fold_delta_count = 0;
  uint64_t fold_reset_count = 0;
  uint64_t index_rebuild_count = 0;
  uint64_t index_incremental_update_count = 0;
  uint64_t index_incremental_fallback_count = 0;
  uint64_t index_incremental_rows_rebuilt = 0;
  uint64_t index_incremental_rows_reused = 0;
  uint64_t index_incremental_layout_units_scanned = 0;
  uint64_t render_plan_count = 0;
  double fold_delta_milliseconds = 0;
  double fold_delta_maximum_milliseconds = 0;
  double fold_reset_milliseconds = 0;
  double index_rebuild_milliseconds = 0;
  double index_incremental_update_milliseconds = 0;
  double render_plan_milliseconds = 0;
};

void display_view_accept_revision(
    const std::shared_ptr<DisplayViewState> &state, uint64_t revision,
    const std::vector<uint32_t> &packed_edits,
    const std::shared_ptr<const SnapshotAnalysis> &old_analysis,
    const std::shared_ptr<const SnapshotAnalysis> &new_analysis);

class DisplayViewWrapper : public Napi::ObjectWrap<DisplayViewWrapper> {
public:
  static Napi::Function init(Napi::Env env);
  static Napi::Object new_instance(Napi::Env env,
                                   std::shared_ptr<SessionState> session,
                                   Napi::Value options);

  explicit DisplayViewWrapper(const Napi::CallbackInfo &info);
  ~DisplayViewWrapper() override;

  std::shared_ptr<DisplayViewState> state() const;
  bool replace_folds_for_revision(Napi::Env env, uint64_t buffer_revision,
                                  uint64_t generation,
                                  const Napi::Value &packed_ranges);

private:
  struct RenderPlanLineData {
    uint64_t id = 0;
    int64_t soft_wrap_indent = -1;
    RenderedLine rendered;
  };

  struct RenderPlanData {
    uint64_t buffer_revision = 0;
    uint64_t syntax_revision = 0;
    uint64_t display_revision = 0;
    uint64_t fold_generation = 0;
    uint64_t highlight_generation = 0;
    uint32_t highlight_coverage_start_row = 0;
    uint32_t highlight_coverage_end_row = 0;
    bool highlight_coverage_complete = false;
    uint64_t indexed_buffer_row_count = 0;
    std::vector<RenderPlanLineData> lines;
  };

  Napi::Value apply_fold_deltas(const Napi::CallbackInfo &info);
  Napi::Value replace_folds(const Napi::CallbackInfo &info);
  Napi::Value replace_highlight_ranges(const Napi::CallbackInfo &info);
  Napi::Value buffer_to_screen(const Napi::CallbackInfo &info);
  Napi::Value screen_to_buffer(const Napi::CallbackInfo &info);
  Napi::Value project_buffer_ranges(const Napi::CallbackInfo &info);
  Napi::Value get_screen_line_count(const Napi::CallbackInfo &info);
  Napi::Value line_length_for_screen_row(const Napi::CallbackInfo &info);
  Napi::Value get_rightmost_screen_position(const Napi::CallbackInfo &info);
  Napi::Value get_indexed_summary(const Napi::CallbackInfo &info);
  Napi::Value buffer_rows_for_screen_rows(const Napi::CallbackInfo &info);
  Napi::Value highlight_shard_starts_for_screen_rows(
      const Napi::CallbackInfo &info);
  Napi::Value translate_screen_column_block(const Napi::CallbackInfo &info);
  Napi::Value build_render_plan(const Napi::CallbackInfo &info);
  Napi::Value build_render_plan_packed(const Napi::CallbackInfo &info);
  Napi::Value get_diagnostics(const Napi::CallbackInfo &info);
  Napi::Value destroy(const Napi::CallbackInfo &info);

  bool ensure_index(Napi::Env env, const SuperstringSnapshotLease **lease,
                    std::shared_ptr<const SnapshotAnalysis> *analysis);
  bool collect_render_plan(Napi::Env env, uint64_t start, uint64_t end,
                           RenderPlanData *plan);

  std::shared_ptr<DisplayViewState> state_;
};

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_DISPLAY_VIEW_H_
