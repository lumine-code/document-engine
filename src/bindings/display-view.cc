#include "bindings/display-view.h"

#include "bindings/addon-data.h"
#include "revision-projection.h"
#include "snapshot-lease.h"
#include "syntax/highlight-index.h"
#include "syntax/injection-engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace document_engine {

namespace {

void set_error_code(Napi::Error &error, const char *code) {
  error.Value().Set("code", Napi::String::New(error.Env(), code));
}

Napi::Value throw_error(Napi::Env env, const char *message, const char *code) {
  Napi::Error error = Napi::Error::New(env, message);
  set_error_code(error, code);
  error.ThrowAsJavaScriptException();
  return env.Undefined();
}

Napi::Value throw_type_error(Napi::Env env, const char *message,
                             const char *code) {
  Napi::TypeError error = Napi::TypeError::New(env, message);
  set_error_code(error, code);
  error.ThrowAsJavaScriptException();
  return env.Undefined();
}

bool read_uint64(Napi::Value value, uint64_t *result) {
  if (!value.IsNumber())
    return false;
  const double number = value.As<Napi::Number>().DoubleValue();
  if (number < 0 || number > 9007199254740991.0 || number != number)
    return false;
  *result = static_cast<uint64_t>(number);
  return static_cast<double>(*result) == number;
}

bool read_uint32(Napi::Value value, uint32_t *result) {
  uint64_t wide = 0;
  if (!read_uint64(value, &wide) || wide > UINT32_MAX)
    return false;
  *result = static_cast<uint32_t>(wide);
  return true;
}

bool read_positive_number(Napi::Value value, double *result) {
  if (!value.IsNumber())
    return false;
  const double number = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(number) || number <= 0)
    return false;
  *result = number;
  return true;
}

bool is_uint32_array(Napi::Value value) {
  return value.IsTypedArray() &&
         value.As<Napi::TypedArray>().TypedArrayType() == napi_uint32_array;
}

ClipDirection read_clip_direction(Napi::Value value) {
  if (!value.IsString())
    return ClipDirection::Closest;
  const std::string text = value.As<Napi::String>();
  if (text == "backward")
    return ClipDirection::Backward;
  if (text == "forward")
    return ClipDirection::Forward;
  return ClipDirection::Closest;
}

bool read_point(Napi::Value value, Point *point) {
  if (!value.IsObject())
    return false;
  Napi::Object object = value.As<Napi::Object>();
  return read_uint64(object.Get("row"), &point->row) &&
         read_uint64(object.Get("column"), &point->column);
}

Napi::Object point_to_js(Napi::Env env, Point point) {
  Napi::Object result = Napi::Object::New(env);
  result.Set("row", Napi::Number::New(env, static_cast<double>(point.row)));
  result.Set("column",
             Napi::Number::New(env, static_cast<double>(point.column)));
  return result;
}

bool parse_fold_ranges(Napi::Env env, Napi::Value value,
                       std::unordered_map<uint32_t, Fold> *folds) {
  if (!is_uint32_array(value)) {
    throw_type_error(env, "Fold ranges must be a Uint32Array",
                     "ERR_INVALID_FOLD_RANGES");
    return false;
  }
  Napi::Uint32Array packed = value.As<Napi::Uint32Array>();
  if (packed.ElementLength() % 5 != 0) {
    throw_type_error(env, "Fold ranges must use stride 5",
                     "ERR_INVALID_FOLD_RANGES");
    return false;
  }
  folds->clear();
  for (size_t index = 0; index < packed.ElementLength(); index += 5) {
    const uint32_t id = packed[index];
    (*folds)[id] = Fold{
        id, Range{Point{packed[index + 1], packed[index + 2]},
                  Point{packed[index + 3], packed[index + 4]}}};
  }
  return true;
}

uint64_t hash_value(uint64_t hash, uint64_t value) {
  constexpr uint64_t prime = UINT64_C(1099511628211);
  for (uint32_t shift = 0; shift < 64; shift += 8) {
    hash ^= static_cast<uint8_t>((value >> shift) & 0xffu);
    hash *= prime;
  }
  return hash;
}

uint64_t line_fingerprint(const ScreenRow &row, const std::u16string &text,
                          const std::vector<int32_t> &tags) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (char16_t character : text)
    hash = hash_value(hash, character);
  for (int32_t tag : tags)
    hash = hash_value(hash, static_cast<uint32_t>(tag));
  hash = hash_value(hash, static_cast<uint64_t>(row.soft_wrap_indent));
  for (const DisplaySpan &span : row.spans) {
    hash = hash_value(hash, static_cast<uint64_t>(span.kind));
    hash = hash_value(hash, span.screen_start);
    hash = hash_value(hash, span.screen_end);
  }
  return hash;
}

struct LineRangeKey {
  Point start;
  Point end;

  bool operator==(const LineRangeKey &other) const {
    return start == other.start && end == other.end;
  }
};

struct LineRangeKeyHash {
  size_t operator()(const LineRangeKey &key) const {
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = hash_value(hash, key.start.row);
    hash = hash_value(hash, key.start.column);
    hash = hash_value(hash, key.end.row);
    hash = hash_value(hash, key.end.column);
    return static_cast<size_t>(hash);
  }
};

Point traverse_point(Point start, Point extent) {
  return extent.row == 0 ? Point{start.row, start.column + extent.column}
                         : Point{start.row + extent.row, extent.column};
}

Point translate_after_edit(Point point, Point old_end, Point new_end) {
  if (point.row == old_end.row) {
    return Point{new_end.row, new_end.column + point.column - old_end.column};
  }
  const int64_t row = static_cast<int64_t>(point.row) +
                      static_cast<int64_t>(new_end.row) -
                      static_cast<int64_t>(old_end.row);
  return Point{static_cast<uint64_t>(std::max<int64_t>(0, row)), point.column};
}

Point splice_point(Point point, Point start, Point old_end, Point new_end,
                   bool is_end) {
  if (point < start || (!is_end && point == start))
    return point;
  if (old_end <= point)
    return translate_after_edit(point, old_end, new_end);
  return is_end ? new_end : start;
}

void splice_folds(std::unordered_map<uint32_t, Fold> &folds, Point start,
                  Point old_extent, Point new_extent) {
  const Point old_end = traverse_point(start, old_extent);
  const Point new_end = traverse_point(start, new_extent);
  for (auto iterator = folds.begin(); iterator != folds.end();) {
    Fold &fold = iterator->second;
    fold.range.start =
        splice_point(fold.range.start, start, old_end, new_end, false);
    fold.range.end =
        splice_point(fold.range.end, start, old_end, new_end, true);
    if (!(fold.range.start < fold.range.end))
      iterator = folds.erase(iterator);
    else
      iterator++;
  }
}

uint64_t next_line_id() {
  // JavaScript represents every integer through 2^53 - 1 exactly. At one new
  // identity per microsecond this process-local counter lasts for centuries.
  static std::atomic<uint64_t> next{1};
  constexpr uint64_t max_safe_integer = UINT64_C(9007199254740991);
  const uint64_t id = next.fetch_add(1, std::memory_order_relaxed);
  return id <= max_safe_integer ? id : max_safe_integer;
}

} // namespace

void display_view_accept_revision(
    const std::shared_ptr<DisplayViewState> &state, uint64_t revision,
    const std::vector<uint32_t> &packed_edits,
    const std::shared_ptr<const SnapshotAnalysis> &old_analysis,
    const std::shared_ptr<const SnapshotAnalysis> &new_analysis) {
  if (!state || state->destroyed)
    return;

  std::vector<DisplayViewState::LineIdentity> candidates =
      state->pending_line_identities.empty()
          ? state->line_identities
          : std::move(state->pending_line_identities);
  std::vector<RevisionEditBatch> batches;
  if (old_analysis && new_analysis && !packed_edits.empty()) {
    RevisionEditBatch batch{revision > 0 ? revision - 1 : 0, revision,
                            packed_edits, old_analysis, new_analysis};
    batches.push_back(batch);
    if (state->pending_display_edits_eligible &&
        state->pending_display_edits.empty()) {
      state->pending_display_edits.push_back(std::move(batch));
    } else {
      state->pending_display_edits.clear();
      state->pending_display_edits_eligible = false;
    }
    for (DisplayViewState::LineIdentity &identity : candidates) {
      if (!identity.initialized)
        continue;
      uint64_t start =
          analysis_offset_for_point(*old_analysis, identity.buffer_start);
      uint64_t end =
          analysis_offset_for_point(*old_analysis, identity.buffer_end);
      if (!project_offset_range(batches, &start, &end)) {
        identity.initialized = false;
        continue;
      }
      identity.buffer_start = analysis_point_for_offset(*new_analysis, start);
      identity.buffer_end = analysis_point_for_offset(*new_analysis, end);
    }
  } else {
    state->pending_display_edits.clear();
    state->pending_display_edits_eligible = false;
  }
  state->pending_line_identities = std::move(candidates);
  state->accepted_edit_count += packed_edits.size() / 8;
  state->target_buffer_revision = revision;
  state->cached_buffer_revision = UINT64_MAX;
  if (state->highlight_buffer_revision != 0 && old_analysis && new_analysis) {
    std::vector<ScopedRange> projected;
    projected.reserve(state->highlight_ranges.size());
    for (ScopedRange scope : state->highlight_ranges) {
      if (scope.order == 0 && scope.start_offset == 0 &&
          scope.end_offset == old_analysis->utf16_length) {
        scope.end_offset = new_analysis->utf16_length;
        projected.push_back(scope);
        continue;
      }
      if (project_offset_range(batches, &scope.start_offset,
                               &scope.end_offset))
        projected.push_back(scope);
    }
    state->highlight_ranges = std::move(projected);
    state->highlight_buffer_revision = revision;
  } else if (state->highlight_buffer_revision != revision) {
    state->highlight_ranges.clear();
    state->highlight_buffer_revision = 0;
    state->highlight_syntax_revision = 0;
  }
}

Napi::Function DisplayViewWrapper::init(Napi::Env env) {
  return DefineClass(
      env, "DisplayView",
      {InstanceMethod<&DisplayViewWrapper::apply_fold_deltas>(
           "applyFoldDeltas"),
       InstanceMethod<&DisplayViewWrapper::replace_folds>("replaceFolds"),
       InstanceMethod<&DisplayViewWrapper::replace_highlight_ranges>(
           "replaceHighlightRanges"),
       InstanceMethod<&DisplayViewWrapper::buffer_to_screen>("bufferToScreen"),
       InstanceMethod<&DisplayViewWrapper::screen_to_buffer>("screenToBuffer"),
       InstanceMethod<&DisplayViewWrapper::project_buffer_ranges>(
           "projectBufferRanges"),
       InstanceMethod<&DisplayViewWrapper::get_screen_line_count>(
           "getScreenLineCount"),
       InstanceMethod<&DisplayViewWrapper::line_length_for_screen_row>(
           "lineLengthForScreenRow"),
       InstanceMethod<&DisplayViewWrapper::get_rightmost_screen_position>(
           "getRightmostScreenPosition"),
       InstanceMethod<&DisplayViewWrapper::get_indexed_summary>(
           "getIndexedSummary"),
       InstanceMethod<&DisplayViewWrapper::buffer_rows_for_screen_rows>(
           "bufferRowsForScreenRows"),
       InstanceMethod<
           &DisplayViewWrapper::highlight_shard_starts_for_screen_rows>(
           "_highlightShardStartsForScreenRows"),
       InstanceMethod<&DisplayViewWrapper::translate_screen_column_block>(
           "translateScreenColumnBlock"),
       InstanceMethod<&DisplayViewWrapper::build_render_plan>("buildRenderPlan"),
       InstanceMethod<&DisplayViewWrapper::build_render_plan_packed>(
           "buildRenderPlanPacked"),
       InstanceMethod<&DisplayViewWrapper::get_diagnostics>("getDiagnostics"),
       InstanceMethod<&DisplayViewWrapper::destroy>("destroy")});
}

Napi::Object DisplayViewWrapper::new_instance(
    Napi::Env env, std::shared_ptr<SessionState> session, Napi::Value options) {
  auto *data = env.GetInstanceData<AddonData>();
  auto *holder = new std::shared_ptr<SessionState>(std::move(session));
  Napi::External<std::shared_ptr<SessionState>> external =
      Napi::External<std::shared_ptr<SessionState>>::New(
          env, holder,
          [](Napi::Env, std::shared_ptr<SessionState> *value) { delete value; });
  return data->display_view_constructor.New({external, options});
}

DisplayViewWrapper::DisplayViewWrapper(const Napi::CallbackInfo &info)
    : Napi::ObjectWrap<DisplayViewWrapper>(info) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !info[0].IsExternal()) {
    throw_type_error(env, "DisplayView instances are created by DocumentSession",
                     "ERR_DISPLAY_VIEW_CONSTRUCTION");
    return;
  }
  auto *holder =
      info[0].As<Napi::External<std::shared_ptr<SessionState>>>().Data();
  state_ = std::make_shared<DisplayViewState>();
  state_->session = *holder;

  DisplayIndexOptions display_options;

  if (info.Length() > 1 && !info[1].IsUndefined()) {
    if (!info[1].IsObject()) {
      throw_type_error(env, "DisplayView options must be an object",
                       "ERR_INVALID_DISPLAY_OPTIONS");
      return;
    }
    Napi::Object options = info[1].As<Napi::Object>();
    Napi::Value wrap = options.Get("wrapColumn");
    Napi::Value tab = options.Get("tabLength");
    if (!wrap.IsUndefined() && !read_uint32(wrap, &display_options.wrap_column)) {
      throw_type_error(env, "wrapColumn must be a uint32 integer",
                       "ERR_INVALID_DISPLAY_OPTIONS");
      return;
    }
    if (!tab.IsUndefined() &&
        (!read_uint32(tab, &display_options.tab_length) ||
         display_options.tab_length == 0)) {
      throw_type_error(env, "tabLength must be a positive uint32 integer",
                       "ERR_INVALID_DISPLAY_OPTIONS");
      return;
    }
    Napi::Value hanging_indent = options.Get("softWrapHangingIndent");
    if (!hanging_indent.IsUndefined() &&
        !read_uint32(hanging_indent,
                     &display_options.soft_wrap_hanging_indent)) {
      throw_type_error(env, "softWrapHangingIndent must be a uint32 integer",
                       "ERR_INVALID_DISPLAY_OPTIONS");
      return;
    }
    Napi::Value atomic_soft_tabs = options.Get("atomicSoftTabs");
    if (!atomic_soft_tabs.IsUndefined()) {
      if (!atomic_soft_tabs.IsBoolean()) {
        throw_type_error(env, "atomicSoftTabs must be a boolean",
                         "ERR_INVALID_DISPLAY_OPTIONS");
        return;
      }
      display_options.atomic_soft_tabs =
          atomic_soft_tabs.As<Napi::Boolean>().Value();
    }
    Napi::Value boundary_mode = options.Get("wrapBoundaryMode");
    if (!boundary_mode.IsUndefined()) {
      if (!boundary_mode.IsString()) {
        throw_type_error(env, "wrapBoundaryMode must be a string",
                         "ERR_INVALID_DISPLAY_OPTIONS");
        return;
      }
      const std::string mode = boundary_mode.As<Napi::String>();
      if (mode == "none")
        display_options.wrap_boundary_mode = WrapBoundaryMode::None;
      else if (mode == "word")
        display_options.wrap_boundary_mode = WrapBoundaryMode::Word;
      else if (mode == "standard")
        display_options.wrap_boundary_mode = WrapBoundaryMode::Standard;
      else {
        throw_type_error(env,
                         "wrapBoundaryMode must be none, word, or standard",
                         "ERR_INVALID_DISPLAY_OPTIONS");
        return;
      }
    }
    Napi::Value fold_character = options.Get("foldCharacter");
    if (!fold_character.IsUndefined()) {
      if (!fold_character.IsString() ||
          fold_character.As<Napi::String>().Utf16Value().empty()) {
        throw_type_error(env, "foldCharacter must be a non-empty string",
                         "ERR_INVALID_DISPLAY_OPTIONS");
        return;
      }
      display_options.fold_character =
          fold_character.As<Napi::String>().Utf16Value();
    }
    Napi::Value widths = options.Get("characterWidthProfile");
    if (!widths.IsUndefined()) {
      if (!widths.IsObject()) {
        throw_type_error(env, "characterWidthProfile must be an object",
                         "ERR_INVALID_DISPLAY_OPTIONS");
        return;
      }
      Napi::Object profile = widths.As<Napi::Object>();
      struct WidthField {
        const char *name;
        double *target;
      };
      const WidthField fields[] = {
          {"default", &display_options.character_widths.default_ratio},
          {"doubleWidth",
           &display_options.character_widths.double_width_ratio},
          {"halfWidth", &display_options.character_widths.half_width_ratio},
          {"korean", &display_options.character_widths.korean_ratio}};
      for (const WidthField &field : fields) {
        Napi::Value value = profile.Get(field.name);
        if (!value.IsUndefined() && !read_positive_number(value, field.target)) {
          throw_type_error(env,
                           "Character width ratios must be positive numbers",
                           "ERR_INVALID_DISPLAY_OPTIONS");
          return;
        }
      }
    }

    Napi::Value invisibles_value = options.Get("invisibles");
    if (!invisibles_value.IsUndefined()) {
      if (!invisibles_value.IsObject()) {
        throw_type_error(env, "invisibles must be an object",
                         "ERR_INVALID_DISPLAY_OPTIONS");
        return;
      }
      Napi::Object invisibles = invisibles_value.As<Napi::Object>();
      struct InvisibleField {
        const char *name;
        std::u16string *target;
      };
      const InvisibleField fields[] = {
          {"tab", &state_->render_style.invisible_tab},
          {"space", &state_->render_style.invisible_space},
          {"eol", &state_->render_style.invisible_eol},
          {"cr", &state_->render_style.invisible_cr}};
      for (const InvisibleField &field : fields) {
        Napi::Value value = invisibles.Get(field.name);
        if (value.IsUndefined() || value.IsNull())
          continue;
        if (!value.IsString()) {
          throw_type_error(env, "Invisible characters must be strings",
                           "ERR_INVALID_DISPLAY_OPTIONS");
          return;
        }
        *field.target = value.As<Napi::String>().Utf16Value();
      }
    }

    Napi::Value built_in_value = options.Get("builtInScopeIds");
    if (!built_in_value.IsUndefined()) {
      if (!built_in_value.IsObject()) {
        throw_type_error(env, "builtInScopeIds must be an object",
                         "ERR_INVALID_DISPLAY_OPTIONS");
        return;
      }
      Napi::Object built_in = built_in_value.As<Napi::Object>();
      Napi::Array names = built_in.GetPropertyNames();
      for (uint32_t index = 0; index < names.Length(); index++) {
        Napi::Value name = names.Get(index);
        uint32_t flags = 0;
        uint32_t scope_id = 0;
        const std::string flag_text = name.ToString().Utf8Value();
        char *flag_end = nullptr;
        const unsigned long parsed_flags =
            std::strtoul(flag_text.c_str(), &flag_end, 10);
        if (flag_end == flag_text.c_str() || *flag_end != '\0' ||
            parsed_flags > UINT32_MAX) {
          throw_type_error(env, "builtInScopeIds keys must be uint32 flags",
                           "ERR_INVALID_DISPLAY_OPTIONS");
          return;
        }
        flags = static_cast<uint32_t>(parsed_flags);
        if (!read_uint32(built_in.Get(name), &scope_id) || scope_id == 0 ||
            scope_id > static_cast<uint32_t>(INT32_MAX)) {
          throw_type_error(env,
                           "builtInScopeIds must map uint32 flags to positive int32 ids",
                           "ERR_INVALID_DISPLAY_OPTIONS");
          return;
        }
        state_->render_style.built_in_scope_ids[flags] = scope_id;
      }
    }
  }
  state_->wrap_column = display_options.wrap_column;
  state_->tab_length = display_options.tab_length;
  state_->index = std::make_unique<DisplayIndex>(std::move(display_options));
}

DisplayViewWrapper::~DisplayViewWrapper() {
  if (state_)
    state_->destroyed = true;
}

std::shared_ptr<DisplayViewState> DisplayViewWrapper::state() const {
  return state_;
}

bool DisplayViewWrapper::replace_folds_for_revision(
    Napi::Env env, uint64_t buffer_revision, uint64_t generation,
    const Napi::Value &packed_ranges) {
  if (!state_ || state_->destroyed) {
    throw_error(env, "DisplayView was destroyed", "ERR_DISPLAY_VIEW_DESTROYED");
    return false;
  }
  const auto started_at = std::chrono::steady_clock::now();
  std::unordered_map<uint32_t, Fold> replacement;
  if (!parse_fold_ranges(env, packed_ranges, &replacement))
    return false;
  state_->folds = std::move(replacement);
  state_->fold_generation = generation;
  state_->target_buffer_revision = buffer_revision;
  state_->cached_buffer_revision = UINT64_MAX;
  state_->display_revision++;
  state_->fold_reset_count++;
  state_->fold_reset_milliseconds +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started_at)
          .count();
  return true;
}

Napi::Value DisplayViewWrapper::replace_folds(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3)
    return throw_type_error(env,
                            "replaceFolds expects revision, generation and ranges",
                            "ERR_INVALID_FOLD_RANGES");
  uint64_t revision = 0;
  uint64_t generation = 0;
  if (!read_uint64(info[0], &revision) || !read_uint64(info[1], &generation))
    return throw_type_error(env, "Fold revision and generation must be integers",
                            "ERR_INVALID_FOLD_RANGES");
  uint64_t current_revision = 0;
  {
    std::lock_guard<std::mutex> lock(state_->session->mutex);
    current_revision = state_->session->buffer_revision;
  }
  if (revision != current_revision)
    return throw_error(env, "Fold reset revision does not match the document",
                       "ERR_FOLD_REVISION_MISMATCH");
  if (!replace_folds_for_revision(env, revision, generation, info[2]))
    return env.Undefined();
  return env.Undefined();
}

Napi::Value DisplayViewWrapper::replace_highlight_ranges(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!state_ || state_->destroyed)
    return throw_error(env, "DisplayView was destroyed",
                       "ERR_DISPLAY_VIEW_DESTROYED");
  if (info.Length() < 3 || !is_uint32_array(info[2]))
    return throw_type_error(
        env,
        "replaceHighlightRanges expects buffer revision, syntax revision and packed ranges",
        "ERR_INVALID_HIGHLIGHT_RANGES");
  uint64_t buffer_revision = 0;
  uint64_t syntax_revision = 0;
  if (!read_uint64(info[0], &buffer_revision) ||
      !read_uint64(info[1], &syntax_revision))
    return throw_type_error(env, "Highlight revisions must be integers",
                            "ERR_INVALID_HIGHLIGHT_RANGES");
  Napi::Uint32Array packed = info[2].As<Napi::Uint32Array>();
  if (packed.ElementLength() % 5 != 0)
    return throw_type_error(env, "Highlight ranges must use stride 5",
                            "ERR_INVALID_HIGHLIGHT_RANGES");

  uint64_t current_buffer_revision = 0;
  uint64_t current_syntax_revision = 0;
  {
    std::lock_guard<std::mutex> lock(state_->session->mutex);
    current_buffer_revision = state_->session->buffer_revision;
    current_syntax_revision = state_->session->syntax_revision;
  }
  if (buffer_revision != current_buffer_revision ||
      syntax_revision != current_syntax_revision)
    return throw_error(env, "Highlight ranges do not match the document",
                       "ERR_HIGHLIGHT_REVISION_MISMATCH");

  std::vector<ScopedRange> ranges;
  ranges.reserve(packed.ElementLength() / 5);
  for (size_t index = 0; index < packed.ElementLength(); index += 5) {
    const uint32_t scope_id = packed[index];
    const uint32_t start = packed[index + 1];
    const uint32_t end = packed[index + 2];
    if (scope_id == 0 || scope_id > static_cast<uint32_t>(INT32_MAX) ||
        start >= end)
      continue;
    ranges.push_back(ScopedRange{scope_id, start, end, packed[index + 3],
                                 packed[index + 4]});
  }
  state_->highlight_ranges = std::move(ranges);
  state_->highlight_buffer_revision = buffer_revision;
  state_->highlight_syntax_revision = syntax_revision;
  state_->display_revision++;
  return env.Undefined();
}

Napi::Value DisplayViewWrapper::apply_fold_deltas(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (!state_ || state_->destroyed)
    return throw_error(env, "DisplayView was destroyed",
                       "ERR_DISPLAY_VIEW_DESTROYED");
  if (info.Length() < 4 || !info[3].IsObject())
    return throw_type_error(env,
                            "applyFoldDeltas expects revision, generations and operations",
                            "ERR_INVALID_FOLD_DELTAS");

  uint64_t revision = 0;
  uint64_t from_generation = 0;
  uint64_t to_generation = 0;
  if (!read_uint64(info[0], &revision) ||
      !read_uint64(info[1], &from_generation) ||
      !read_uint64(info[2], &to_generation))
    return throw_type_error(env, "Fold revision and generations must be integers",
                            "ERR_INVALID_FOLD_DELTAS");
  uint64_t current_revision = 0;
  {
    std::lock_guard<std::mutex> lock(state_->session->mutex);
    current_revision = state_->session->buffer_revision;
  }
  if (revision != current_revision)
    return throw_error(env, "Fold delta revision does not match the document",
                       "ERR_FOLD_REVISION_MISMATCH");
  if (from_generation != state_->fold_generation)
    return throw_error(env, "Fold generation does not match the DisplayView",
                       "ERR_FOLD_GENERATION_MISMATCH");
  if (to_generation <= from_generation)
    return throw_error(env, "Fold generation must increase",
                       "ERR_FOLD_GENERATION_MISMATCH");

  Napi::Object operations = info[3].As<Napi::Object>();
  Napi::Value upserts_value = operations.Get("upserts");
  Napi::Value removals_value = operations.Get("removals");
  Napi::Value splices_value = operations.Get("splices");
  if (!splices_value.IsUndefined() &&
      (!is_uint32_array(splices_value) ||
       splices_value.As<Napi::Uint32Array>().ElementLength() % 6 != 0))
    return throw_type_error(env, "Fold splices must use Uint32Array stride 6",
                            "ERR_INVALID_FOLD_DELTAS");
  if (!removals_value.IsUndefined() && !is_uint32_array(removals_value))
    return throw_type_error(env, "Fold removals must be a Uint32Array",
                            "ERR_INVALID_FOLD_DELTAS");

  // Validate and decode every input before mutating the live projection. This
  // preserves the all-or-nothing error contract without cloning the complete
  // fold map for every one-fold upsert (which made a sequence of N folds
  // quadratic).
  std::unordered_map<uint32_t, Fold> upserts;
  if (!upserts_value.IsUndefined() &&
      !parse_fold_ranges(env, upserts_value, &upserts))
    return env.Undefined();

  const auto started_at = std::chrono::steady_clock::now();
  if (!splices_value.IsUndefined()) {
    Napi::Uint32Array splices = splices_value.As<Napi::Uint32Array>();
    for (size_t index = 0; index < splices.ElementLength(); index += 6) {
      splice_folds(state_->folds, Point{splices[index], splices[index + 1]},
                   Point{splices[index + 2], splices[index + 3]},
                   Point{splices[index + 4], splices[index + 5]});
    }
  }
  if (!removals_value.IsUndefined()) {
    Napi::Uint32Array removals = removals_value.As<Napi::Uint32Array>();
    for (size_t index = 0; index < removals.ElementLength(); index++)
      state_->folds.erase(removals[index]);
  }
  for (auto &[id, fold] : upserts)
    state_->folds[id] = fold;

  state_->fold_generation = to_generation;
  state_->target_buffer_revision = revision;
  state_->cached_buffer_revision = UINT64_MAX;
  state_->display_revision++;
  const double elapsed =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started_at)
          .count();
  state_->fold_delta_count++;
  state_->fold_delta_milliseconds += elapsed;
  state_->fold_delta_maximum_milliseconds =
      std::max(state_->fold_delta_maximum_milliseconds, elapsed);
  return env.Undefined();
}

bool DisplayViewWrapper::ensure_index(
    Napi::Env env, const SuperstringSnapshotLease **lease,
    std::shared_ptr<const SnapshotAnalysis> *analysis) {
  if (!state_ || state_->destroyed) {
    throw_error(env, "DisplayView was destroyed", "ERR_DISPLAY_VIEW_DESTROYED");
    return false;
  }
  uint64_t revision = 0;
  {
    std::lock_guard<std::mutex> lock(state_->session->mutex);
    if (state_->session->destroyed || state_->session->current_lease == nullptr ||
        !state_->session->analysis) {
      throw_error(env, "DocumentSession has no published revision",
                  "ERR_DOCUMENT_NOT_READY");
      return false;
    }
    revision = state_->session->buffer_revision;
    *lease = state_->session->current_lease;
    *analysis = state_->session->analysis;
  }
  if (state_->target_buffer_revision != 0 &&
      state_->target_buffer_revision != revision) {
    throw_error(env, "Fold projection and document revisions do not match",
                "ERR_FOLD_REVISION_MISMATCH");
    return false;
  }

  if (state_->cached_buffer_revision != revision ||
      state_->cached_fold_generation != state_->fold_generation) {
    const auto rebuild_started_at = std::chrono::steady_clock::now();
    SnapshotReader reader(*lease);
    if (!reader.valid()) {
      throw_error(env, reader.error().c_str(), "ERR_SNAPSHOT_LEASE");
      return false;
    }
    DisplayIndexUpdateDiagnostics update_diagnostics;
    const bool had_cached_index = state_->index_initialized;
    const bool incrementally_updated =
        had_cached_index && state_->pending_display_edits_eligible &&
        state_->index->update(reader, *analysis, state_->folds,
                              state_->pending_display_edits,
                              &update_diagnostics);
    if (!reader.valid()) {
      throw_error(env, reader.error().c_str(), "ERR_SNAPSHOT_LEASE");
      return false;
    }
    if (!incrementally_updated &&
        !state_->index->rebuild(reader, *analysis, state_->folds)) {
      throw_error(env, reader.error().c_str(), "ERR_SNAPSHOT_LEASE");
      return false;
    }
    if (!reader.valid()) {
      throw_error(env, reader.error().c_str(), "ERR_SNAPSHOT_LEASE");
      return false;
    }
    std::vector<DisplayViewState::LineIdentity> candidates =
        state_->pending_line_identities.empty()
            ? state_->line_identities
            : std::move(state_->pending_line_identities);
    state_->pending_line_identities.clear();
    state_->pending_display_edits.clear();
    state_->pending_display_edits_eligible = true;
    const uint64_t screen_row_count = state_->index->row_count();
    const uint64_t local_start = update_diagnostics.replaced_screen_row_start;
    const uint64_t local_count = update_diagnostics.replaced_screen_row_count;
    if (incrementally_updated && update_diagnostics.updated_in_place &&
        candidates.size() == screen_row_count && local_count > 0 &&
        local_start <= screen_row_count &&
        local_count <= screen_row_count - local_start) {
      std::vector<DisplayViewState::LineIdentity> local_candidates(
          candidates.begin() + static_cast<size_t>(local_start),
          candidates.begin() + static_cast<size_t>(local_start + local_count));
      std::unordered_multimap<LineRangeKey, DisplayViewState::LineIdentity,
                              LineRangeKeyHash>
          candidates_by_range;
      for (const DisplayViewState::LineIdentity &identity : local_candidates) {
        if (identity.initialized)
          candidates_by_range.emplace(
              LineRangeKey{identity.buffer_start, identity.buffer_end}, identity);
      }
      std::vector<DisplayViewState::LineIdentity> reconciled(local_count);
      std::unordered_set<uint64_t> reconciled_ids;
      for (uint64_t offset = 0; offset < local_count; offset++) {
        const ScreenRow *screen_row = state_->index->row(local_start + offset);
        if (screen_row == nullptr)
          continue;
        const LineRangeKey key{screen_row->buffer_start, screen_row->buffer_end};
        auto range = candidates_by_range.equal_range(key);
        if (range.first != range.second) {
          reconciled[offset] = range.first->second;
          reconciled_ids.insert(range.first->second.id);
          candidates_by_range.erase(range.first);
        }
      }
      for (uint64_t offset = 0; offset < local_count; offset++) {
        if (reconciled[offset].initialized)
          continue;
        const ScreenRow *screen_row = state_->index->row(local_start + offset);
        const DisplayViewState::LineIdentity &candidate =
            local_candidates[offset];
        if (screen_row == nullptr || !candidate.initialized ||
            reconciled_ids.contains(candidate.id) ||
            candidate.buffer_start.row != candidate.buffer_end.row ||
            candidate.buffer_start.row != screen_row->buffer_start.row ||
            candidate.buffer_end.row != screen_row->buffer_end.row)
          continue;
        reconciled[offset] = candidate;
        reconciled[offset].buffer_start = screen_row->buffer_start;
        reconciled[offset].buffer_end = screen_row->buffer_end;
        reconciled_ids.insert(candidate.id);
      }
      for (uint64_t offset = 0; offset < local_count; offset++)
        candidates[local_start + offset] = std::move(reconciled[offset]);
      state_->line_identities = std::move(candidates);
    } else {
      std::unordered_multimap<LineRangeKey, DisplayViewState::LineIdentity,
                              LineRangeKeyHash>
          candidates_by_range;
      for (const DisplayViewState::LineIdentity &identity : candidates) {
        if (identity.initialized)
          candidates_by_range.emplace(
              LineRangeKey{identity.buffer_start, identity.buffer_end}, identity);
      }
      std::vector<DisplayViewState::LineIdentity> reconciled(screen_row_count);
      std::unordered_set<uint64_t> reconciled_ids;
      for (uint64_t row = 0; row < screen_row_count; row++) {
        const ScreenRow *screen_row = state_->index->row(row);
        if (screen_row == nullptr)
          continue;
        const LineRangeKey key{screen_row->buffer_start, screen_row->buffer_end};
        auto range = candidates_by_range.equal_range(key);
        if (range.first != range.second) {
          reconciled[row] = range.first->second;
          reconciled_ids.insert(range.first->second.id);
          candidates_by_range.erase(range.first);
        }
      }
      // A same-row edit can move every later soft-wrap boundary by one column,
      // so projected buffer ranges no longer match even when a rendered tail
      // segment is byte-for-byte unchanged. Preserve an unmatched identity at
      // the same screen-row index only within the same logical buffer row. The
      // render fingerprint remains authoritative: build_render_plan assigns a
      // fresh id before exposing any line whose text, tags, spans or wrap indent
      // changed, so this fallback cannot reuse stale rendered content.
      for (uint64_t row = 0; row < screen_row_count; row++) {
        if (reconciled[row].initialized || row >= candidates.size())
          continue;
        const ScreenRow *screen_row = state_->index->row(row);
        const DisplayViewState::LineIdentity &candidate = candidates[row];
        if (screen_row == nullptr || !candidate.initialized ||
            reconciled_ids.contains(candidate.id) ||
            candidate.buffer_start.row != candidate.buffer_end.row ||
            candidate.buffer_start.row != screen_row->buffer_start.row ||
            candidate.buffer_end.row != screen_row->buffer_end.row)
          continue;
        reconciled[row] = candidate;
        reconciled[row].buffer_start = screen_row->buffer_start;
        reconciled[row].buffer_end = screen_row->buffer_end;
        reconciled_ids.insert(candidate.id);
      }
      state_->line_identities = std::move(reconciled);
    }
    state_->cached_buffer_revision = revision;
    state_->cached_fold_generation = state_->fold_generation;
    state_->index_initialized = true;
    state_->display_revision++;
    const double elapsed =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - rebuild_started_at)
            .count();
    if (incrementally_updated) {
      state_->index_incremental_update_count++;
      if (update_diagnostics.updated_in_place)
        state_->index_in_place_update_count++;
      state_->index_incremental_update_milliseconds += elapsed;
      state_->index_incremental_rows_rebuilt +=
          update_diagnostics.screen_rows_rebuilt;
      state_->index_incremental_rows_reused +=
          update_diagnostics.screen_rows_reused;
      state_->index_incremental_layout_units_scanned +=
          update_diagnostics.layout_units_scanned;
    } else {
      state_->index_rebuild_count++;
      state_->index_rebuild_milliseconds += elapsed;
      if (had_cached_index)
        state_->index_incremental_fallback_count++;
    }
  }
  return true;
}

Napi::Value DisplayViewWrapper::buffer_to_screen(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  const SuperstringSnapshotLease *lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  if (!ensure_index(env, &lease, &analysis))
    return env.Undefined();
  SnapshotReader reader(lease);
  const ClipDirection clip =
      read_clip_direction(info.Length() > 1 ? info[1] : env.Undefined());

  if (info.Length() == 0)
    return throw_type_error(env, "bufferToScreen expects a point or Uint32Array",
                            "ERR_INVALID_POINT");
  if (is_uint32_array(info[0])) {
    Napi::Uint32Array input = info[0].As<Napi::Uint32Array>();
    if (input.ElementLength() % 2 != 0)
      return throw_type_error(env, "Packed points must use stride 2",
                              "ERR_INVALID_POINT");
    Napi::Uint32Array output = Napi::Uint32Array::New(env, input.ElementLength());
    for (size_t index = 0; index < input.ElementLength(); index += 2) {
      Point mapped =
          state_->index->buffer_to_screen(reader, Point{input[index], input[index + 1]}, clip);
      output[index] = static_cast<uint32_t>(std::min<uint64_t>(mapped.row, UINT32_MAX));
      output[index + 1] =
          static_cast<uint32_t>(std::min<uint64_t>(mapped.column, UINT32_MAX));
    }
    return output;
  }
  Point point;
  if (!read_point(info[0], &point))
    return throw_type_error(env, "Invalid buffer point", "ERR_INVALID_POINT");
  return point_to_js(env, state_->index->buffer_to_screen(reader, point, clip));
}

Napi::Value DisplayViewWrapper::screen_to_buffer(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  const SuperstringSnapshotLease *lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  if (!ensure_index(env, &lease, &analysis))
    return env.Undefined();
  SnapshotReader reader(lease);
  const ClipDirection clip =
      read_clip_direction(info.Length() > 1 ? info[1] : env.Undefined());

  if (info.Length() == 0)
    return throw_type_error(env, "screenToBuffer expects a point or Uint32Array",
                            "ERR_INVALID_POINT");
  if (is_uint32_array(info[0])) {
    Napi::Uint32Array input = info[0].As<Napi::Uint32Array>();
    if (input.ElementLength() % 2 != 0)
      return throw_type_error(env, "Packed points must use stride 2",
                              "ERR_INVALID_POINT");
    Napi::Uint32Array output = Napi::Uint32Array::New(env, input.ElementLength());
    for (size_t index = 0; index < input.ElementLength(); index += 2) {
      Point mapped =
          state_->index->screen_to_buffer(reader, Point{input[index], input[index + 1]}, clip);
      output[index] = static_cast<uint32_t>(std::min<uint64_t>(mapped.row, UINT32_MAX));
      output[index + 1] =
          static_cast<uint32_t>(std::min<uint64_t>(mapped.column, UINT32_MAX));
    }
    return output;
  }
  Point point;
  if (!read_point(info[0], &point))
    return throw_type_error(env, "Invalid screen point", "ERR_INVALID_POINT");
  return point_to_js(env, state_->index->screen_to_buffer(reader, point, clip));
}

Napi::Value DisplayViewWrapper::project_buffer_ranges(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() == 0 || !is_uint32_array(info[0]))
    return throw_type_error(env, "projectBufferRanges expects a Uint32Array",
                            "ERR_INVALID_RANGE");
  Napi::Uint32Array input = info[0].As<Napi::Uint32Array>();
  if (input.ElementLength() % 4 != 0)
    return throw_type_error(env, "Packed ranges must use stride 4",
                            "ERR_INVALID_RANGE");

  const SuperstringSnapshotLease *lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  if (!ensure_index(env, &lease, &analysis))
    return env.Undefined();
  SnapshotReader reader(lease);
  Napi::Uint32Array output = Napi::Uint32Array::New(env, input.ElementLength());
  for (size_t index = 0; index < input.ElementLength(); index += 4) {
    Point start = state_->index->buffer_to_screen(
        reader, Point{input[index], input[index + 1]}, ClipDirection::Backward);
    Point end = state_->index->buffer_to_screen(
        reader, Point{input[index + 2], input[index + 3]}, ClipDirection::Forward);
    output[index] = static_cast<uint32_t>(std::min<uint64_t>(start.row, UINT32_MAX));
    output[index + 1] =
        static_cast<uint32_t>(std::min<uint64_t>(start.column, UINT32_MAX));
    output[index + 2] = static_cast<uint32_t>(std::min<uint64_t>(end.row, UINT32_MAX));
    output[index + 3] =
        static_cast<uint32_t>(std::min<uint64_t>(end.column, UINT32_MAX));
  }
  return output;
}

Napi::Value DisplayViewWrapper::get_screen_line_count(
    const Napi::CallbackInfo &info) {
  const SuperstringSnapshotLease *lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  if (!ensure_index(info.Env(), &lease, &analysis))
    return info.Env().Undefined();
  return Napi::Number::New(
      info.Env(), static_cast<double>(state_->index->row_count()));
}

Napi::Value DisplayViewWrapper::line_length_for_screen_row(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  uint64_t row = 0;
  if (info.Length() == 0 || !read_uint64(info[0], &row))
    return throw_type_error(env, "lineLengthForScreenRow expects a row",
                            "ERR_INVALID_VIEWPORT");
  const SuperstringSnapshotLease *lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  if (!ensure_index(env, &lease, &analysis))
    return env.Undefined();
  const ScreenRow *screen_row = state_->index->row(row);
  return screen_row == nullptr
             ? env.Undefined()
             : Napi::Value(Napi::Number::New(
                   env, static_cast<double>(screen_row->visual_width)));
}

Napi::Value DisplayViewWrapper::get_rightmost_screen_position(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  const SuperstringSnapshotLease *lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  if (!ensure_index(env, &lease, &analysis))
    return env.Undefined();
  Point rightmost{};
  for (uint64_t row = 0; row < state_->index->row_count(); row++) {
    const ScreenRow *candidate = state_->index->row(row);
    if (candidate != nullptr && candidate->visual_width > rightmost.column)
      rightmost = Point{row, candidate->visual_width};
  }
  return point_to_js(env, rightmost);
}

Napi::Value
DisplayViewWrapper::get_indexed_summary(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  uint64_t buffer_row_count = 0;
  if (info.Length() == 0 || !read_uint64(info[0], &buffer_row_count))
    return throw_type_error(env, "getIndexedSummary expects a buffer row count",
                            "ERR_INVALID_VIEWPORT");
  const SuperstringSnapshotLease *lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  if (!ensure_index(env, &lease, &analysis))
    return env.Undefined();
  const IndexedDisplaySummary summary =
      state_->index->indexed_summary(buffer_row_count);
  Napi::Object result = Napi::Object::New(env);
  result.Set("screenLineCount",
             Napi::Number::New(env,
                               static_cast<double>(summary.screen_row_count)));
  result.Set("rightmostScreenPosition",
             point_to_js(env, summary.rightmost_screen_position));
  return result;
}

Napi::Value DisplayViewWrapper::buffer_rows_for_screen_rows(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  uint64_t start = 0;
  uint64_t end = 0;
  if (info.Length() < 2 || !read_uint64(info[0], &start) ||
      !read_uint64(info[1], &end) || end < start)
    return throw_type_error(env,
                            "bufferRowsForScreenRows expects an ordered range",
                            "ERR_INVALID_VIEWPORT");
  const SuperstringSnapshotLease *lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  if (!ensure_index(env, &lease, &analysis))
    return env.Undefined();
  SnapshotReader reader(lease);
  Napi::Uint32Array rows =
      Napi::Uint32Array::New(env, static_cast<size_t>(end - start));
  for (uint64_t row = start; row < end; row++) {
    if (row < state_->index->row_count()) {
      const Point point = state_->index->screen_to_buffer(
          reader, Point{row, 0}, ClipDirection::Closest);
      rows[row - start] =
          static_cast<uint32_t>(std::min<uint64_t>(point.row, UINT32_MAX));
    } else {
      const uint64_t buffer_row =
          analysis->line_starts.size() + (row - state_->index->row_count());
      rows[row - start] =
          static_cast<uint32_t>(std::min<uint64_t>(buffer_row, UINT32_MAX));
    }
  }
  return rows;
}

Napi::Value DisplayViewWrapper::highlight_shard_starts_for_screen_rows(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  uint64_t start = 0;
  uint64_t end = 0;
  if (info.Length() < 2 || !read_uint64(info[0], &start) ||
      !read_uint64(info[1], &end) || end < start)
    return throw_type_error(
        env, "_highlightShardStartsForScreenRows expects an ordered range",
        "ERR_INVALID_VIEWPORT");
  const SuperstringSnapshotLease *lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  if (!ensure_index(env, &lease, &analysis))
    return env.Undefined();
  end = std::min<uint64_t>(end, state_->index->row_count());
  start = std::min<uint64_t>(start, end);
  std::set<uint32_t> starts;
  auto include = [&](uint64_t row) {
    starts.insert(highlight_shard_start(static_cast<uint32_t>(
        std::min<uint64_t>(row, UINT32_MAX))));
  };
  for (uint64_t row = start; row < end; row++) {
    const ScreenRow *screen_row = state_->index->row(row);
    if (screen_row == nullptr)
      continue;
    include(screen_row->buffer_start.row);
    include(screen_row->buffer_end.row);
    for (const DisplaySpan &span : screen_row->spans) {
      include(span.start.row);
      include(span.end.row);
    }
  }
  Napi::Uint32Array result = Napi::Uint32Array::New(env, starts.size());
  uint32_t index = 0;
  for (uint32_t shard : starts)
    result[index++] = shard;
  return result;
}

Napi::Value DisplayViewWrapper::translate_screen_column_block(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  uint64_t start_row = 0;
  uint64_t end_row = 0;
  uint64_t start_column = 0;
  uint64_t end_column = 0;
  if (info.Length() < 4 || !read_uint64(info[0], &start_row) ||
      !read_uint64(info[1], &end_row) ||
      !read_uint64(info[2], &start_column) ||
      !read_uint64(info[3], &end_column) || end_row < start_row)
    return throw_type_error(
        env,
        "translateScreenColumnBlock expects rows and non-negative columns",
        "ERR_INVALID_VIEWPORT");
  const SuperstringSnapshotLease *lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  if (!ensure_index(env, &lease, &analysis))
    return env.Undefined();
  SnapshotReader reader(lease);
  if (state_->index->row_count() == 0)
    return Napi::Uint32Array::New(env, 0);
  Napi::Uint32Array output = Napi::Uint32Array::New(
      env, static_cast<size_t>((end_row - start_row + 1) * 5));
  size_t offset = 0;
  for (uint64_t row = start_row; row <= end_row; row++) {
    const Point start = state_->index->screen_to_buffer(
        reader, Point{row, start_column}, ClipDirection::Closest);
    const Point end = state_->index->screen_to_buffer(
        reader, Point{row, end_column}, ClipDirection::Closest);
    output[offset++] = static_cast<uint32_t>(start.row);
    output[offset++] = static_cast<uint32_t>(start.column);
    output[offset++] = static_cast<uint32_t>(end.row);
    output[offset++] = static_cast<uint32_t>(end.column);
    if (start == end) {
      const Point round_trip = state_->index->buffer_to_screen(
          reader, start, ClipDirection::Closest);
      output[offset++] = static_cast<uint32_t>(std::min<uint64_t>(
          round_trip.column, UINT32_MAX - UINT64_C(1)));
    } else {
      output[offset++] = UINT32_MAX;
    }
  }
  return output;
}

bool DisplayViewWrapper::collect_render_plan(Napi::Env env, uint64_t start,
                                             uint64_t end,
                                             RenderPlanData *plan) {
  if (plan == nullptr)
    return false;
  const SuperstringSnapshotLease *lease = nullptr;
  std::shared_ptr<const SnapshotAnalysis> analysis;
  if (!ensure_index(env, &lease, &analysis))
    return false;
  SnapshotReader reader(lease);
  end = std::min<uint64_t>(end, state_->index->row_count());
  start = std::min<uint64_t>(start, end);

  plan->lines.clear();
  plan->lines.reserve(static_cast<size_t>(end - start));
  uint64_t copied = 0;
  plan->indexed_buffer_row_count = 0;
  uint32_t highlight_start_row = 0;
  uint32_t highlight_end_row = 0;
  std::vector<uint32_t> highlight_shards;
  std::unordered_set<uint32_t> seen_highlight_shards;
  auto include_highlight_row = [&](uint64_t raw_row) {
    const uint32_t row = static_cast<uint32_t>(
        std::min<uint64_t>(raw_row, UINT32_MAX));
    const uint32_t shard = highlight_shard_start(row);
    if (seen_highlight_shards.insert(shard).second)
      highlight_shards.push_back(shard);
  };
  if (start < end) {
    if (const ScreenRow *first = state_->index->row(start)) {
      highlight_start_row = static_cast<uint32_t>(std::min<uint64_t>(
          first->buffer_start.row, UINT32_MAX));
    }
    for (uint64_t row = start; row < end; row++) {
      if (const ScreenRow *screen_row = state_->index->row(row)) {
        include_highlight_row(screen_row->buffer_start.row);
        include_highlight_row(screen_row->buffer_end.row);
        for (const DisplaySpan &span : screen_row->spans) {
          include_highlight_row(span.start.row);
          include_highlight_row(span.end.row);
        }
        const uint64_t last_buffer_row =
            std::max(screen_row->buffer_start.row,
                     screen_row->buffer_end.row);
        highlight_end_row = static_cast<uint32_t>(std::min<uint64_t>(
            last_buffer_row + 1, UINT32_MAX));
      }
    }
  }
  if (start < end && highlight_end_row < analysis->line_starts.size())
    highlight_shards.push_back(highlight_shard_start(highlight_end_row));
  std::sort(highlight_shards.begin(), highlight_shards.end());
  highlight_shards.erase(
      std::unique(highlight_shards.begin(), highlight_shards.end()),
      highlight_shards.end());
  std::vector<ScopedRange> shared_highlights;
  const std::vector<ScopedRange> *highlights = &state_->highlight_ranges;
  uint64_t highlight_syntax_revision = state_->highlight_syntax_revision;
  {
    std::lock_guard<std::mutex> lock(state_->session->mutex);
    const HighlightIndexTags tags = state_->session->highlight_index->tags();
    const InjectionEngineDiagnostics injections =
        state_->session->injection_engine->diagnostics();
    const bool tags_current = state_->session->highlight_index->active_for(tags) &&
        tags.buffer_revision == state_->session->buffer_revision &&
        tags.syntax_revision == state_->session->syntax_revision &&
        tags.syntax_revision == tags.buffer_revision &&
        tags.language_generation == state_->session->language_generation &&
        tags.injection_generation == injections.topology_generation;
    plan->highlight_generation =
        state_->session->highlight_index->generation();
    if (!state_->session->async_highlight_mode &&
        state_->highlight_buffer_revision == state_->session->buffer_revision &&
        state_->highlight_syntax_revision == state_->session->syntax_revision) {
      plan->highlight_coverage_complete = true;
      plan->highlight_coverage_start_row = highlight_start_row;
      plan->highlight_coverage_end_row = highlight_end_row;
    } else if (state_->session->language_id.empty()) {
      highlights = &shared_highlights;
      highlight_syntax_revision = 0;
      plan->highlight_coverage_complete = true;
      plan->highlight_coverage_start_row = highlight_start_row;
      plan->highlight_coverage_end_row = highlight_end_row;
    } else if (state_->session->async_highlight_mode && tags_current &&
               start == end) {
      highlights = &shared_highlights;
      highlight_syntax_revision = tags.syntax_revision;
      plan->highlight_coverage_complete = true;
    } else if (state_->session->async_highlight_mode && tags_current &&
        !highlight_shards.empty() &&
        state_->session->highlight_index->collect_shards(
            highlight_shards, tags,
            shared_highlights)) {
      highlights = &shared_highlights;
      highlight_syntax_revision = tags.syntax_revision;
      plan->highlight_coverage_complete = true;
      plan->highlight_coverage_start_row = highlight_start_row;
      plan->highlight_coverage_end_row = highlight_end_row;
    } else {
      highlights = &shared_highlights;
      highlight_syntax_revision = 0;
    }
  }
  RenderPlanBuilder builder(reader, *state_->index, state_->render_style,
                            *highlights);
  for (uint64_t row = start; row < end; row++) {
    const ScreenRow *screen_row = state_->index->row(row);
    RenderedLine rendered = builder.build_line(row, copied);
    DisplayViewState::LineIdentity &identity = state_->line_identities[row];
    const uint64_t fingerprint =
        screen_row
            ? line_fingerprint(*screen_row, rendered.text, rendered.tags)
            : 0;
    if (!identity.initialized || identity.fingerprint != fingerprint) {
      identity.fingerprint = fingerprint;
      identity.id = next_line_id();
      identity.initialized = true;
    }
    if (screen_row != nullptr) {
      identity.buffer_start = screen_row->buffer_start;
      identity.buffer_end = screen_row->buffer_end;
      plan->indexed_buffer_row_count = std::max<uint64_t>(
          plan->indexed_buffer_row_count,
          std::max(screen_row->buffer_start.row, screen_row->buffer_end.row) +
              1);
    }
    plan->lines.push_back(RenderPlanLineData{
        identity.id, screen_row ? screen_row->soft_wrap_indent : 0,
        std::move(rendered)});
  }

  {
    std::lock_guard<std::mutex> lock(state_->session->mutex);
    state_->session->counters.viewport_utf16_copied += copied;
    plan->buffer_revision = state_->session->buffer_revision;
    plan->syntax_revision = highlight_syntax_revision;
  }
  plan->display_revision = state_->display_revision;
  plan->fold_generation = state_->fold_generation;
  if (end == state_->index->row_count())
    plan->indexed_buffer_row_count = analysis->line_starts.size();
  return true;
}

Napi::Value DisplayViewWrapper::build_render_plan(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  uint64_t start = 0;
  uint64_t end = 0;
  if (info.Length() < 2 || !read_uint64(info[0], &start) ||
      !read_uint64(info[1], &end) || end < start)
    return throw_type_error(env,
                            "buildRenderPlan expects an ordered screen row range",
                            "ERR_INVALID_VIEWPORT");
  const auto render_started_at = std::chrono::steady_clock::now();
  RenderPlanData data;
  if (!collect_render_plan(env, start, end, &data))
    return env.Undefined();

  Napi::Array lines = Napi::Array::New(env, data.lines.size());
  for (size_t line_index = 0; line_index < data.lines.size(); line_index++) {
    const RenderPlanLineData &source = data.lines[line_index];
    Napi::Object line = Napi::Object::New(env);
    line.Set("id", Napi::Number::New(env, static_cast<double>(source.id)));
    line.Set("lineText", Napi::String::New(env, source.rendered.text.data(),
                                           source.rendered.text.size()));
    Napi::Int32Array tags =
        Napi::Int32Array::New(env, source.rendered.tags.size());
    for (size_t tag_index = 0; tag_index < source.rendered.tags.size();
         tag_index++)
      tags[tag_index] = source.rendered.tags[tag_index];
    line.Set("tags", tags);
    line.Set("softWrapIndent", Napi::Number::New(env, source.soft_wrap_indent));
    lines.Set(static_cast<uint32_t>(line_index), line);
  }

  Napi::Object plan = Napi::Object::New(env);
  plan.Set("bufferRevision", Napi::Number::New(env, data.buffer_revision));
  plan.Set("syntaxRevision", Napi::Number::New(env, data.syntax_revision));
  plan.Set("displayRevision",
           Napi::Number::New(env, data.display_revision));
  plan.Set("foldGeneration",
           Napi::Number::New(env, data.fold_generation));
  plan.Set("highlightGeneration",
           Napi::Number::New(env, data.highlight_generation));
  plan.Set("highlightCoverageComplete",
           Napi::Boolean::New(env, data.highlight_coverage_complete));
  plan.Set("highlightCoverageStartRow",
           Napi::Number::New(env, data.highlight_coverage_start_row));
  plan.Set("highlightCoverageEndRow",
           Napi::Number::New(env, data.highlight_coverage_end_row));
  plan.Set("indexedBufferRowCount",
           Napi::Number::New(env, data.indexed_buffer_row_count));
  plan.Set("lines", lines);
  state_->render_plan_count++;
  state_->render_plan_milliseconds +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - render_started_at)
          .count();
  return plan;
}

Napi::Value DisplayViewWrapper::build_render_plan_packed(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  uint64_t start = 0;
  uint64_t end = 0;
  if (info.Length() < 2 || !read_uint64(info[0], &start) ||
      !read_uint64(info[1], &end) || end < start)
    return throw_type_error(
        env, "buildRenderPlanPacked expects an ordered screen row range",
        "ERR_INVALID_VIEWPORT");
  const auto render_started_at = std::chrono::steady_clock::now();
  RenderPlanData data;
  if (!collect_render_plan(env, start, end, &data))
    return env.Undefined();

  constexpr size_t descriptor_stride = 5;
  constexpr uint64_t max_offset = std::numeric_limits<uint32_t>::max();
  uint64_t text_length = 0;
  uint64_t tag_count = 0;
  for (const RenderPlanLineData &line : data.lines) {
    text_length += line.rendered.text.size();
    tag_count += line.rendered.tags.size();
    if (text_length > max_offset || tag_count > max_offset ||
        line.soft_wrap_indent < -1 ||
        line.soft_wrap_indent >= static_cast<int64_t>(max_offset))
      return throw_error(env, "Packed RenderPlan exceeds uint32 offsets",
                         "ERR_RENDER_PLAN_TOO_LARGE");
  }
  if (data.lines.size() >
      std::numeric_limits<size_t>::max() / descriptor_stride)
    return throw_error(env, "Packed RenderPlan has too many lines",
                       "ERR_RENDER_PLAN_TOO_LARGE");

  std::u16string text;
  text.reserve(static_cast<size_t>(text_length));
  std::vector<int32_t> packed_tags;
  packed_tags.reserve(static_cast<size_t>(tag_count));
  Napi::Float64Array line_ids =
      Napi::Float64Array::New(env, data.lines.size());
  Napi::Uint32Array descriptors = Napi::Uint32Array::New(
      env, data.lines.size() * descriptor_stride);
  size_t descriptor_offset = 0;
  for (size_t line_index = 0; line_index < data.lines.size(); line_index++) {
    const RenderPlanLineData &line = data.lines[line_index];
    line_ids[line_index] = static_cast<double>(line.id);
    descriptors[descriptor_offset++] = static_cast<uint32_t>(text.size());
    descriptors[descriptor_offset++] =
        static_cast<uint32_t>(line.rendered.text.size());
    descriptors[descriptor_offset++] =
        static_cast<uint32_t>(packed_tags.size());
    descriptors[descriptor_offset++] =
        static_cast<uint32_t>(line.rendered.tags.size());
    descriptors[descriptor_offset++] =
        line.soft_wrap_indent < 0
            ? std::numeric_limits<uint32_t>::max()
            : static_cast<uint32_t>(line.soft_wrap_indent);
    text.append(line.rendered.text);
    packed_tags.insert(packed_tags.end(), line.rendered.tags.begin(),
                       line.rendered.tags.end());
  }
  Napi::Int32Array tags = Napi::Int32Array::New(env, packed_tags.size());
  for (size_t index = 0; index < packed_tags.size(); index++)
    tags[index] = packed_tags[index];

  Napi::Object plan = Napi::Object::New(env);
  plan.Set("bufferRevision", Napi::Number::New(env, data.buffer_revision));
  plan.Set("syntaxRevision", Napi::Number::New(env, data.syntax_revision));
  plan.Set("displayRevision",
           Napi::Number::New(env, data.display_revision));
  plan.Set("foldGeneration",
           Napi::Number::New(env, data.fold_generation));
  plan.Set("highlightGeneration",
           Napi::Number::New(env, data.highlight_generation));
  plan.Set("highlightCoverageComplete",
           Napi::Boolean::New(env, data.highlight_coverage_complete));
  plan.Set("highlightCoverageStartRow",
           Napi::Number::New(env, data.highlight_coverage_start_row));
  plan.Set("highlightCoverageEndRow",
           Napi::Number::New(env, data.highlight_coverage_end_row));
  plan.Set("indexedBufferRowCount",
           Napi::Number::New(env, data.indexed_buffer_row_count));
  plan.Set("text", Napi::String::New(env, text.data(), text.size()));
  plan.Set("lineIds", line_ids);
  plan.Set("lineDescriptorStride",
           Napi::Number::New(env, descriptor_stride));
  plan.Set("lineDescriptors", descriptors);
  plan.Set("tags", tags);
  state_->render_plan_count++;
  state_->render_plan_milliseconds +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - render_started_at)
          .count();
  return plan;
}

Napi::Value DisplayViewWrapper::get_diagnostics(
    const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  Napi::Object result = Napi::Object::New(env);
  result.Set("displayRevision",
             Napi::Number::New(env, state_->display_revision));
  result.Set("foldGeneration", Napi::Number::New(env, state_->fold_generation));
  result.Set("targetBufferRevision",
             Napi::Number::New(env, state_->target_buffer_revision));
  result.Set("cachedBufferRevision",
             Napi::Number::New(env, state_->cached_buffer_revision == UINT64_MAX
                                        ? 0
                                        : state_->cached_buffer_revision));
  result.Set("activeFoldCount", Napi::Number::New(env, state_->folds.size()));
  result.Set("highlightBufferRevision",
             Napi::Number::New(env, state_->highlight_buffer_revision));
  result.Set("highlightSyntaxRevision",
             Napi::Number::New(env, state_->highlight_syntax_revision));
  result.Set("acceptedEditCount",
             Napi::Number::New(env, state_->accepted_edit_count));
  result.Set("foldDeltaCount",
             Napi::Number::New(env, state_->fold_delta_count));
  result.Set("foldDeltaMilliseconds",
             Napi::Number::New(env, state_->fold_delta_milliseconds));
  result.Set("foldDeltaMaximumMilliseconds",
             Napi::Number::New(env,
                               state_->fold_delta_maximum_milliseconds));
  result.Set("foldResetCount",
             Napi::Number::New(env, state_->fold_reset_count));
  result.Set("foldResetMilliseconds",
             Napi::Number::New(env, state_->fold_reset_milliseconds));
  result.Set("indexRebuildCount",
             Napi::Number::New(env, state_->index_rebuild_count));
  result.Set("indexRebuildMilliseconds",
             Napi::Number::New(env, state_->index_rebuild_milliseconds));
  result.Set("indexIncrementalUpdateCount",
             Napi::Number::New(env,
                               state_->index_incremental_update_count));
  result.Set("indexInPlaceUpdateCount",
             Napi::Number::New(env, state_->index_in_place_update_count));
  result.Set("indexIncrementalFallbackCount",
             Napi::Number::New(env,
                               state_->index_incremental_fallback_count));
  result.Set("indexIncrementalUpdateMilliseconds",
             Napi::Number::New(
                 env, state_->index_incremental_update_milliseconds));
  result.Set("indexIncrementalRowsRebuilt",
             Napi::Number::New(env,
                               state_->index_incremental_rows_rebuilt));
  result.Set("indexIncrementalRowsReused",
             Napi::Number::New(env,
                               state_->index_incremental_rows_reused));
  result.Set("indexIncrementalLayoutUnitsScanned",
             Napi::Number::New(
                 env, state_->index_incremental_layout_units_scanned));
  result.Set("renderPlanCount",
             Napi::Number::New(env, state_->render_plan_count));
  result.Set("renderPlanMilliseconds",
             Napi::Number::New(env, state_->render_plan_milliseconds));
  const DisplayIndexDiagnostics diagnostics =
      state_->index ? state_->index->diagnostics() : DisplayIndexDiagnostics{};
  result.Set("screenRowCount",
             Napi::Number::New(env, diagnostics.screen_row_count));
  result.Set("displaySpanCount",
             Napi::Number::New(env, diagnostics.display_span_count));
  result.Set("retainedBytes",
             Napi::Number::New(env, diagnostics.retained_bytes));
  result.Set("sourceUtf16Length",
             Napi::Number::New(env, diagnostics.source_utf16_length));
  result.Set("layoutUnitsScanned",
             Napi::Number::New(env, diagnostics.layout_units_scanned));
  result.Set("peakLogicalSegments",
             Napi::Number::New(env, diagnostics.peak_logical_segments));
  result.Set("peakRowSpans",
             Napi::Number::New(env, diagnostics.peak_row_spans));
  return result;
}

Napi::Value DisplayViewWrapper::destroy(const Napi::CallbackInfo &info) {
  if (state_) {
    state_->destroyed = true;
    state_->index.reset();
    state_->folds.clear();
    state_->line_identities.clear();
  }
  return info.Env().Undefined();
}

} // namespace document_engine
