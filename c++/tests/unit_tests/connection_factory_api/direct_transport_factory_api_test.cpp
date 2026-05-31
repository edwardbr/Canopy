/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <transports/local/factory.h>

#ifdef CANOPY_HAS_IPC_SPSC_TRANSPORT_FACTORY
#  include <transports/ipc_spsc/factory.h>
#endif

#ifdef CANOPY_HAS_UNSHARED_SCHEDULER_DLL_TRANSPORT_FACTORY
#  include <transports/unshared_scheduler_dll/factory.h>
#endif

#ifdef CANOPY_HAS_SHARED_SCHEDULER_DLL_TRANSPORT_FACTORY
#  include <transports/shared_scheduler_dll/factory.h>
#endif

#ifdef CANOPY_HAS_SGX_BLOCKING_TRANSPORT_FACTORY
#  include <transports/sgx_blocking/factory.h>
#endif

#ifdef CANOPY_HAS_SGX_COROUTINE_TRANSPORT_FACTORY
#  include <transports/sgx_coroutine/host/factory.h>
#endif

TEST(
    ConnectionFactoryApi,
    DirectLocalTransportFactoryUsesImplementationOwnedTypedSettings)
{
    rpc::local_transport::transport_settings settings;
    settings.name = "api-test-local";
    settings.encoding = rpc::encoding::yas_json;

    auto result = rpc::local_transport::connect_transport(settings);

    ASSERT_EQ(result.error_code, rpc::error::OK());
    ASSERT_NE(result.service, nullptr);
    ASSERT_NE(result.transport, nullptr);
    EXPECT_EQ(result.service_proxy_name, "api-test-local");
}

TEST(
    ConnectionFactoryApi,
    DirectIpcSpscTransportFactoryExposesImplementationOwnedTypedSettings)
{
#ifdef CANOPY_HAS_IPC_SPSC_TRANSPORT_FACTORY
    rpc::ipc_spsc::transport_settings settings;
    settings.name = "api-test-ipc-spsc";
    settings.use_sidecar = true;
    settings.sidecar_executable_path = "/tmp/not-started-by-api-test";
    settings.dynamic_library_path = "/tmp/not-loaded-by-api-test.so";
    settings.peer_to_peer_shared_memory_file = "/tmp/not-opened-by-api-test";
    settings.create_peer_to_peer_shared_memory_file = true;
    settings.unlink_peer_to_peer_shared_memory_file_on_close = true;

    EXPECT_EQ(settings.name.value(), "api-test-ipc-spsc");
    EXPECT_TRUE(settings.use_sidecar);
    EXPECT_EQ(settings.dynamic_library_path, "/tmp/not-loaded-by-api-test.so");
    EXPECT_EQ(settings.peer_to_peer_shared_memory_file, "/tmp/not-opened-by-api-test");
    EXPECT_TRUE(settings.create_peer_to_peer_shared_memory_file);
    EXPECT_TRUE(settings.unlink_peer_to_peer_shared_memory_file_on_close);
#endif

    SUCCEED();
}

TEST(
    ConnectionFactoryApi,
    DirectCoroutineDynamicLibraryTransportFactoriesExposeImplementationOwnedTypedSettings)
{
#ifdef CANOPY_HAS_SHARED_SCHEDULER_DLL_TRANSPORT_FACTORY
    rpc::shared_scheduler_dll::transport_settings shared_settings;
    shared_settings.name = "api-test-shared-scheduler-dll";
    shared_settings.service_proxy_name = "api-test-shared-scheduler-child";
    shared_settings.encoding = rpc::encoding::yas_json;
    shared_settings.dynamic_library_path = "/tmp/not-loaded-by-api-test.so";

    EXPECT_EQ(shared_settings.name.value(), "api-test-shared-scheduler-dll");
    EXPECT_EQ(shared_settings.service_proxy_name.value(), "api-test-shared-scheduler-child");
    EXPECT_EQ(shared_settings.encoding.value(), rpc::encoding::yas_json);
    EXPECT_EQ(shared_settings.dynamic_library_path, "/tmp/not-loaded-by-api-test.so");
#endif

#ifdef CANOPY_HAS_UNSHARED_SCHEDULER_DLL_TRANSPORT_FACTORY
    rpc::unshared_scheduler_dll::transport_settings unshared_settings;
    unshared_settings.name = "api-test-unshared-scheduler-dll";
    unshared_settings.service_proxy_name = "api-test-unshared-scheduler-child";
    unshared_settings.encoding = rpc::encoding::yas_json;
    unshared_settings.dynamic_library_path = "/tmp/not-loaded-by-api-test.so";

    EXPECT_EQ(unshared_settings.name.value(), "api-test-unshared-scheduler-dll");
    EXPECT_EQ(unshared_settings.service_proxy_name.value(), "api-test-unshared-scheduler-child");
    EXPECT_EQ(unshared_settings.encoding.value(), rpc::encoding::yas_json);
    EXPECT_EQ(unshared_settings.dynamic_library_path, "/tmp/not-loaded-by-api-test.so");
#endif

    SUCCEED();
}

TEST(
    ConnectionFactoryApi,
    DirectSgxTransportFactoriesExposeImplementationOwnedTypedSettings)
{
#ifdef CANOPY_HAS_SGX_BLOCKING_TRANSPORT_FACTORY
    rpc::sgx_blocking_transport::transport_settings blocking_settings;
    blocking_settings.name = "api-test-sgx-blocking";
    blocking_settings.enclave_path = "/tmp/not-loaded-by-api-test.signed.so";
    EXPECT_EQ(blocking_settings.name.value(), "api-test-sgx-blocking");
    EXPECT_EQ(blocking_settings.enclave_path, "/tmp/not-loaded-by-api-test.signed.so");
#endif

#ifdef CANOPY_HAS_SGX_COROUTINE_TRANSPORT_FACTORY
    rpc::sgx_coroutine_transport::transport_settings coroutine_settings;
    coroutine_settings.name = "api-test-sgx-coroutine";
    coroutine_settings.enclave_path = "/tmp/not-loaded-by-api-test.signed.so";
    coroutine_settings.use_sidecar = true;
    coroutine_settings.sidecar_executable_path = "/tmp/not-started-by-api-test";
    coroutine_settings.peer_to_peer_shared_memory_file = "/tmp/not-opened-by-api-test";
    EXPECT_EQ(coroutine_settings.name.value(), "api-test-sgx-coroutine");
    EXPECT_TRUE(coroutine_settings.use_sidecar);
    EXPECT_EQ(coroutine_settings.sidecar_executable_path, "/tmp/not-started-by-api-test");
    EXPECT_EQ(coroutine_settings.peer_to_peer_shared_memory_file, "/tmp/not-opened-by-api-test");
#endif

    SUCCEED();
}
