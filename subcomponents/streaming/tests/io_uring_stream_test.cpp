// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// Direct io_uring_tcp_stream test — no transport layer, no RPC stack.
// Just: TCP accept/connect, wrap in io_uring_tcp_stream, send/receive raw bytes.

#include <gtest/gtest.h>

#if defined(__linux__)

#include <streaming/io_uring_tcp_stream.h>
#include <coro/coro.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>

using namespace std::chrono_literals;
using io_kind = coro::net::io_status::kind;

namespace
{
    constexpr uint16_t test_port = 19950;

    std::shared_ptr<coro::scheduler> make_scheduler()
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                .pool = coro::thread_pool::options{.thread_count = 2},
                .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));
    }

    // Receive exactly buf.size() bytes, looping on partial reads.
    // Returns false on timeout or error.
    auto recv_exact(streaming::io_uring_tcp_stream& stm, std::span<char> buf, std::chrono::milliseconds timeout)
        -> coro::task<bool>
    {
        size_t received = 0;
        while (received < buf.size())
        {
            auto [status, data] = co_await stm.receive(buf.subspan(received), timeout);
            if (status.type == io_kind::ok)
            {
                received += data.size();
            }
            else
            {
                co_return false;
            }
        }
        co_return true;
    }
} // namespace

// ── Test 1: single ping-pong ──────────────────────────────────────────────────

TEST(IoUringStream, PingPong)
{
    auto scheduler = make_scheduler();
    bool server_ok = false;
    bool client_ok = false;

    coro::sync_wait(coro::when_all(
        [&]() -> coro::task<void>
        {
            coro::net::tcp::server server(scheduler, coro::net::socket_address{"127.0.0.1", test_port});
            auto conn = co_await server.accept(5000ms);
            if (!conn.has_value())
                co_return;
            auto stm = std::make_shared<streaming::io_uring_tcp_stream>(std::move(*conn), scheduler);

            std::array<char, 64> buf{};
            bool got = co_await recv_exact(*stm, std::span(buf.data(), 4), 2000ms);
            EXPECT_TRUE(got);
            EXPECT_EQ((std::string_view{buf.data(), 4}), "ping");

            std::string_view reply = "pong";
            auto st = co_await stm->send(std::span<const char>(reply.data(), reply.size()));
            EXPECT_EQ(st.type, io_kind::ok);

            stm->set_closed();
            server_ok = true;
        }(),
        [&]() -> coro::task<void>
        {
            coro::net::tcp::client client(scheduler, coro::net::socket_address{"127.0.0.1", test_port});
            auto cs = co_await client.connect(5000ms);
            if (cs != coro::net::connect_status::connected)
                co_return;
            auto stm = std::make_shared<streaming::io_uring_tcp_stream>(std::move(client), scheduler);

            std::string_view msg = "ping";
            auto st = co_await stm->send(std::span<const char>(msg.data(), msg.size()));
            EXPECT_EQ(st.type, io_kind::ok);

            std::array<char, 64> buf{};
            bool got = co_await recv_exact(*stm, std::span(buf.data(), 4), 2000ms);
            EXPECT_TRUE(got);
            EXPECT_EQ((std::string_view{buf.data(), 4}), "pong");

            stm->set_closed();
            client_ok = true;
        }()));

    scheduler->shutdown();
    EXPECT_TRUE(server_ok);
    EXPECT_TRUE(client_ok);
}

// ── Test 2: many round trips ──────────────────────────────────────────────────

TEST(IoUringStream, ManyRoundTrips)
{
    auto scheduler = make_scheduler();
    constexpr int rounds = 500;
    int server_rounds = 0;
    int client_rounds = 0;

    coro::sync_wait(coro::when_all(
        [&]() -> coro::task<void>
        {
            coro::net::tcp::server server(
                scheduler, coro::net::socket_address{"127.0.0.1", static_cast<uint16_t>(test_port + 1)});
            auto conn = co_await server.accept(5000ms);
            if (!conn.has_value())
                co_return;
            auto stm = std::make_shared<streaming::io_uring_tcp_stream>(std::move(*conn), scheduler);

            std::array<char, 8> buf{};
            for (int i = 0; i < rounds; ++i)
            {
                if (!co_await recv_exact(*stm, std::span(buf.data(), 4), 2000ms))
                    break;
                auto st = co_await stm->send(std::span<const char>(buf.data(), 4));
                if (st.type != io_kind::ok)
                    break;
                ++server_rounds;
            }
            stm->set_closed();
        }(),
        [&]() -> coro::task<void>
        {
            coro::net::tcp::client client(
                scheduler, coro::net::socket_address{"127.0.0.1", static_cast<uint16_t>(test_port + 1)});
            if (co_await client.connect(5000ms) != coro::net::connect_status::connected)
                co_return;
            auto stm = std::make_shared<streaming::io_uring_tcp_stream>(std::move(client), scheduler);

            std::array<char, 8> buf{};
            for (int i = 0; i < rounds; ++i)
            {
                char payload[4] = {'r',
                    static_cast<char>('0' + (i % 10)),
                    static_cast<char>('0' + (i / 10 % 10)),
                    static_cast<char>('0' + (i / 100 % 10))};
                auto st = co_await stm->send(std::span<const char>(payload, 4));
                if (st.type != io_kind::ok)
                    break;
                if (!co_await recv_exact(*stm, std::span(buf.data(), 4), 2000ms))
                    break;
                ++client_rounds;
            }
            stm->set_closed();
        }()));

    scheduler->shutdown();
    EXPECT_EQ(server_rounds, rounds);
    EXPECT_EQ(client_rounds, rounds);
}

// ── Test 3: receive timeout ───────────────────────────────────────────────────

TEST(IoUringStream, ReceiveTimeout)
{
    auto scheduler = make_scheduler();
    bool timed_out = false;

    coro::sync_wait(coro::when_all(
        [&]() -> coro::task<void>
        {
            // Server accepts but never sends — client should time out on receive.
            coro::net::tcp::server server(
                scheduler, coro::net::socket_address{"127.0.0.1", static_cast<uint16_t>(test_port + 2)});
            auto conn = co_await server.accept(5000ms);
            if (!conn.has_value())
                co_return;
            auto stm = std::make_shared<streaming::io_uring_tcp_stream>(std::move(*conn), scheduler);
            co_await scheduler->yield_for(std::chrono::milliseconds{500});
            stm->set_closed();
        }(),
        [&]() -> coro::task<void>
        {
            coro::net::tcp::client client(
                scheduler, coro::net::socket_address{"127.0.0.1", static_cast<uint16_t>(test_port + 2)});
            if (co_await client.connect(5000ms) != coro::net::connect_status::connected)
                co_return;
            auto stm = std::make_shared<streaming::io_uring_tcp_stream>(std::move(client), scheduler);

            std::array<char, 64> buf{};
            auto [status, data] = co_await stm->receive(buf, 100ms);
            timed_out = (status.type == io_kind::timeout);
            stm->set_closed();
        }()));

    scheduler->shutdown();
    EXPECT_TRUE(timed_out);
}

// ── Test 4: large payload ─────────────────────────────────────────────────────

TEST(IoUringStream, LargePayload)
{
    auto scheduler = make_scheduler();
    constexpr size_t payload_size = 256 * 1024;
    bool send_ok = false;
    bool recv_ok = false;

    std::vector<char> send_buf(payload_size);
    for (size_t i = 0; i < payload_size; ++i)
        send_buf[i] = static_cast<char>(i & 0xff);

    coro::sync_wait(coro::when_all(
        [&]() -> coro::task<void>
        {
            coro::net::tcp::server server(
                scheduler, coro::net::socket_address{"127.0.0.1", static_cast<uint16_t>(test_port + 3)});
            auto conn = co_await server.accept(5000ms);
            if (!conn.has_value())
                co_return;
            auto stm = std::make_shared<streaming::io_uring_tcp_stream>(std::move(*conn), scheduler);

            std::vector<char> recv_buf(payload_size);
            if (co_await recv_exact(*stm, recv_buf, 5000ms))
            {
                recv_ok = (recv_buf == send_buf);
            }
            stm->set_closed();
        }(),
        [&]() -> coro::task<void>
        {
            coro::net::tcp::client client(
                scheduler, coro::net::socket_address{"127.0.0.1", static_cast<uint16_t>(test_port + 3)});
            if (co_await client.connect(5000ms) != coro::net::connect_status::connected)
                co_return;
            auto stm = std::make_shared<streaming::io_uring_tcp_stream>(std::move(client), scheduler);

            auto st = co_await stm->send(send_buf);
            send_ok = (st.type == io_kind::ok);
            stm->set_closed();
        }()));

    scheduler->shutdown();
    EXPECT_TRUE(send_ok);
    EXPECT_TRUE(recv_ok);
}

#endif // defined(__linux__)

// libstreaming.so uses rpc_log via the RPC macro infrastructure. Tests that
// do not link against the full RPC stack must provide this stub.
void rpc_log(int /*level*/, const char* /*str*/, size_t /*sz*/) { }
