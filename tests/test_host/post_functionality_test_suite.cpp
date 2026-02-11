/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Standard C++ headers
#include <iostream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <filesystem>
#include <atomic>

// RPC headers
#include <rpc/rpc.h>
#ifdef CANOPY_USE_TELEMETRY
#include <rpc/telemetry/i_telemetry_service.h>
#include <rpc/telemetry/multiplexing_telemetry_service.h>
#include <rpc/telemetry/console_telemetry_service.h>
#include <rpc/telemetry/sequence_diagram_telemetry_service.h>
#endif

// Other headers
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsuggest-override"
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsuggest-override"
#endif

#include <args.hxx>

#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef CANOPY_BUILD_COROUTINE
#include <coro/io_scheduler.hpp>
#endif
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <common/foo_impl.h>
#include <common/tests.h>

#include "rpc_global_logger.h"
#include "test_host.h"
#include <transport/tests/direct/setup.h>
#include <transport/tests/local/setup.h>
#ifdef CANOPY_BUILD_ENCLAVE
#include <transport/tests/sgx/setup.h>
#endif
#ifdef CANOPY_BUILD_COROUTINE
#include <transport/tests/tcp/setup.h>
#include <transport/tests/spsc/setup.h>
#endif
#include "crash_handler.h"

using namespace marshalled_tests;

#include "test_globals.h"
#include "type_test_fixture.h"

extern bool enable_multithreaded_tests;

// Test fixture for post functionality
template<typename SetupType> using post_functionality_test = type_test<SetupType>;

// Define the test types for different transport mechanisms
using post_test_implementations = ::testing::Types<in_memory_setup<false>,
    in_memory_setup<true>,
    inproc_setup<false, false, false>,
    inproc_setup<false, false, true>,
    inproc_setup<false, true, false>,
    inproc_setup<false, true, true>,
    inproc_setup<true, false, false>,
    inproc_setup<true, false, true>,
    inproc_setup<true, true, false>,
    inproc_setup<true, true, true>
#ifdef CANOPY_BUILD_COROUTINE
    ,
    tcp_setup<false, false, false>,
    tcp_setup<false, false, true>,
    tcp_setup<false, true, false>,
    tcp_setup<false, true, true>,
    tcp_setup<true, false, false>,
    tcp_setup<true, false, true>,
    tcp_setup<true, true, false>,
    tcp_setup<true, true, true>,
    spsc_setup<false, false, false>,
    spsc_setup<false, false, true>,
    spsc_setup<false, true, false>,
    spsc_setup<false, true, true>,
    spsc_setup<true, false, false>,
    spsc_setup<true, false, true>,
    spsc_setup<true, true, false>,
    spsc_setup<true, true, true>
#endif
#ifdef CANOPY_BUILD_ENCLAVE
    ,
    sgx_setup<false, false, false>,
    sgx_setup<false, false, true>,
    sgx_setup<false, true, false>,
    sgx_setup<false, true, true>,
    sgx_setup<true, false, false>,
    sgx_setup<true, false, true>,
    sgx_setup<true, true, false>,
    sgx_setup<true, true, true>
#endif
    >;

TYPED_TEST_SUITE(post_functionality_test, post_test_implementations);

// Test for [post] attribute - fire-and-forget one-way messaging with ordering guarantee
template<class T> CORO_TASK(bool) coro_test_post_attribute(T& lib)
{
    // Create a foo instance to test with
    rpc::shared_ptr<xxx::i_foo> i_foo_ptr;
    auto ret = CO_AWAIT lib.get_example()->create_foo(i_foo_ptr);
    CORO_ASSERT_EQ(ret, rpc::error::OK());
    CORO_ASSERT_NE(i_foo_ptr, nullptr);

    // Clear any existing messages first
    auto clear_ret = CO_AWAIT i_foo_ptr->clear_recorded_messages();
    CORO_ASSERT_EQ(clear_ret, rpc::error::OK());

    // Test 1: Send multiple [post] messages and verify they are received in order
    const int num_messages = 10;
    for (int i = 0; i < num_messages; ++i)
    {
        // [post] methods are fire-and-forget - they return immediately without waiting for response
        auto post_ret = CO_AWAIT i_foo_ptr->record_message(i);
        CORO_ASSERT_EQ(post_ret, rpc::error::OK());
    }

    // Give some time for all messages to be processed (since they're async)
    // In real implementation, post messages should be buffered and processed in order
#ifdef CANOPY_BUILD_COROUTINE
    for (int i = 0; i < num_messages; ++i)
    {
        lib.get_scheduler()->process_events(std::chrono::milliseconds(1));
    }
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif

    // Retrieve recorded messages and verify ordering
    std::vector<int> recorded_messages;
    auto get_ret = CO_AWAIT i_foo_ptr->get_recorded_messages(recorded_messages);
    CORO_ASSERT_EQ(get_ret, rpc::error::OK());

    // Verify all messages were received
    CORO_ASSERT_EQ(recorded_messages.size(), num_messages);

    // Verify messages were received in the order they were sent
    for (int i = 0; i < num_messages; ++i)
    {
        CORO_ASSERT_EQ(recorded_messages[i], i);
    }

    // Test 2: Clear messages and verify
    clear_ret = CO_AWAIT i_foo_ptr->clear_recorded_messages();
    CORO_ASSERT_EQ(clear_ret, rpc::error::OK());

    recorded_messages.clear();
    get_ret = CO_AWAIT i_foo_ptr->get_recorded_messages(recorded_messages);
    CORO_ASSERT_EQ(get_ret, rpc::error::OK());
    CORO_ASSERT_EQ(recorded_messages.size(), 0);

    // Test 3: Send a larger batch to stress test ordering
    const int large_batch = 100;
    for (int i = 0; i < large_batch; ++i)
    {
        auto post_ret = CO_AWAIT i_foo_ptr->record_message(i * 2); // Send even numbers
        CORO_ASSERT_EQ(post_ret, rpc::error::OK());
    }

#ifdef CANOPY_BUILD_COROUTINE
    for (int i = 0; i < large_batch; ++i)
    {
        lib.get_scheduler()->process_events(std::chrono::milliseconds(1));
    }
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
#endif

    recorded_messages.clear();
    get_ret = CO_AWAIT i_foo_ptr->get_recorded_messages(recorded_messages);
    CORO_ASSERT_EQ(get_ret, rpc::error::OK());
    CORO_ASSERT_EQ(recorded_messages.size(), large_batch);

    // Verify ordering for large batch
    for (int i = 0; i < large_batch; ++i)
    {
        CORO_ASSERT_EQ(recorded_messages[i], i * 2);
    }

    RPC_INFO("Post attribute test completed successfully - all {} messages received in order", large_batch);

    CO_RETURN true;
}

TYPED_TEST(post_functionality_test, test_post_attribute)
{
    run_coro_test(*this, [](auto& lib) { return coro_test_post_attribute<TypeParam>(lib); });
}
