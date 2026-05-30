/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#if defined(CANOPY_BUILD_ENCLAVE) && defined(CANOPY_BUILD_COROUTINE)
#  include <rpc/rpc.h>
#  include <common/tests.h>
#  include "gtest/gtest.h"
#  include "type_test_fixture.h"
#  include <transport/tests/sgx_coroutine/setup.h>

#  include <atomic>
#  include <chrono>
#  include <memory>
#  include <utility>

#  if defined(__linux__)
#    include <cerrno>
#    include <csignal>
#    include <unistd.h>
#  endif

template<class T> class sgx_coroutine_transport_test : public type_test<T>
{
};

using sgx_coroutine_transport_test_implementations
    = ::testing::Types<sgx_coroutine_setup<false, false, false>, sgx_coroutine_setup<true, false, false>>;

TYPED_TEST_SUITE(
    sgx_coroutine_transport_test,
    sgx_coroutine_transport_test_implementations);

TYPED_TEST(
    sgx_coroutine_transport_test,
    remote_standard_tests)
{
    run_coro_test(*this, [](auto& lib) { return marshalled_tests::coro_remote_standard_tests(lib); });
}

#  if defined(__linux__) && defined(CANOPY_TEST_SGX_COROUTINE_SIDECAR_PATH)
namespace
{
    template<class Predicate>
    bool pump_until(
        const std::shared_ptr<coro::scheduler>& scheduler,
        Predicate&& predicate,
        std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;
            scheduler->process_events(std::chrono::milliseconds{1});
        }
        return predicate();
    }

    void release_sidecar_test_state(
        std::shared_ptr<coro::scheduler> scheduler,
        std::shared_ptr<rpc::root_service>& root_service,
        rpc::shared_ptr<yyy::i_example>& output_interface)
    {
        auto shutdown_event = std::make_shared<rpc::event>(false);
        if (root_service)
            root_service->set_shutdown_event(shutdown_event);

        std::atomic<bool> interfaces_released{false};
        auto release_interfaces = [&]() -> CORO_TASK(void)
        {
            output_interface = nullptr;
            interfaces_released.store(true, std::memory_order_release);
            CO_RETURN;
        };
        ASSERT_TRUE(scheduler->spawn_detached(release_interfaces()));
        ASSERT_TRUE(pump_until(
            scheduler,
            [&]() { return interfaces_released.load(std::memory_order_acquire); },
            std::chrono::milliseconds{5000}));

        std::atomic<bool> root_released{false};
        auto release_root = [&]() -> CORO_TASK(void)
        {
            root_service = nullptr;
            current_host_service.reset();
            CO_AWAIT shutdown_event->wait();
            root_released.store(true, std::memory_order_release);
            CO_RETURN;
        };
        ASSERT_TRUE(scheduler->spawn_detached(release_root()));
        ASSERT_TRUE(pump_until(
            scheduler, [&]() { return root_released.load(std::memory_order_acquire); }, std::chrono::milliseconds{5000}));
    }

    void release_sidecar_output_interface(
        const std::shared_ptr<coro::scheduler>& scheduler,
        rpc::shared_ptr<yyy::i_example>& output_interface)
    {
        std::atomic<bool> released{false};
        auto release_interface = [&]() -> CORO_TASK(void)
        {
            output_interface = nullptr;
            released.store(true, std::memory_order_release);
            CO_RETURN;
        };
        ASSERT_TRUE(scheduler->spawn_detached(release_interface()));
        ASSERT_TRUE(pump_until(
            scheduler, [&]() { return released.load(std::memory_order_acquire); }, std::chrono::milliseconds{5000}));
    }

    bool process_is_alive(pid_t pid)
    {
        if (pid <= 0)
            return false;
        return ::kill(pid, 0) == 0 || errno == EPERM;
    }
}

TEST(
    sgx_coroutine_sidecar_transport_test,
    sidecar_startup_exit_reports_error_and_host_continues)
{
    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{
            .thread_strategy = coro::scheduler::thread_strategy_t::manual,
            .pool = coro::thread_pool::options{.thread_count = 1},
        }));
    auto root_service = rpc::root_service::create("sidecar failed startup host", rpc::DEFAULT_PREFIX, scheduler);
    current_host_service = root_service;

    auto transport = std::make_shared<rpc::sgx_coroutine_transport::host::transport>(
        "sidecar failed startup child", root_service, coroutine_enclave_path);
    transport->set_use_sidecar(true);
    transport->set_sidecar_executable_path("/bin/false");

    auto connect_result = sgx_coroutine_setup_detail::run_on_manual_scheduler<rpc::service_connect_result<yyy::i_example>>(
        scheduler,
        rpc::sgx_coroutine_transport::host::connect_to_enclave_zone<yyy::i_host, yyy::i_example>(
            root_service, "sidecar failed startup child", transport, nullptr));

    EXPECT_EQ(connect_result.error_code, rpc::error::TRANSPORT_ERROR());
    EXPECT_EQ(connect_result.output_interface, nullptr);
    EXPECT_TRUE(
        pump_until(scheduler, [&]() { return transport->sidecar_pid_for_test() <= 0; }, std::chrono::milliseconds{5000}));

    rpc::get_new_zone_id_params zone_params;
    zone_params.protocol_version = rpc::get_version();
    auto zone_result = sgx_coroutine_setup_detail::run_on_manual_scheduler<rpc::new_zone_id_result>(
        scheduler, root_service->get_new_zone_id(std::move(zone_params)), std::chrono::milliseconds{1000});
    EXPECT_EQ(zone_result.error_code, rpc::error::OK());

    auto transport_ref = std::weak_ptr<rpc::sgx_coroutine_transport::host::transport>(transport);
    release_sidecar_test_state(scheduler, root_service, connect_result.output_interface);
    transport.reset();
    EXPECT_TRUE(pump_until(scheduler, [&]() { return transport_ref.expired(); }, std::chrono::milliseconds{5000}));

    scheduler->shutdown();
}

TEST(
    sgx_coroutine_sidecar_transport_test,
    sidecar_clean_shutdown_stops_child_process_and_host_continues)
{
    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{
            .thread_strategy = coro::scheduler::thread_strategy_t::manual,
            .pool = coro::thread_pool::options{.thread_count = 1},
        }));
    auto root_service = rpc::root_service::create("sidecar clean shutdown host", rpc::DEFAULT_PREFIX, scheduler);
    current_host_service = root_service;

    auto transport = std::make_shared<rpc::sgx_coroutine_transport::host::transport>(
        "sidecar clean shutdown child", root_service, coroutine_enclave_path);
    transport->set_use_sidecar(true);
    transport->set_sidecar_executable_path(CANOPY_TEST_SGX_COROUTINE_SIDECAR_PATH);

    auto connect_result = sgx_coroutine_setup_detail::run_on_manual_scheduler<rpc::service_connect_result<yyy::i_example>>(
        scheduler,
        rpc::sgx_coroutine_transport::host::connect_to_enclave_zone<yyy::i_host, yyy::i_example>(
            root_service, "sidecar clean shutdown child", transport, nullptr));

    ASSERT_EQ(connect_result.error_code, rpc::error::OK());
    ASSERT_NE(connect_result.output_interface, nullptr);

    const auto sidecar_pid = transport->sidecar_pid_for_test();
    ASSERT_GT(sidecar_pid, 0);

    release_sidecar_output_interface(scheduler, connect_result.output_interface);
    EXPECT_TRUE(pump_until(
        scheduler,
        [&]() { return transport->get_status() == rpc::transport_status::DISCONNECTED; },
        std::chrono::milliseconds{5000}));
    EXPECT_TRUE(pump_until(
        scheduler,
        [&]() { return transport->sidecar_pid_for_test() <= 0 || !process_is_alive(sidecar_pid); },
        std::chrono::milliseconds{5000}));

    rpc::get_new_zone_id_params zone_params;
    zone_params.protocol_version = rpc::get_version();
    auto zone_result = sgx_coroutine_setup_detail::run_on_manual_scheduler<rpc::new_zone_id_result>(
        scheduler, root_service->get_new_zone_id(std::move(zone_params)), std::chrono::milliseconds{1000});
    EXPECT_EQ(zone_result.error_code, rpc::error::OK());

    auto transport_ref = std::weak_ptr<rpc::sgx_coroutine_transport::host::transport>(transport);
    release_sidecar_test_state(scheduler, root_service, connect_result.output_interface);
    transport.reset();
    EXPECT_TRUE(pump_until(scheduler, [&]() { return transport_ref.expired(); }, std::chrono::milliseconds{5000}));

    scheduler->shutdown();
}

TEST(
    sgx_coroutine_sidecar_transport_test,
    sidecar_crash_notifies_transport_down_and_host_continues)
{
    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{
            .thread_strategy = coro::scheduler::thread_strategy_t::manual,
            .pool = coro::thread_pool::options{.thread_count = 1},
        }));
    auto root_service = rpc::root_service::create("sidecar host", rpc::DEFAULT_PREFIX, scheduler);
    current_host_service = root_service;

    auto transport = std::make_shared<rpc::sgx_coroutine_transport::host::transport>(
        "sidecar child", root_service, coroutine_enclave_path);
    transport->set_use_sidecar(true);
    transport->set_sidecar_executable_path(CANOPY_TEST_SGX_COROUTINE_SIDECAR_PATH);

    auto connect_result = sgx_coroutine_setup_detail::run_on_manual_scheduler<rpc::service_connect_result<yyy::i_example>>(
        scheduler,
        rpc::sgx_coroutine_transport::host::connect_to_enclave_zone<yyy::i_host, yyy::i_example>(
            root_service, "sidecar child", transport, nullptr));

    ASSERT_EQ(connect_result.error_code, rpc::error::OK());
    ASSERT_NE(connect_result.output_interface, nullptr);

    const auto sidecar_pid = transport->sidecar_pid_for_test();
    ASSERT_GT(sidecar_pid, 0);
    ASSERT_EQ(::kill(sidecar_pid, SIGKILL), 0);

    ASSERT_TRUE(pump_until(
        scheduler,
        [&]() { return transport->get_status() >= rpc::transport_status::DISCONNECTING; },
        std::chrono::milliseconds{5000}));
    release_sidecar_output_interface(scheduler, connect_result.output_interface);

    EXPECT_TRUE(pump_until(
        scheduler,
        [&]() { return transport->get_status() == rpc::transport_status::DISCONNECTED; },
        std::chrono::milliseconds{5000}));

    rpc::get_new_zone_id_params zone_params;
    zone_params.protocol_version = rpc::get_version();
    auto zone_result = sgx_coroutine_setup_detail::run_on_manual_scheduler<rpc::new_zone_id_result>(
        scheduler, root_service->get_new_zone_id(std::move(zone_params)), std::chrono::milliseconds{1000});
    EXPECT_EQ(zone_result.error_code, rpc::error::OK());

    auto transport_ref = std::weak_ptr<rpc::sgx_coroutine_transport::host::transport>(transport);
    release_sidecar_test_state(scheduler, root_service, connect_result.output_interface);
    transport.reset();
    EXPECT_TRUE(pump_until(scheduler, [&]() { return transport_ref.expired(); }, std::chrono::milliseconds{5000}));

    scheduler->shutdown();
}
#  endif

#endif
