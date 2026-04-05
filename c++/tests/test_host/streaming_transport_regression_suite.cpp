/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <common/tests.h>
#include <vector>

#include "gtest/gtest.h"
#include "test_host.h"
#include "test_globals.h"

#ifdef CANOPY_BUILD_COROUTINE
#  include <transport/tests/streaming_tcp/setup.h>
#  include <transport/tests/streaming_spsc/setup.h>
#  include <transport/tests/streaming_iouring/setup.h>
#endif

#include "type_test_fixture.h"

using namespace marshalled_tests;

template<class T> using streaming_transport_regression_test = type_test<T>;

#ifdef CANOPY_BUILD_COROUTINE
// Keep this suite on the in-process SPSC streaming path for now. TCP and io_uring
// need separate host-level coverage once their heavier setup/teardown characteristics
// are accounted for in the test harness.
using streaming_transport_regression_implementations = ::testing::Types<
    streaming_tcp_setup<false, false, false>,
    streaming_spsc_setup<false, false, false>
    // ,
    // streaming_iouring_setup<false, false, false>
    >;

TYPED_TEST_SUITE(
    streaming_transport_regression_test,
    streaming_transport_regression_implementations);

template<class T> CORO_TASK(bool) coro_large_blob_round_trip_progress(T& lib)
{
    rpc::shared_ptr<xxx::i_baz> baz_ptr;
    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->create_baz(baz_ptr), rpc::error::OK());
    CORO_ASSERT_NE(baz_ptr, nullptr);

    std::vector<uint8_t> input(256 * 1024);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<uint8_t>(i & 0xFF);

    constexpr int rounds = 4;
    for (int round = 0; round < rounds; ++round)
    {
        std::vector<uint8_t> output;
        CORO_ASSERT_EQ(CO_AWAIT baz_ptr->blob_test(input, output), rpc::error::OK());
        CORO_ASSERT_EQ(output, input);
    }

    baz_ptr = nullptr;
    CO_RETURN true;
}

TYPED_TEST(
    streaming_transport_regression_test,
    large_blob_round_trip_progress)
{
    run_coro_test(*this, [](auto& lib) { return coro_large_blob_round_trip_progress<TypeParam>(lib); });
}

// One concurrent add() call. All parameters are moved into the coroutine frame
// by value so there is no dangling reference to a temporary lambda closure.
static CORO_TASK(void) do_add_call(
    rpc::shared_ptr<yyy::i_example> example,
    int a,
    std::atomic<int>* completed,
    std::atomic<int>* errors)
{
    int result = 0;
    int ret = CO_AWAIT example->add(a, a + 1, result);
    if (ret == rpc::error::OK() && result == a + a + 1)
        completed->fetch_add(1, std::memory_order_relaxed);
    else
        errors->fetch_add(1, std::memory_order_relaxed);
    CO_RETURN;
}

// Exercises the send_queue_ready_ wakeup path in send_producer_loop.
// Spawning many concurrent calls on a single-threaded cooperative scheduler
// causes multiple outbound messages to queue before the send loop drains them;
// the loop must wake from send_queue_ready_ and process all of them without
// dropping or deadlocking.
template<class T> CORO_TASK(bool) coro_concurrent_queued_sends(T& lib)
{
    constexpr int kConcurrentCalls = 16;
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};

    auto example = lib.get_example();
    auto svc = lib.get_root_service();

    for (int i = 0; i < kConcurrentCalls; ++i)
        svc->spawn(do_add_call(example, i, &completed, &errors));

    while (completed.load(std::memory_order_acquire) + errors.load(std::memory_order_acquire) < kConcurrentCalls)
        CO_AWAIT svc->schedule();

    CORO_ASSERT_EQ(errors.load(), 0);
    CORO_ASSERT_EQ(completed.load(), kConcurrentCalls);
    CO_RETURN true;
}

TYPED_TEST(
    streaming_transport_regression_test,
    concurrent_queued_sends)
{
    run_coro_test(*this, [](auto& lib) { return coro_concurrent_queued_sends<TypeParam>(lib); });
}
#endif
