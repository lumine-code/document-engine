#include "bindings/addon-data.h"
#include "bindings/display-view.h"
#include "bindings/document-session.h"
#include "syntax/injection-engine.h"
#include "syntax/syntax-backend.h"

#include <napi.h>

namespace document_engine {

AddonData::AddonData(Napi::Env env) {
  if (napi_get_uv_event_loop(env, &loop_) != napi_ok)
    return;
  if (napi_add_async_cleanup_hook(env, cleanup_hook_callback, this,
                                  &cleanup_hook_) != napi_ok) {
    loop_ = nullptr;
    cleanup_hook_ = nullptr;
  }
}

AddonData::~AddonData() {
  if (cleanup_hook_ != nullptr &&
      !cleanup_started_.load(std::memory_order_acquire))
    napi_remove_async_cleanup_hook(cleanup_hook_);
}

void AddonData::register_session(
    const std::shared_ptr<SessionState> &session) {
  for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
    if (iterator->expired())
      iterator = sessions_.erase(iterator);
    else
      iterator++;
  }
  sessions_.push_back(session);
}

void AddonData::register_node_lease(
    uint64_t id,
    const std::shared_ptr<SnapshotLeaseReference> &lease) {
  for (auto iterator = node_leases_.begin(); iterator != node_leases_.end();) {
    if (iterator->lease.expired())
      iterator = node_leases_.erase(iterator);
    else
      iterator++;
  }
  node_leases_.push_back(NodeLeaseEntry{id, lease});
}

void AddonData::release_node_lease(uint64_t id) {
  for (auto iterator = node_leases_.begin(); iterator != node_leases_.end();) {
    if (iterator->id == id) {
      if (std::shared_ptr<SnapshotLeaseReference> lease =
              iterator->lease.lock())
        release_snapshot_lease_reference(lease);
      iterator = node_leases_.erase(iterator);
    } else if (iterator->lease.expired()) {
      iterator = node_leases_.erase(iterator);
    } else {
      iterator++;
    }
  }
}

void AddonData::notify_cleanup_progress() {
  if (cleanup_started_.load(std::memory_order_acquire) &&
      cleanup_async_initialized_.load(std::memory_order_acquire) &&
      !cleanup_async_closing_.load(std::memory_order_acquire))
    uv_async_send(&cleanup_async_);
}

void AddonData::cleanup_hook_callback(
    napi_async_cleanup_hook_handle handle, void *data) {
  static_cast<AddonData *>(data)->begin_cleanup(handle);
}

void AddonData::begin_cleanup(napi_async_cleanup_hook_handle handle) {
  cleanup_started_.store(true, std::memory_order_release);
  cleanup_hook_ = handle;
  cleanup_async_.data = this;
  if (uv_async_init(loop_, &cleanup_async_, cleanup_async_callback) != 0) {
    napi_fatal_error("document-engine", NAPI_AUTO_LENGTH,
                     "Unable to initialize async environment cleanup",
                     NAPI_AUTO_LENGTH);
    return;
  }
  cleanup_async_initialized_.store(true, std::memory_order_release);

  cleanup_sessions_.reserve(sessions_.size());
  for (const std::weak_ptr<SessionState> &weak_session : sessions_) {
    if (std::shared_ptr<SessionState> session = weak_session.lock())
      cleanup_sessions_.push_back(std::move(session));
  }
  sessions_.clear();
  for (const NodeLeaseEntry &entry : node_leases_) {
    if (std::shared_ptr<SnapshotLeaseReference> lease = entry.lease.lock())
      release_snapshot_lease_reference(lease);
  }
  node_leases_.clear();
  for (const std::shared_ptr<SessionState> &session : cleanup_sessions_)
    begin_environment_cleanup(session);
  uv_async_send(&cleanup_async_);
}

void AddonData::cleanup_async_callback(uv_async_t *handle) {
  static_cast<AddonData *>(handle->data)->poll_cleanup();
}

void AddonData::poll_cleanup() {
  for (const std::shared_ptr<SessionState> &session : cleanup_sessions_) {
    if (!environment_cleanup_complete(session))
      return;
  }
  if (cleanup_async_closing_.exchange(true, std::memory_order_acq_rel))
    return;
  uv_close(reinterpret_cast<uv_handle_t *>(&cleanup_async_),
           cleanup_async_closed);
}

void AddonData::cleanup_async_closed(uv_handle_t *handle) {
  auto *data = static_cast<AddonData *>(handle->data);
  data->cleanup_async_initialized_.store(false, std::memory_order_release);
  data->cleanup_sessions_.clear();
  napi_async_cleanup_hook_handle cleanup_hook = data->cleanup_hook_;
  data->cleanup_hook_ = nullptr;
  napi_remove_async_cleanup_hook(cleanup_hook);
}

Napi::Object init_addon(Napi::Env env, Napi::Object exports) {
  auto *data = new AddonData(env);
  if (!data->ready()) {
    delete data;
    Napi::Error::New(env, "Unable to install document cleanup hook")
        .ThrowAsJavaScriptException();
    return exports;
  }
  env.SetInstanceData<AddonData>(data);

  Napi::Function display_view = DisplayViewWrapper::init(env);
  data->display_view_constructor = Napi::Persistent(display_view);
  data->display_view_constructor.SuppressDestruct();

  Napi::Function document_session = DocumentSessionWrapper::init(env);
  data->document_session_constructor = Napi::Persistent(document_session);
  data->document_session_constructor.SuppressDestruct();

  Napi::Function injection_node = InjectionNodeWrapper::init(env);
  data->injection_node_constructor = Napi::Persistent(injection_node);
  data->injection_node_constructor.SuppressDestruct();

  Napi::Object capabilities = Napi::Object::New(env);
  capabilities.Set(
      "nativeSyntaxWasm",
      Napi::Boolean::New(env, syntax_backend_capabilities().native_wasm));
  capabilities.Set(
      "nativeQueries",
      Napi::Boolean::New(env, syntax_backend_capabilities().native_queries));
  capabilities.Set(
      "nativeGrammarCache",
      Napi::Boolean::New(
          env, syntax_backend_capabilities().native_grammar_cache));
  capabilities.Set(
      "nativeQueryCache",
      Napi::Boolean::New(
          env, syntax_backend_capabilities().native_query_cache));
  capabilities.Set("nativeLayeredQueries", Napi::Boolean::New(env, true));
  capabilities.Set("nativeQueryLanguageResolution",
                   Napi::Boolean::New(env, true));
  capabilities.Set("nativeSyntaxNodeApi", Napi::Boolean::New(env, true));
  capabilities.Set("syntaxBackend",
                   Napi::String::New(
                       env, syntax_backend_capabilities().backend));
  capabilities.Set("treeSitterVersion", Napi::String::New(env, "0.27.0"));
  capabilities.Set("wasmtimeVersion", Napi::String::New(env, "48.0.1"));
  capabilities.Set(
      "maxSyntaxUtf16Length",
      Napi::Number::New(env, static_cast<double>(MAX_SYNTAX_UTF16_LENGTH)));
  capabilities.Set(
      "grammarCacheCapacity",
      Napi::Number::New(
          env, static_cast<double>(SYNTAX_GRAMMAR_CACHE_CAPACITY)));
  capabilities.Set(
      "queryCacheCapacity",
      Napi::Number::New(env,
                        static_cast<double>(NATIVE_QUERY_CACHE_CAPACITY)));
  capabilities.Set("snapshotLeaseAbi", Napi::Number::New(env, 1));
  capabilities.Set("nativeDisplayIndex", Napi::Boolean::New(env, true));
  capabilities.Set("nativeDisplayParity", Napi::Boolean::New(env, true));
  capabilities.Set("nativeDynamicInjections", Napi::Boolean::New(env, true));
  capabilities.Set("nativeInjectionChildParsing",
                   Napi::Boolean::New(env, true));
  napi_object_freeze(env, capabilities);

  exports.Set("DocumentSession", document_session);
  exports.Set("DisplayView", display_view);
  exports.Set("capabilities", capabilities);
  return exports;
}

} // namespace document_engine

Napi::Object init_document_engine(Napi::Env env, Napi::Object exports) {
  return document_engine::init_addon(env, exports);
}

NODE_API_MODULE(document_engine, init_document_engine)
