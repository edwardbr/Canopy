/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <memory>
#include <utility>

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
#  include <transports/sgx_blocking/transport.h>
#endif

#ifdef CANOPY_HAS_SGX_COROUTINE_TRANSPORT_FACTORY
#  include <transports/sgx_coroutine/host/factory.h>
#  include <transports/sgx_coroutine/host/transport.h>
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

TEST(
    ConnectionFactoryApi,
    DirectSgxTransportFactoriesConstructWithImmutableStartupSettings)
{
#ifdef CANOPY_HAS_SGX_BLOCKING_TRANSPORT_FACTORY
    rpc::sgx_blocking_transport::transport_settings blocking_settings;
    blocking_settings.name = "api-test-sgx-blocking";
    blocking_settings.service_proxy_name = "api-test-sgx-blocking-child";
    blocking_settings.enclave_path = "/tmp/not-loaded-by-api-test.signed.so";
    blocking_settings.enclave = rpc::sgx_enclave_runtime::runtime_settings{};
    blocking_settings.enclave.value().io_uring.queue_depth = 64;

    ASSERT_EQ(
        rpc::sgx_blocking_transport::enclave_transport::validate_startup_settings(blocking_settings), rpc::error::OK());
    auto blocking_service = rpc::root_service::create("api-test-sgx-blocking-direct-service", rpc::DEFAULT_PREFIX);
    auto direct_blocking_transport = std::make_shared<rpc::sgx_blocking_transport::enclave_transport>(
        "api-test-sgx-blocking-direct", blocking_service, blocking_settings);
    EXPECT_EQ(direct_blocking_transport->get_name(), "api-test-sgx-blocking-direct");
    EXPECT_EQ(direct_blocking_transport->get_enclave_path(), "/tmp/not-loaded-by-api-test.signed.so");
    ASSERT_TRUE(direct_blocking_transport->get_enclave_runtime_startup_settings().has_value());
    EXPECT_EQ(direct_blocking_transport->get_enclave_runtime_startup_settings().value().io_uring.queue_depth, 64u);

    auto blocking_result = rpc::sgx_blocking_transport::connect_transport(blocking_settings);
    ASSERT_EQ(blocking_result.error_code, rpc::error::OK());
    auto blocking_transport
        = std::dynamic_pointer_cast<rpc::sgx_blocking_transport::enclave_transport>(blocking_result.transport);
    ASSERT_NE(blocking_transport, nullptr);
    EXPECT_EQ(blocking_transport->get_name(), "api-test-sgx-blocking");
    EXPECT_EQ(blocking_transport->get_enclave_path(), "/tmp/not-loaded-by-api-test.signed.so");
    ASSERT_TRUE(blocking_transport->get_enclave_runtime_startup_settings().has_value());
    EXPECT_EQ(blocking_transport->get_enclave_runtime_startup_settings().value().io_uring.queue_depth, 64u);
    EXPECT_EQ(blocking_result.service_proxy_name, "api-test-sgx-blocking-child");
#endif

#ifdef CANOPY_HAS_SGX_COROUTINE_TRANSPORT_FACTORY
    rpc::sgx_coroutine_transport::transport_settings coroutine_settings;
    coroutine_settings.name = "api-test-sgx-coroutine";
    coroutine_settings.service_proxy_name = "api-test-sgx-coroutine-child";
    coroutine_settings.enclave_path = "/tmp/not-loaded-by-api-test.signed.so";
    coroutine_settings.worker_thread_count = 2;
    coroutine_settings.use_sidecar = true;
    coroutine_settings.sidecar_executable_path = "/tmp/not-started-by-api-test";
    coroutine_settings.peer_to_peer_shared_memory_file = "/tmp/not-opened-by-api-test";
    coroutine_settings.startup_applications.emplace("filesystem", json::v1::object{json::v1::map{}});
    coroutine_settings.enclave = rpc::sgx_enclave_runtime::runtime_settings{};
    coroutine_settings.enclave.value().io_uring.queue_depth = 32;

    ASSERT_EQ(
        rpc::sgx_coroutine_transport::host::transport::validate_startup_settings(coroutine_settings), rpc::error::OK());
    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
            .pool = coro::thread_pool::options{.thread_count = 1}}));
    auto coroutine_service
        = rpc::root_service::create("api-test-sgx-coroutine-direct-service", rpc::DEFAULT_PREFIX, scheduler);
    auto direct_coroutine_transport = std::make_shared<rpc::sgx_coroutine_transport::host::transport>(
        "api-test-sgx-coroutine-direct", coroutine_service, coroutine_settings);
    EXPECT_EQ(direct_coroutine_transport->get_name(), "api-test-sgx-coroutine-direct");
    EXPECT_EQ(direct_coroutine_transport->get_enclave_path(), "/tmp/not-loaded-by-api-test.signed.so");
    EXPECT_TRUE(direct_coroutine_transport->get_use_sidecar());
    EXPECT_EQ(direct_coroutine_transport->enclave_worker_thread_count_for_test(), 2u);
    EXPECT_EQ(direct_coroutine_transport->sidecar_executable_path_for_test(), "/tmp/not-started-by-api-test");
    EXPECT_EQ(direct_coroutine_transport->peer_to_peer_shared_memory_file_for_test(), "/tmp/not-opened-by-api-test");
    EXPECT_EQ(direct_coroutine_transport->enclave_startup_applications_for_test().size(), 1u);
    EXPECT_NE(
        direct_coroutine_transport->enclave_startup_applications_for_test().find("filesystem"),
        direct_coroutine_transport->enclave_startup_applications_for_test().end());
    ASSERT_TRUE(direct_coroutine_transport->get_enclave_runtime_startup_settings().has_value());
    EXPECT_EQ(direct_coroutine_transport->get_enclave_runtime_startup_settings().value().io_uring.queue_depth, 32u);
    ASSERT_TRUE(direct_coroutine_transport->get_enclave_io_uring_options().has_value());
    EXPECT_EQ(direct_coroutine_transport->get_enclave_io_uring_options().value().queue_depth, 32u);

    auto coroutine_result = rpc::sgx_coroutine_transport::host::connect_transport(coroutine_settings);
    ASSERT_EQ(coroutine_result.error_code, rpc::error::OK());
    auto coroutine_transport
        = std::dynamic_pointer_cast<rpc::sgx_coroutine_transport::host::transport>(coroutine_result.transport);
    ASSERT_NE(coroutine_transport, nullptr);
    EXPECT_EQ(coroutine_transport->get_name(), "api-test-sgx-coroutine");
    EXPECT_EQ(coroutine_transport->get_enclave_path(), "/tmp/not-loaded-by-api-test.signed.so");
    EXPECT_TRUE(coroutine_transport->get_use_sidecar());
    EXPECT_EQ(coroutine_transport->enclave_worker_thread_count_for_test(), 2u);
    EXPECT_EQ(coroutine_transport->sidecar_executable_path_for_test(), "/tmp/not-started-by-api-test");
    EXPECT_EQ(coroutine_transport->peer_to_peer_shared_memory_file_for_test(), "/tmp/not-opened-by-api-test");
    EXPECT_EQ(coroutine_transport->enclave_startup_applications_for_test().size(), 1u);
    EXPECT_NE(
        coroutine_transport->enclave_startup_applications_for_test().find("filesystem"),
        coroutine_transport->enclave_startup_applications_for_test().end());
    ASSERT_TRUE(coroutine_transport->get_enclave_runtime_startup_settings().has_value());
    EXPECT_EQ(coroutine_transport->get_enclave_runtime_startup_settings().value().io_uring.queue_depth, 32u);
    ASSERT_TRUE(coroutine_transport->get_enclave_io_uring_options().has_value());
    EXPECT_EQ(coroutine_transport->get_enclave_io_uring_options().value().queue_depth, 32u);
    EXPECT_EQ(coroutine_result.service_proxy_name, "api-test-sgx-coroutine-child");
    direct_coroutine_transport.reset();
    coroutine_service.reset();
    scheduler->shutdown();
#endif

    SUCCEED();
}

TEST(
    ConnectionFactoryApi,
    DirectSgxTransportFactoriesRejectInvalidStartupSettingsBeforeConstruction)
{
#ifdef CANOPY_HAS_SGX_BLOCKING_TRANSPORT_FACTORY
    rpc::sgx_blocking_transport::transport_settings invalid_blocking;
    auto invalid_blocking_result = rpc::sgx_blocking_transport::connect_transport(invalid_blocking);
    EXPECT_EQ(invalid_blocking_result.error_code, rpc::error::INVALID_DATA());
    EXPECT_EQ(invalid_blocking_result.transport, nullptr);
#endif

#ifdef CANOPY_HAS_SGX_COROUTINE_TRANSPORT_FACTORY
    rpc::sgx_coroutine_transport::transport_settings invalid_coroutine;
    invalid_coroutine.enclave_path = "/tmp/not-loaded-by-api-test.signed.so";
    invalid_coroutine.startup_applications.emplace("", json::v1::object{json::v1::map{}});

    EXPECT_EQ(
        rpc::sgx_coroutine_transport::host::transport::validate_startup_settings(invalid_coroutine),
        rpc::error::INVALID_DATA());

    auto invalid_coroutine_result = rpc::sgx_coroutine_transport::host::connect_transport(invalid_coroutine);
    EXPECT_EQ(invalid_coroutine_result.error_code, rpc::error::INVALID_DATA());
    EXPECT_EQ(invalid_coroutine_result.transport, nullptr);
#endif

    SUCCEED();
}
