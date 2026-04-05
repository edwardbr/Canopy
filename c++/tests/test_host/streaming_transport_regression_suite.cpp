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
#endif
