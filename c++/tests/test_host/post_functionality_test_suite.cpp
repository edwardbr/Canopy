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
#  include <rpc/telemetry/i_telemetry_service.h>
#endif

// Other headers
#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wsuggest-override"
#  pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wsuggest-override"
#endif

#include <args.hxx>

#ifdef __clang__
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#ifdef CANOPY_BUILD_COROUTINE
#  include <coro/scheduler.hpp>
#endif
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <common/foo_impl.h>
#include <common/tests.h>

#include "rpc_global_logger.h"
#include "test_host.h"
#include <transport/tests/direct/setup.h>
#include <transport/tests/local/setup.h>
#ifdef CANOPY_BUILD_COROUTINE
#  include <transport/tests/streaming_tcp_coroutine/setup.h>
#  include <transport/tests/streaming_spsc/setup.h>
#endif
#include "crash_handler.h"

using namespace marshalled_tests;

#include "test_globals.h"
#include "type_test_fixture.h"

extern bool enable_multithreaded_tests; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Test fixture for post functionality
template<typename SetupType> using post_functionality_test = type_test<SetupType>;

// Define the test types for different transport mechanisms
using post_test_implementations = ::testing::Types<
    in_memory_setup<false>,
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
    streaming_spsc_setup<false, false, false>,
    streaming_spsc_setup<false, false, true>,
    streaming_spsc_setup<false, true, false>,
    streaming_spsc_setup<false, true, true>,
    streaming_spsc_setup<true, false, false>,
    streaming_spsc_setup<true, false, true>,
    streaming_spsc_setup<true, true, false>,
    streaming_spsc_setup<true, true, true>,
    streaming_tcp_coroutine_setup<false, false, false>,
    streaming_tcp_coroutine_setup<false, false, true>,
    streaming_tcp_coroutine_setup<false, true, false>,
    streaming_tcp_coroutine_setup<false, true, true>,
    streaming_tcp_coroutine_setup<true, false, false>,
    streaming_tcp_coroutine_setup<true, false, true>,
    streaming_tcp_coroutine_setup<true, true, false>,
    streaming_tcp_coroutine_setup<true, true, true>
#endif
    >;

TYPED_TEST_SUITE(
    post_functionality_test,
    post_test_implementations);

// Test for [post] attribute - fire-and-forget one-way messaging with ordering guarantee
template<class T> CORO_TASK(bool) coro_test_post_with_shared_ptr(T& lib)
{
    // Create a foo instance to test with
    rpc::shared_ptr<xxx::i_foo> i_foo_ptr;
    auto ret = CO_AWAIT lib.get_example()->create_foo(i_foo_ptr);
    CORO_ASSERT_EQ(ret, rpc::error::OK());
    CORO_ASSERT_NE(i_foo_ptr, nullptr);

    std::vector<rpc::interface_descriptor> descriptors;
    auto schema_ret = CO_AWAIT rpc::casting_interface::get_schema(
        *i_foo_ptr, descriptors, rpc::encoding::yas_json, rpc::schema_flavor::mcp, false);
    CORO_ASSERT_EQ(schema_ret, rpc::error::OK());

    bool found_record_message = false;
    rpc::interface_ordinal record_message_interface_id;
    rpc::method record_message_method_id;
    uint64_t record_message_tag = 0;
    for (const auto& descriptor : descriptors)
    {
        for (const auto& method : descriptor.methods)
        {
            if (method.name == "record_message")
            {
                found_record_message = true;
                record_message_interface_id = descriptor.interface_id;
                record_message_method_id = method.id;
                record_message_tag = method.tag;
                CORO_ASSERT_EQ(method.post, true);
                CORO_ASSERT_EQ(method.deprecated, false);
            }
        }
    }
    CORO_ASSERT_EQ(found_record_message, true);

    rpc::send_params invalid_call_params;
    invalid_call_params.protocol_version = rpc::get_version();
    invalid_call_params.encoding_type = rpc::encoding::yas_json;
    invalid_call_params.tag = record_message_tag;
    invalid_call_params.interface_id = record_message_interface_id;
    invalid_call_params.method_id = record_message_method_id;
    auto invalid_call_ret = CO_AWAIT rpc::casting_interface::call(*i_foo_ptr, std::move(invalid_call_params));
    CORO_ASSERT_EQ(invalid_call_ret.error_code, rpc::error::INVALID_METHOD_ID());

    // Clear any existing messages first
    auto clear_ret = CO_AWAIT i_foo_ptr->clear_recorded_messages();
    CORO_ASSERT_EQ(clear_ret, rpc::error::OK());

    const std::string generic_post_json = R"({"message_id":1234})";
    rpc::post_params generic_post_params;
    generic_post_params.protocol_version = rpc::get_version();
    generic_post_params.encoding_type = rpc::encoding::yas_json;
    generic_post_params.tag = record_message_tag;
    generic_post_params.interface_id = record_message_interface_id;
    generic_post_params.method_id = record_message_method_id;
    generic_post_params.in_data.assign(generic_post_json.begin(), generic_post_json.end());
    auto generic_post_ret = CO_AWAIT rpc::casting_interface::post(*i_foo_ptr, std::move(generic_post_params));
    CORO_ASSERT_EQ(generic_post_ret, rpc::error::OK());

#ifdef CANOPY_BUILD_COROUTINE
    lib.get_scheduler()->process_events(std::chrono::milliseconds(1));
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif

    std::vector<int> generic_recorded_messages;
    auto generic_get_ret = CO_AWAIT i_foo_ptr->get_recorded_messages(generic_recorded_messages);
    CORO_ASSERT_EQ(generic_get_ret, rpc::error::OK());
    CORO_ASSERT_EQ(generic_recorded_messages.size(), 1u);
    CORO_ASSERT_EQ(generic_recorded_messages.front(), 1234);

    clear_ret = CO_AWAIT i_foo_ptr->clear_recorded_messages();
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

TYPED_TEST(
    post_functionality_test,
    post_with_shared_ptr)
{
    run_coro_test(*this, [](auto& lib) { return coro_test_post_with_shared_ptr<TypeParam>(lib); });
}
