#ifndef LUMINE_DOCUMENT_ENGINE_SNAPSHOT_LEASE_TEST_PROVIDER_H_
#define LUMINE_DOCUMENT_ENGINE_SNAPSHOT_LEASE_TEST_PROVIDER_H_

#include <napi.h>

namespace document_engine {

void install_snapshot_lease_test_provider(Napi::Env env,
                                          Napi::Object exports);

} // namespace document_engine

#endif // LUMINE_DOCUMENT_ENGINE_SNAPSHOT_LEASE_TEST_PROVIDER_H_
