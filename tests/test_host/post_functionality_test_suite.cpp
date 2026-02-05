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
#include <transport/tests/sgx/setup.h>
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
using post_test_implementations = ::testing::Types<

    // in memory tests
    in_memory_setup<false>,
    in_memory_setup<true>,

    // in process marshalled tests
    inproc_setup<false, false>,
    inproc_setup<false, true>,
    inproc_setup<true, false>,
    inproc_setup<true, true>,

#ifdef CANOPY_BUILD_COROUTINE
    ,
    tcp_setup<true, false>,
    tcp_setup<true, true>,
    tcp_setup<false, false>,
    tcp_setup<false, true>,
    spsc_setup<true, false>,
    spsc_setup<true, true>,
    spsc_setup<false, false>,
    spsc_setup<false, true>,

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

// Test basic post functionality with normal option
TYPED_TEST(post_functionality_test, basic_post_normal)
{
    auto& lib = this->get_lib();
    auto root_service = lib.get_root_service();

    run_coro_test(*this,
        [root_service, &lib](auto& lib_param) -> CORO_TASK(bool)
        {
            (void)lib_param; // lib_param not used in this test

            // Get example object (local or remote depending on setup)
            auto example = lib.get_example();
            CORO_ASSERT_NE(example, nullptr);

            // Create a foo object to test with
            rpc::shared_ptr<xxx::i_foo> foo_obj;
            CORO_ASSERT_EQ(CO_AWAIT example->create_foo(foo_obj), 0);
            CORO_ASSERT_NE(foo_obj, nullptr);

            // Get the service proxy for the foo object to use the post functionality
            auto sp = rpc::casting_interface::get_service_proxy(*foo_obj);
            CORO_ASSERT_NE(sp, nullptr);

            // Get the service from the service proxy to call post
            auto service = sp->get_operating_zone_service();
            CORO_ASSERT_NE(service, nullptr);

            // Perform a post operation - fire-and-forget call using the service directly
            // This should not block or return anything
            CO_AWAIT service->post(rpc::get_version(),
                RPC_DEFAULT_ENCODING,
                0, // tag
                sp->get_zone_id().as_caller(),
                sp->get_destination_zone_id(),
                rpc::casting_interface::get_object_id(*foo_obj),
                0,                                             // interface_id - this is for the i_foo interface
                0,                                             // method_id - this would be specific to the method
                rpc::span{(const uint8_t*)nullptr, (size_t)0}, // in_data
                {});                                           // in_back_channel

            // Since this is fire-and-forget, we just verify no crash occurred
            // The actual behavior will depend on the implementation
            CO_RETURN true;
        });
}

// TODO: Add transport_down_notification test using dodgy_transport to trigger actual transport failure
// TODO: Add object_released_notification test by releasing an object with optimistic references

// Test multiple concurrent post operations
TYPED_TEST(post_functionality_test, concurrent_post_operations)
{
    auto& lib = this->get_lib();
    auto root_service = lib.get_root_service();

    run_coro_test(*this,
        [root_service, &lib](auto& lib_param) -> CORO_TASK(bool)
        {
            (void)lib_param; // lib_param not used in this test

            // Get example object (local or remote depending on setup)
            auto example = lib.get_example();
            CORO_ASSERT_NE(example, nullptr);

            // Create multiple foo objects to test with
            std::vector<rpc::shared_ptr<xxx::i_foo>> foo_objects;
            for (int i = 0; i < 5; ++i)
            {
                rpc::shared_ptr<xxx::i_foo> foo_obj;
                CORO_ASSERT_EQ(CO_AWAIT example->create_foo(foo_obj), 0);
                CORO_ASSERT_NE(foo_obj, nullptr);
                foo_objects.push_back(foo_obj);
            }

            // Perform multiple post operations concurrently
            for (size_t i = 0; i < foo_objects.size(); ++i)
            {
                // Get the service proxy for each foo object to use the post functionality
                auto sp = rpc::casting_interface::get_service_proxy(*foo_objects[i]);
                CORO_ASSERT_NE(sp, nullptr);

                // Get the service from the service proxy to call post
                auto service = sp->get_operating_zone_service();
                CORO_ASSERT_NE(service, nullptr);

                // Each post operation is fire-and-forget using the service directly
                CO_AWAIT service->post(rpc::get_version(),
                    RPC_DEFAULT_ENCODING,
                    0, // tag
                    sp->get_zone_id().as_caller(),
                    sp->get_destination_zone_id(),
                    rpc::casting_interface::get_object_id(*foo_objects[i]),
                    0,                                             // interface_id
                    0,                                             // method_id
                    rpc::span{(const uint8_t*)nullptr, (size_t)0}, // in_data
                    {});                                           // in_back_channel
            }

            // Verify no crash occurred
            CO_RETURN true;
        });
}

// Test post functionality with different data sizes
TYPED_TEST(post_functionality_test, post_with_different_data_sizes)
{
    auto& lib = this->get_lib();
    auto root_service = lib.get_root_service();

    run_coro_test(*this,
        [root_service, &lib](auto& lib_param) -> CORO_TASK(bool)
        {
            (void)lib_param; // lib_param not used in this test

            // Get example object (local or remote depending on setup)
            auto example = lib.get_example();
            CORO_ASSERT_NE(example, nullptr);

            // Create a foo object to test with
            rpc::shared_ptr<xxx::i_foo> foo_obj;
            CORO_ASSERT_EQ(CO_AWAIT example->create_foo(foo_obj), 0);
            CORO_ASSERT_NE(foo_obj, nullptr);

            // Test with different data sizes
            std::vector<std::string> test_data = {
                "small",
                std::string(100, 'x'), // Medium size
                std::string(1000, 'y') // Large size
            };

            for (size_t i = 0; i < test_data.size(); ++i)
            {
                // Get the service proxy for the foo object to use the post functionality
                auto sp = rpc::casting_interface::get_service_proxy(*foo_obj);
                CORO_ASSERT_NE(sp, nullptr);

                // Get the service from the service proxy to call post
                auto service = sp->get_operating_zone_service();
                CORO_ASSERT_NE(service, nullptr);

                // Perform post operation with different data sizes - fire-and-forget
                // Using the service directly with the string data as payload
                CO_AWAIT service->post(rpc::get_version(),
                    RPC_DEFAULT_ENCODING,
                    0, // tag
                    sp->get_zone_id().as_caller(),
                    sp->get_destination_zone_id(),
                    rpc::casting_interface::get_object_id(*foo_obj),
                    0,                                                                    // interface_id
                    0,                                                                    // method_id
                    rpc::span{(const uint8_t*)test_data[i].c_str(), test_data[i].size()}, // in_data
                    {});                                                                  // in_back_channel
            }

            // Verify no crash occurred
            CO_RETURN true;
        });
}

// Test that post operations don't affect regular RPC calls
TYPED_TEST(post_functionality_test, post_does_not_interfere_with_regular_calls)
{
    auto& lib = this->get_lib();
    auto root_service = lib.get_root_service();

    run_coro_test(*this,
        [root_service, &lib](auto& lib_param) -> CORO_TASK(bool)
        {
            (void)lib_param; // lib_param not used in this test

            // Get example object (local or remote depending on setup)
            auto example = lib.get_example();
            CORO_ASSERT_NE(example, nullptr);

            // Create a foo object to test with
            rpc::shared_ptr<xxx::i_foo> foo_obj;
            CORO_ASSERT_EQ(CO_AWAIT example->create_foo(foo_obj), 0);
            CORO_ASSERT_NE(foo_obj, nullptr);

            // Perform several post operations
            for (int i = 0; i < 10; ++i)
            {
                // Get the service proxy for the foo object to use the post functionality
                auto sp = rpc::casting_interface::get_service_proxy(*foo_obj);
                CORO_ASSERT_NE(sp, nullptr);

                // Get the service from the service proxy to call post
                auto service = sp->get_operating_zone_service();
                CORO_ASSERT_NE(service, nullptr);

                // Perform post operation - fire-and-forget using the service directly
                CO_AWAIT service->post(rpc::get_version(),
                    RPC_DEFAULT_ENCODING,
                    0, // tag
                    sp->get_zone_id().as_caller(),
                    sp->get_destination_zone_id(),
                    rpc::casting_interface::get_object_id(*foo_obj),
                    0,                                             // interface_id
                    0,                                             // method_id
                    rpc::span{(const uint8_t*)nullptr, (size_t)0}, // in_data
                    {});                                           // in_back_channel
            }

            // Now perform regular RPC calls to ensure they still work
            int result = 5;
            CORO_ASSERT_EQ(CO_AWAIT foo_obj->do_something_in_val(result), 0);

            // Create baz interface through regular call
            rpc::shared_ptr<xxx::i_baz> baz;
            CORO_ASSERT_EQ(CO_AWAIT foo_obj->create_baz_interface(baz), 0);
            CORO_ASSERT_NE(baz, nullptr);

            CO_RETURN true;
        });
}

// Test post operations with optimistic pointer
TYPED_TEST(post_functionality_test, post_with_optimistic_ptr)
{
    auto& lib = this->get_lib();
    auto root_service = lib.get_root_service();

    run_coro_test(*this,
        [root_service, &lib](auto& lib_param) -> CORO_TASK(bool)
        {
            (void)lib_param; // lib_param not used in this test

            // Get example object (local or remote depending on setup)
            auto example = lib.get_example();
            CORO_ASSERT_NE(example, nullptr);

            // Create a foo object to test with
            rpc::shared_ptr<xxx::i_foo> foo_obj;
            CORO_ASSERT_EQ(CO_AWAIT example->create_foo(foo_obj), 0);
            CORO_ASSERT_NE(foo_obj, nullptr);

            // Create an optimistic pointer
            rpc::optimistic_ptr<xxx::i_foo> opt_foo;
            auto err = CO_AWAIT rpc::make_optimistic(foo_obj, opt_foo);
            CORO_ASSERT_EQ(err, rpc::error::OK());

            // Get the service proxy for the foo object to use the post functionality
            auto sp = rpc::casting_interface::get_service_proxy(*foo_obj);
            CORO_ASSERT_NE(sp, nullptr);

            // Get the service from the service proxy to call post
            auto service = sp->get_operating_zone_service();
            CORO_ASSERT_NE(service, nullptr);

            // Perform post operation through service directly (not through optimistic pointer)
            CO_AWAIT service->post(rpc::get_version(),
                RPC_DEFAULT_ENCODING,
                0, // tag
                sp->get_zone_id().as_caller(),
                sp->get_destination_zone_id(),
                rpc::casting_interface::get_object_id(*foo_obj),
                0,                                             // interface_id
                0,                                             // method_id
                rpc::span{(const uint8_t*)nullptr, (size_t)0}, // in_data
                {});                                           // in_back_channel

            // Verify no crash occurred
            CO_RETURN true;
        });
}
