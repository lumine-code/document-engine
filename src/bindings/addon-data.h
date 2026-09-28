#ifndef LUMINE_DOCUMENT_ENGINE_ADDON_DATA_H_
#define LUMINE_DOCUMENT_ENGINE_ADDON_DATA_H_

#include <napi.h>
#include <uv.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace document_engine {

struct SessionState;
class SnapshotLeaseReference;

class AddonData final {
public:
  explicit AddonData(Napi::Env env);
  ~AddonData();

  bool ready() const { return cleanup_hook_ != nullptr && loop_ != nullptr; }
  void register_session(const std::shared_ptr<SessionState> &session);
  void register_node_lease(
      uint64_t id,
      const std::shared_ptr<SnapshotLeaseReference> &lease);
  void release_node_lease(uint64_t id);
  void notify_cleanup_progress();

  Napi::FunctionReference document_session_constructor;
  Napi::FunctionReference display_view_constructor;
  Napi::FunctionReference injection_node_constructor;

private:
  static void cleanup_hook_callback(napi_async_cleanup_hook_handle handle,
                                    void *data);
  static void cleanup_async_callback(uv_async_t *handle);
  static void cleanup_async_closed(uv_handle_t *handle);

  void begin_cleanup(napi_async_cleanup_hook_handle handle);
  void poll_cleanup();

  uv_loop_t *loop_ = nullptr;
  uv_async_t cleanup_async_{};
  std::atomic<bool> cleanup_async_initialized_{false};
  std::atomic<bool> cleanup_async_closing_{false};
  std::atomic<bool> cleanup_started_{false};
  napi_async_cleanup_hook_handle cleanup_hook_ = nullptr;
  std::vector<std::weak_ptr<SessionState>> sessions_;
  struct NodeLeaseEntry {
    uint64_t id = 0;
    std::weak_ptr<SnapshotLeaseReference> lease;
  };
  std::vector<NodeLeaseEntry> node_leases_;
  std::vector<std::shared_ptr<SessionState>> cleanup_sessions_;
};

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_ADDON_DATA_H_
