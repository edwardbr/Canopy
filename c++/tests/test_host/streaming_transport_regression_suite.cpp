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
#  include <connection_factory/spsc_queue.h>
#  include <transport/tests/streaming_layered_spsc/setup.h>
#  include <transport/tests/streaming_layered_tcp_coroutine/setup.h>
#  include <transport/tests/streaming_tcp_coroutine/setup.h>
#  include <transport/tests/streaming_spsc/setup.h>
#endif

#include "type_test_fixture.h"

using namespace marshalled_tests;

template<class T> using streaming_transport_regression_test = type_test<T>;

#ifdef CANOPY_BUILD_COROUTINE
namespace
{
    template<class Buffer> rpc::byte_span as_byte_span(const Buffer& buffer)
    {
        return rpc::byte_span{reinterpret_cast<const char*>(buffer.data()), buffer.size()};
    }

    CORO_TASK(bool) coro_malformed_init_message_disconnects_transport(std::shared_ptr<coro::scheduler> scheduler)
    {
        auto zone_id = rpc::DEFAULT_PREFIX;
        (void)zone_id.set_subnet(zone_id.get_subnet() + 4096);
        auto service = rpc::root_service::create("bad_init_transport_test", zone_id, scheduler);

        auto queues = rpc::spsc_queue::queue_pair::create();
        auto peer_stream = std::make_shared<::streaming::spsc_queue::stream>(
            queues.connect_to_accept, queues.accept_to_connect, scheduler);
        auto transport_stream = std::make_shared<::streaming::spsc_queue::stream>(
            queues.accept_to_connect, queues.connect_to_accept, scheduler);

        auto handler_called = std::make_shared<std::atomic_bool>(false);
        auto transport = rpc::stream_transport::create(
            "bad_init_responder",
            service,
            std::move(transport_stream),
            [handler_called](rpc::connection_settings, std::shared_ptr<rpc::service>, std::shared_ptr<rpc::transport>)
                -> CORO_TASK(rpc::connection_handler_result)
            {
                handler_called->store(true, std::memory_order_release);
                CO_RETURN rpc::connection_handler_result{rpc::error::OK(), {}};
            },
            rpc::stream_transport::stream_transport_options{
                .call_timeout = std::chrono::milliseconds{0},
                .call_timeout_sweep = std::chrono::milliseconds{0},
                .shutdown_timeout = std::chrono::milliseconds{1},
            });

        CO_AWAIT service->schedule();

        rpc::stream_transport::envelope_payload payload{
            FLD(payload_fingerprint) rpc::id<rpc::stream_transport::init_client_channel_send>::get(rpc::get_version()),
            FLD(payload) std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef},
        };
        auto payload_bytes = rpc::to_yas_binary(payload);
        rpc::stream_transport::envelope_prefix prefix{
            FLD(version) rpc::get_version(),
            FLD(direction) rpc::stream_transport::message_direction::send,
            FLD(sequence_number) uint64_t{1},
            FLD(payload_size) payload_bytes.size(),
        };
        auto prefix_bytes = rpc::to_yas_binary(prefix);

        auto send_status = CO_AWAIT peer_stream->send(as_byte_span(prefix_bytes));
        CORO_ASSERT_EQ(send_status.is_ok(), true);
        send_status = CO_AWAIT peer_stream->send(as_byte_span(payload_bytes));
        CORO_ASSERT_EQ(send_status.is_ok(), true);

        bool saw_disconnect_state = false;
        for (int i = 0; i < 2000 && transport->get_status() != rpc::transport_status::DISCONNECTED; ++i)
        {
            if (transport->get_status() >= rpc::transport_status::DISCONNECTING)
                saw_disconnect_state = true;
            CO_AWAIT service->schedule();
        }
        saw_disconnect_state = saw_disconnect_state || transport->get_status() >= rpc::transport_status::DISCONNECTING;

        CORO_ASSERT_EQ(handler_called->load(std::memory_order_acquire), false);
        CORO_ASSERT_EQ(saw_disconnect_state, true);
        CORO_ASSERT_EQ(transport->get_status(), rpc::transport_status::DISCONNECTED);

        for (int i = 0; i < 8; ++i)
            CO_AWAIT service->schedule();

        transport.reset();
        peer_stream.reset();
        service.reset();
        CO_RETURN true;
    }
}

TEST(
    StreamingTransportBadMessage,
    MalformedInitClientChannelSendDisconnects)
{
    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
            .pool = coro::thread_pool::options{.thread_count = 1}}));

    std::atomic_bool done{false};
    bool passed = false;
    auto runner = [&]() -> coro::task<void>
    {
        passed = CO_AWAIT coro_malformed_init_message_disconnects_transport(scheduler);
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
}

// Keep this suite on the active TCP coroutine and SPSC streaming paths.
using streaming_transport_regression_implementations = ::testing::Types<
    streaming_tcp_coroutine_setup<false, false, false>,
    streaming_spsc_setup<false, false, false>,
    streaming_layered_tcp_coroutine_setup<false, false, false>,
    streaming_layered_spsc_setup<false, false, false>>;

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
