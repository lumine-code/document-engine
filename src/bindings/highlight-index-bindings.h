#ifndef LUMINE_DOCUMENT_ENGINE_HIGHLIGHT_INDEX_BINDINGS_H_
#define LUMINE_DOCUMENT_ENGINE_HIGHLIGHT_INDEX_BINDINGS_H_

#include <napi.h>

#include <memory>

namespace document_engine {

struct SessionState;

Napi::Value request_highlight_coverage(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &state);
Napi::Value commit_highlight_coverage(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &state);
Napi::Value abort_highlight_coverage(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &state);
Napi::Value invalidate_highlight_index(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &state);
Napi::Value use_synchronous_highlights(
    const Napi::CallbackInfo &info,
    const std::shared_ptr<SessionState> &state);
void cancel_highlight_requests(Napi::Env env,
                               const std::shared_ptr<SessionState> &state,
                               const char *reason);

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_HIGHLIGHT_INDEX_BINDINGS_H_
