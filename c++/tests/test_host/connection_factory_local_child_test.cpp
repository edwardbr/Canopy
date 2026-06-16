/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <atomic>
#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include <json/config.h>
#include <connection_factory/connection_factory.h>
#include <common/foo_impl.h>

#include "test_host.h"

namespace
{
    CORO_TASK(bool)
    run_local_child_connection_factory_test(
        std::shared_ptr<rpc::service> service,
        rpc::executor_ptr expected_child_executor = {})
    {
        bool child_executor_matches = true;
        auto settings = rpc::connection_factory::materialise_connection_settings(json::v1::parse(R"json({
            "transport": {
                "type": "local",
                "settings": { "name": "configured_local_child", "encoding": "nanopb" }
            }
        })json"));
        if (settings.error_code != rpc::error::OK())
            CO_RETURN false;

        rpc::shared_ptr<yyy::i_host> local_host(new host());
        auto connect_result = CO_AWAIT rpc::connection_factory::connect_local_child_rpc<yyy::i_host, yyy::i_example>(
            local_host,
            [expected_child_executor, &child_executor_matches](
                rpc::shared_ptr<yyy::i_host> remote_host,
                std::shared_ptr<rpc::service> child_service) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
            {
                if (expected_child_executor)
                    child_executor_matches = child_service && child_service->get_executor() == expected_child_executor;
                auto example = rpc::shared_ptr<yyy::i_example>(
                    new marshalled_tests::example(std::move(child_service), std::move(remote_host)));
                CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(example)};
            },
            settings.settings,
            std::move(service));
        if (connect_result.error_code != rpc::error::OK() || !connect_result.output_interface)
            CO_RETURN false;

        int result = 0;
        auto add_error = CO_AWAIT connect_result.output_interface->add(20, 22, result);
        CO_RETURN add_error == rpc::error::OK() && result == 42 && child_executor_matches;
    }

#ifndef CANOPY_BUILD_COROUTINE
    CORO_TASK(bool)
    run_blocking_dll_connection_factory_test(std::shared_ptr<rpc::service> service)
    {
        auto config = std::string(R"json({
            "transport": {
                "type": "blocking_dll",
                "settings": {
                    "name": "configured_blocking_dll",
                    "encoding": "nanopb",
                    "dynamic_library_path": ")json")
                      + CANOPY_TEST_DLL_PATH + R"json("
                }
            }
        })json";
        auto settings = rpc::connection_factory::materialise_connection_settings(json::v1::parse(config));
        if (settings.error_code != rpc::error::OK())
            CO_RETURN false;

        rpc::shared_ptr<yyy::i_host> local_host(new host());
        auto connect_result = CO_AWAIT rpc::connection_factory::connect_rpc<yyy::i_host, yyy::i_example>(
            local_host, settings.settings, std::move(service));
        if (connect_result.error_code != rpc::error::OK() || !connect_result.output_interface)
            CO_RETURN false;

        int result = 0;
        auto add_error = CO_AWAIT connect_result.output_interface->add(20, 22, result);
        CO_RETURN add_error == rpc::error::OK() && result == 42;
    }
#endif
}

TEST(
    ConnectionFactoryLocalChildRpc,
    SparseConfigCreatesLocalChildZone)
{
#ifdef CANOPY_BUILD_COROUTINE
    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
            .pool = coro::thread_pool::options{.thread_count = 1}}));
    auto service = rpc::root_service::create("local_config_parent", rpc::DEFAULT_PREFIX, scheduler);

    std::atomic_bool done{false};
    bool passed = false;
    auto runner = [&]() -> CORO_TASK(void)
    {
        passed = CO_AWAIT run_local_child_connection_factory_test(std::move(service), scheduler);
        done.store(true, std::memory_order_release);
        CO_RETURN;
    };

    ASSERT_TRUE(scheduler->spawn_detached(runner()));

    for (int i = 0; i < 3000 && !done.load(std::memory_order_acquire); ++i)
        scheduler->process_events(std::chrono::milliseconds{1});

    EXPECT_TRUE(done.load(std::memory_order_acquire));
    EXPECT_TRUE(passed);

    for (int i = 0; i < 100 && !scheduler->empty(); ++i)
        scheduler->process_events(std::chrono::milliseconds{1});
#else
    auto service = rpc::root_service::create("local_config_parent", rpc::DEFAULT_PREFIX);
    EXPECT_TRUE(run_local_child_connection_factory_test(std::move(service)));
#endif
}

#ifndef CANOPY_BUILD_COROUTINE
TEST(
    ConnectionFactoryLocalChildRpc,
    BlockingParentExecutorIsInheritedByChildZone)
{
    auto executor = std::make_shared<rpc::blocking_executor>();
    auto service = rpc::root_service::create("local_config_parent_with_executor", rpc::DEFAULT_PREFIX, executor);
    EXPECT_TRUE(run_local_child_connection_factory_test(std::move(service), executor));
    executor->shutdown();
}

TEST(
    ConnectionFactoryBlockingDllRpc,
    SparseConfigCreatesBlockingDllZone)
{
    auto service = rpc::root_service::create("blocking_dll_config_parent", rpc::DEFAULT_PREFIX);
    EXPECT_TRUE(run_blocking_dll_connection_factory_test(std::move(service)));
}
#endif
