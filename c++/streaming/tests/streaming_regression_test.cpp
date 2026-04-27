// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <gtest/gtest.h>

#include <streaming/listener.h>
#include <streaming/spsc_wrapping/stream.h>
#include <coro/coro.hpp>
#include <rpc/rpc.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>

using namespace std::chrono_literals;

namespace
{
    std::shared_ptr<coro::scheduler> make_scheduler()
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                .pool = coro::thread_pool::options{.thread_count = 2},
                .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));
    }

    class send_failure_stream : public streaming::stream
    {
    public:
        explicit send_failure_stream(std::shared_ptr<coro::scheduler> scheduler)
            : scheduler_(std::move(scheduler))
        {
        }

        auto receive(
            rpc::mutable_byte_span,
            std::chrono::milliseconds timeout)
            -> coro::task<std::pair<
                coro::net::io_status,
                rpc::mutable_byte_span>> override
        {
            if (closed_.load(std::memory_order_acquire))
                co_return std::pair{
                    coro::net::io_status{.type = coro::net::io_status::kind::closed}, rpc::mutable_byte_span{}};

            co_await scheduler_->yield_for(std::min(timeout, 1ms));

            if (closed_.load(std::memory_order_acquire))
                co_return std::pair{
                    coro::net::io_status{.type = coro::net::io_status::kind::closed}, rpc::mutable_byte_span{}};

            co_return std::pair{
                coro::net::io_status{.type = coro::net::io_status::kind::timeout}, rpc::mutable_byte_span{}};
        }

        auto send(rpc::byte_span) -> coro::task<coro::net::io_status> override
        {
            send_calls_.fetch_add(1, std::memory_order_relaxed);
            co_return coro::net::io_status{.type = coro::net::io_status::kind::native, .native_code = EPIPE};
        }

        bool is_closed() const override { return closed_.load(std::memory_order_acquire); }

        auto set_closed() -> coro::task<void> override
        {
            set_closed_called_.store(true, std::memory_order_release);
            closed_.store(true, std::memory_order_release);
            co_return;
        }

        auto get_peer_info() const -> streaming::peer_info override { return {}; }

        [[nodiscard]] bool set_closed_called() const { return set_closed_called_.load(std::memory_order_acquire); }

        [[nodiscard]] int send_calls() const { return send_calls_.load(std::memory_order_acquire); }

    private:
        std::shared_ptr<coro::scheduler> scheduler_;
        std::atomic<bool> closed_{false};
        std::atomic<bool> set_closed_called_{false};
        std::atomic<int> send_calls_{0};
    };

    class late_accept_stream : public streaming::stream
    {
    public:
        auto receive(
            rpc::mutable_byte_span,
            std::chrono::milliseconds)
            -> coro::task<std::pair<
                coro::net::io_status,
                rpc::mutable_byte_span>> override
        {
            co_return std::pair{coro::net::io_status{.type = coro::net::io_status::kind::closed}, rpc::mutable_byte_span{}};
        }

        auto send(rpc::byte_span) -> coro::task<coro::net::io_status> override
        {
            co_return coro::net::io_status{.type = coro::net::io_status::kind::ok};
        }

        bool is_closed() const override { return closed_.load(std::memory_order_acquire); }

        auto set_closed() -> coro::task<void> override
        {
            closed_.store(true, std::memory_order_release);
            co_return;
        }

        auto get_peer_info() const -> streaming::peer_info override { return {}; }

    private:
        std::atomic<bool> closed_{false};
    };

    class late_accept_acceptor : public streaming::stream_acceptor
    {
    public:
        explicit late_accept_acceptor(std::shared_ptr<late_accept_stream> accepted_stream)
            : accepted_stream_(std::move(accepted_stream))
        {
        }

        bool init(std::shared_ptr<coro::scheduler> scheduler) override
        {
            scheduler_ = std::move(scheduler);
            stopped_.store(false, std::memory_order_release);
            released_.store(false, std::memory_order_release);
            returned_once_.store(false, std::memory_order_release);
            return true;
        }

        CORO_TASK(std::optional<std::shared_ptr<streaming::stream>>) accept() override
        {
            while (!released_.load(std::memory_order_acquire))
                co_await scheduler_->yield_for(1ms);

            if (returned_once_.exchange(true, std::memory_order_acq_rel))
                co_return std::nullopt;

            co_return std::static_pointer_cast<streaming::stream>(accepted_stream_);
        }

        void stop() override
        {
            stopped_.store(true, std::memory_order_release);
            released_.store(true, std::memory_order_release);
        }

        [[nodiscard]] bool stopped() const { return stopped_.load(std::memory_order_acquire); }

    private:
        std::shared_ptr<coro::scheduler> scheduler_;
        std::shared_ptr<late_accept_stream> accepted_stream_;
        std::atomic<bool> stopped_{false};
        std::atomic<bool> released_{false};
        std::atomic<bool> returned_once_{false};
    };
} // namespace

TEST(
    SpscWrappingStream,
    PropagatesUnderlyingSendFailure)
{
    auto scheduler = make_scheduler();
    auto underlying = std::make_shared<send_failure_stream>(scheduler);
    auto wrapper = streaming::spsc_wrapping::stream::create(underlying, scheduler);

    auto first_status = coro::sync_wait(wrapper->send(rpc::byte_span{"abc", 3}));
    EXPECT_TRUE(first_status.is_ok());

    coro::sync_wait(scheduler->yield_for(20ms));

    auto second_status = coro::sync_wait(wrapper->send(rpc::byte_span{"x", 1}));
    EXPECT_EQ(second_status.type, coro::net::io_status::kind::native);
    EXPECT_EQ(second_status.native_code, EPIPE);
    EXPECT_GE(underlying->send_calls(), 1);

    coro::sync_wait(wrapper->set_closed());
    scheduler->shutdown();
}

TEST(
    SpscWrappingStream,
    SetClosedAllowsWrapperExpiry)
{
    auto scheduler = make_scheduler();
    auto underlying = std::make_shared<send_failure_stream>(scheduler);
    std::weak_ptr<streaming::spsc_wrapping::stream> wrapper_weak;

    {
        auto wrapper = streaming::spsc_wrapping::stream::create(underlying, scheduler);
        wrapper_weak = wrapper;
        coro::sync_wait(wrapper->set_closed());
    }

    EXPECT_TRUE(wrapper_weak.expired());
    scheduler->shutdown();
}

TEST(
    Listener,
    StopDropsLateAcceptedConnection)
{
    auto scheduler = make_scheduler();
    auto zone_id = rpc::DEFAULT_PREFIX;
    auto service = rpc::root_service::create("listener-test", zone_id, scheduler);
    auto accepted_stream = std::make_shared<late_accept_stream>();
    auto acceptor = std::make_shared<late_accept_acceptor>(accepted_stream);
    std::atomic<int> callback_calls{0};

    auto listener = std::make_shared<streaming::listener>(
        "test-listener",
        acceptor,
        [&](const std::string&, std::shared_ptr<rpc::service>, std::shared_ptr<streaming::stream>) -> CORO_TASK(void)
        {
            callback_calls.fetch_add(1, std::memory_order_relaxed);
            co_return;
        });

    coro::sync_wait(
        [&]() -> coro::task<void>
        {
            EXPECT_TRUE(CO_AWAIT listener->start_listening_async(service));
            CO_AWAIT listener->stop_listening();
        }());

    EXPECT_TRUE(acceptor->stopped());
    EXPECT_TRUE(accepted_stream->is_closed());
    EXPECT_EQ(callback_calls.load(std::memory_order_acquire), 0);
    scheduler->shutdown();
}
