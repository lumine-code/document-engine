#include "testing/snapshot-lease-test-provider.h"

#include "snapshot-lease.h"

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>

namespace document_engine {

namespace {

class FaultingSnapshotLeaseState {
public:
  FaultingSnapshotLeaseState(const SuperstringSnapshotLease *upstream,
                             uint64_t fail_after)
      : upstream_(upstream), owner_thread_(std::this_thread::get_id()),
        fail_after_(fail_after) {
    lease_.abi_version = SUPERSTRING_SNAPSHOT_LEASE_ABI_VERSION;
    lease_.struct_size = sizeof(SuperstringSnapshotLease);
    lease_.functions = &functions_;
    lease_.context = this;
    lease_.utf16_length = upstream_->utf16_length;
    lease_.chunk_count = upstream_->chunk_count;
    lease_.flags = upstream_->flags;
  }

  SuperstringSnapshotLease *lease() { return &lease_; }

  void release_owner() {
    bool expected = true;
    if (owner_reference_active_.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel,
            std::memory_order_relaxed))
      release_reference();
  }

private:
  ~FaultingSnapshotLeaseState() {
    upstream_->functions->release(upstream_->context);
  }

  static SuperstringSnapshotLeaseStatus retain(void *context) {
    if (context == nullptr)
      return SUPERSTRING_SNAPSHOT_LEASE_STATUS_INVALID_ARGUMENT;
    auto *state = static_cast<FaultingSnapshotLeaseState *>(context);
    uint32_t count = state->reference_count_.load(std::memory_order_relaxed);
    while (count != 0 && count != std::numeric_limits<uint32_t>::max()) {
      if (state->reference_count_.compare_exchange_weak(
              count, count + 1, std::memory_order_acquire,
              std::memory_order_relaxed))
        return SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK;
    }
    return SUPERSTRING_SNAPSHOT_LEASE_STATUS_DESTROYED;
  }

  static SuperstringSnapshotLeaseStatus release(void *context) {
    if (context == nullptr)
      return SUPERSTRING_SNAPSHOT_LEASE_STATUS_INVALID_ARGUMENT;
    auto *state = static_cast<FaultingSnapshotLeaseState *>(context);
    if (std::this_thread::get_id() != state->owner_thread_)
      return SUPERSTRING_SNAPSHOT_LEASE_STATUS_WRONG_THREAD;
    state->release_reference();
    return SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK;
  }

  static SuperstringSnapshotLeaseStatus
  chunk_at(const void *context, uint64_t index,
           SuperstringSnapshotChunk *result) {
    if (context == nullptr || result == nullptr)
      return SUPERSTRING_SNAPSHOT_LEASE_STATUS_INVALID_ARGUMENT;
    const auto *state =
        static_cast<const FaultingSnapshotLeaseState *>(context);
    const uint64_t call =
        state->chunk_calls_.fetch_add(1, std::memory_order_relaxed);
    if (call >= state->fail_after_)
      return SUPERSTRING_SNAPSHOT_LEASE_STATUS_DESTROYED;
    return state->upstream_->functions->chunk_at(state->upstream_->context,
                                                  index, result);
  }

  static SuperstringSnapshotLeaseStatus chunk_count(const void *context,
                                                     uint64_t *result) {
    if (context == nullptr || result == nullptr)
      return SUPERSTRING_SNAPSHOT_LEASE_STATUS_INVALID_ARGUMENT;
    const auto *state =
        static_cast<const FaultingSnapshotLeaseState *>(context);
    if (superstring_snapshot_lease_has_chunk_count_function(state->upstream_))
      return state->upstream_->functions->chunk_count(
          state->upstream_->context, result);
    *result = state->upstream_->chunk_count;
    return SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK;
  }

  static SuperstringSnapshotLeaseStatus line_count(const void *context,
                                                    uint64_t *result) {
    if (context == nullptr || result == nullptr)
      return SUPERSTRING_SNAPSHOT_LEASE_STATUS_INVALID_ARGUMENT;
    const auto *state =
        static_cast<const FaultingSnapshotLeaseState *>(context);
    if (!superstring_snapshot_lease_has_line_index(state->upstream_))
      return SUPERSTRING_SNAPSHOT_LEASE_STATUS_ABI_MISMATCH;
    return state->upstream_->functions->line_count(state->upstream_->context,
                                                    result);
  }

  static SuperstringSnapshotLeaseStatus
  line_at(const void *context, uint64_t index,
          SuperstringSnapshotLine *result) {
    if (context == nullptr || result == nullptr)
      return SUPERSTRING_SNAPSHOT_LEASE_STATUS_INVALID_ARGUMENT;
    const auto *state =
        static_cast<const FaultingSnapshotLeaseState *>(context);
    if (!superstring_snapshot_lease_has_line_index(state->upstream_))
      return SUPERSTRING_SNAPSHOT_LEASE_STATUS_ABI_MISMATCH;
    return state->upstream_->functions->line_at(state->upstream_->context,
                                                 index, result);
  }

  void release_reference() {
    const uint32_t previous =
        reference_count_.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 1)
      delete this;
  }

  static const SuperstringSnapshotLeaseFunctions functions_;

  const SuperstringSnapshotLease *upstream_ = nullptr;
  std::thread::id owner_thread_;
  uint64_t fail_after_ = 0;
  mutable std::atomic<uint64_t> chunk_calls_{0};
  std::atomic<uint32_t> reference_count_{1};
  std::atomic<bool> owner_reference_active_{true};
  SuperstringSnapshotLease lease_{};
};

const SuperstringSnapshotLeaseFunctions
    FaultingSnapshotLeaseState::functions_ = {
        SUPERSTRING_SNAPSHOT_LEASE_ABI_VERSION,
        sizeof(SuperstringSnapshotLeaseFunctions),
        &FaultingSnapshotLeaseState::retain,
        &FaultingSnapshotLeaseState::release,
        &FaultingSnapshotLeaseState::chunk_at,
        &FaultingSnapshotLeaseState::line_count,
        &FaultingSnapshotLeaseState::line_at,
        &FaultingSnapshotLeaseState::chunk_count,
};

struct FaultingSnapshotOwner {
  FaultingSnapshotLeaseState *state = nullptr;
};

Napi::Value get_snapshot_lease(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  auto *owner = static_cast<FaultingSnapshotOwner *>(info.Data());
  if (owner == nullptr || owner->state == nullptr) {
    Napi::Error::New(env, "Faulting snapshot was destroyed")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::External<SuperstringSnapshotLease> external =
      Napi::External<SuperstringSnapshotLease>::New(env,
                                                    owner->state->lease());
  if (napi_type_tag_object(env, external,
                           &SUPERSTRING_SNAPSHOT_LEASE_TYPE_TAG) != napi_ok) {
    Napi::Error::New(env, "Unable to tag the faulting SnapshotLease")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return external;
}

Napi::Value destroy_snapshot(const Napi::CallbackInfo &info) {
  auto *owner = static_cast<FaultingSnapshotOwner *>(info.Data());
  if (owner != nullptr && owner->state != nullptr) {
    owner->state->release_owner();
    owner->state = nullptr;
  }
  return info.Env().Undefined();
}

void finalize_snapshot(napi_env, void *data, void *) {
  auto *owner = static_cast<FaultingSnapshotOwner *>(data);
  if (owner != nullptr) {
    if (owner->state != nullptr)
      owner->state->release_owner();
    delete owner;
  }
}

Napi::Value create_failing_snapshot(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() == 0) {
    Napi::TypeError::New(env, "A Superstring snapshot is required")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  uint64_t fail_after = 0;
  if (info.Length() > 1) {
    if (!info[1].IsNumber() || info[1].As<Napi::Number>().DoubleValue() < 0) {
      Napi::TypeError::New(env, "failAfter must be a non-negative number")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    fail_after = info[1].As<Napi::Number>().Uint32Value();
  }

  const SuperstringSnapshotLease *upstream = nullptr;
  const SuperstringSnapshotLeaseStatus status =
      superstring_snapshot_lease_acquire(env, info[0], &upstream);
  if (status != SUPERSTRING_SNAPSHOT_LEASE_STATUS_OK) {
    if (!env.IsExceptionPending())
      Napi::TypeError::New(env, "Unable to acquire the source SnapshotLease")
          .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto *owner = new FaultingSnapshotOwner{
      new FaultingSnapshotLeaseState(upstream, fail_after)};
  Napi::Object snapshot = Napi::Object::New(env);
  if (napi_type_tag_object(env, snapshot,
                           &SUPERSTRING_SNAPSHOT_SOURCE_TYPE_TAG) != napi_ok) {
    owner->state->release_owner();
    delete owner;
    Napi::Error::New(env, "Unable to tag the faulting snapshot")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  snapshot.Set(SUPERSTRING_SNAPSHOT_LEASE_METHOD_NAME,
               Napi::Function::New(env, get_snapshot_lease,
                                   SUPERSTRING_SNAPSHOT_LEASE_METHOD_NAME,
                                   owner));
  snapshot.Set("destroy",
               Napi::Function::New(env, destroy_snapshot, "destroy", owner));
  if (napi_add_finalizer(env, snapshot, owner, finalize_snapshot, nullptr,
                         nullptr) != napi_ok) {
    owner->state->release_owner();
    delete owner;
    Napi::Error::New(env, "Unable to finalize the faulting snapshot")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return snapshot;
}

} // namespace

void install_snapshot_lease_test_provider(Napi::Env env,
                                          Napi::Object exports) {
  const char *enabled =
      std::getenv("LUMINE_DOCUMENT_ENGINE_ENABLE_TEST_FAULTS");
  if (enabled == nullptr || std::strcmp(enabled, "1") != 0)
    return;
  exports.Set("_createFailingSnapshotLeaseForTest",
              Napi::Function::New(env, create_failing_snapshot));
}

} // namespace document_engine
