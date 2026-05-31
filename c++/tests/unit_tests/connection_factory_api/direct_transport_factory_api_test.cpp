/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <transports/local/factory.h>

#ifdef CANOPY_HAS_IPC_SPSC_TRANSPORT_FACTORY
#  include <transports/ipc_spsc_transport/factory.h>
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
    rpc::ipc_spsc_transport::transport_settings settings;
    settings.name = "api-test-ipc-spsc";
    settings.use_sidecar = true;
    settings.sidecar_executable_path = "/tmp/not-started-by-api-test";
    settings.dynamic_library_path = "/tmp/not-loaded-by-api-test.so";

    EXPECT_EQ(settings.name.value(), "api-test-ipc-spsc");
    EXPECT_TRUE(settings.use_sidecar);
    EXPECT_EQ(settings.dynamic_library_path, "/tmp/not-loaded-by-api-test.so");
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
    EXPECT_EQ(coroutine_settings.name.value(), "api-test-sgx-coroutine");
    EXPECT_TRUE(coroutine_settings.use_sidecar);
#endif

    SUCCEED();
}
