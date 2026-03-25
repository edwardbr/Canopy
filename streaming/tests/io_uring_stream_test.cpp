// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// Direct io_uring::stream test — no transport layer, no RPC stack.
// Just: TCP accept/connect, wrap in io_uring::stream, send/receive raw bytes.

#include <gtest/gtest.h>

#include <streaming/io_uring/acceptor.h>
#include <streaming/io_uring/stream.h>
#include <coro/coro.hpp>
#include <coro/net/socket_address.hpp>
#include <coro/net/tcp/client.hpp>
#include <rpc/rpc.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string>
#include <string_view>
#include <unistd.h>

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
    auto recv_exact(
        streaming::stream& stm,
        rpc::mutable_byte_span buf,
        std::chrono::milliseconds timeout) -> coro::task<bool>
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

    auto make_loopback_address() -> canopy::network_config::ip_address
    {
        canopy::network_config::ip_address addr{};
        addr[0] = 127;
        addr[1] = 0;
        addr[2] = 0;
        addr[3] = 1;
        return addr;
    }

    auto connect_iouring_stream(
        std::shared_ptr<coro::scheduler> scheduler,
        uint16_t port) -> coro::task<std::shared_ptr<streaming::io_uring::stream>>
    {
        coro::net::tcp::client client(scheduler, coro::net::socket_address{"127.0.0.1", port});
        if (CO_AWAIT client.connect(5000ms) != coro::net::connect_status::connected)
        {
            co_return nullptr;
        }

        co_return std::make_shared<streaming::io_uring::stream>(std::move(client), scheduler);
    }
} // namespace

// ── Test 1: single ping-pong ──────────────────────────────────────────────────

TEST(
    IoUringStream,
    PingPong)
{
    auto server_scheduler = make_scheduler();
    auto client_scheduler = make_scheduler();
    bool server_ok = false;
    bool client_ok = false;
    std::atomic_bool server_ready = false;

    coro::sync_wait(
        coro::when_all(
            [&]() -> coro::task<void>
            {
                streaming::io_uring::acceptor acceptor(make_loopback_address(), test_port);
                if (!acceptor.init(server_scheduler))
                    co_return;
                server_ready = true;
                auto conn = co_await acceptor.accept();
                if (!conn.has_value())
                    co_return;
                auto stm = *conn;

                std::array<char, 64> buf{};
                bool got = co_await recv_exact(*stm, rpc::mutable_byte_span(buf.data(), 4), 2000ms);
                EXPECT_TRUE(got);
                EXPECT_EQ((std::string_view{buf.data(), 4}), "ping");

                std::string_view reply = "pong";
                auto st = co_await stm->send(rpc::byte_span(reply.data(), reply.size()));
                EXPECT_EQ(st.type, io_kind::ok);

                co_await stm->set_closed();
                acceptor.stop();
                server_ok = true;
            }(),
            [&]() -> coro::task<void>
            {
                while (!server_ready.load())
                    co_await client_scheduler->yield_for(1ms);

                auto stm = co_await connect_iouring_stream(client_scheduler, test_port);
                if (!stm)
                    co_return;

                std::string_view msg = "ping";
                auto st = co_await stm->send(rpc::byte_span(msg.data(), msg.size()));
                EXPECT_EQ(st.type, io_kind::ok);

                std::array<char, 64> buf{};
                bool got = co_await recv_exact(*stm, rpc::mutable_byte_span(buf.data(), 4), 2000ms);
                EXPECT_TRUE(got);
                EXPECT_EQ((std::string_view{buf.data(), 4}), "pong");

                co_await stm->set_closed();
                client_ok = true;
            }()));

    server_scheduler->shutdown();
    client_scheduler->shutdown();
    EXPECT_TRUE(server_ok);
    EXPECT_TRUE(client_ok);
}

// ── Test 2: many round trips ──────────────────────────────────────────────────

TEST(
    IoUringStream,
    ManyRoundTrips)
{
    auto server_scheduler = make_scheduler();
    auto client_scheduler = make_scheduler();
    constexpr int rounds = 500;
    int server_rounds = 0;
    int client_rounds = 0;
    std::atomic_bool server_ready = false;

    coro::sync_wait(
        coro::when_all(
            [&]() -> coro::task<void>
            {
                streaming::io_uring::acceptor acceptor(make_loopback_address(), static_cast<uint16_t>(test_port + 1));
                if (!acceptor.init(server_scheduler))
                    co_return;
                server_ready = true;
                auto conn = co_await acceptor.accept();
                if (!conn.has_value())
                    co_return;
                auto stm = *conn;

                std::array<char, 8> buf{};
                for (int i = 0; i < rounds; ++i)
                {
                    if (!co_await recv_exact(*stm, rpc::mutable_byte_span(buf.data(), 4), 2000ms))
                        break;
                    auto st = co_await stm->send(rpc::byte_span(buf.data(), 4));
                    if (st.type != io_kind::ok)
                        break;
                    ++server_rounds;
                }
                co_await stm->set_closed();
                acceptor.stop();
            }(),
            [&]() -> coro::task<void>
            {
                while (!server_ready.load())
                    co_await client_scheduler->yield_for(1ms);

                auto stm = co_await connect_iouring_stream(client_scheduler, static_cast<uint16_t>(test_port + 1));
                if (!stm)
                    co_return;

                std::array<char, 8> buf{};
                for (int i = 0; i < rounds; ++i)
                {
                    char payload[4] = {'r',
                        static_cast<char>('0' + (i % 10)),
                        static_cast<char>('0' + (i / 10 % 10)),
                        static_cast<char>('0' + (i / 100 % 10))};
                    auto st = co_await stm->send(rpc::byte_span(payload, 4));
                    if (st.type != io_kind::ok)
                        break;
                    if (!co_await recv_exact(*stm, rpc::mutable_byte_span(buf.data(), 4), 2000ms))
                        break;
                    ++client_rounds;
                }
                co_await stm->set_closed();
            }()));

    server_scheduler->shutdown();
    client_scheduler->shutdown();
    EXPECT_EQ(server_rounds, rounds);
    EXPECT_EQ(client_rounds, rounds);
}

// ── Test 3: receive timeout ───────────────────────────────────────────────────

TEST(
    IoUringStream,
    ReceiveTimeout)
{
    auto server_scheduler = make_scheduler();
    auto client_scheduler = make_scheduler();
    bool timed_out = false;
    std::atomic_bool server_ready = false;

    coro::sync_wait(
        coro::when_all(
            [&]() -> coro::task<void>
            {
                // Server accepts but never sends — client should time out on receive.
                streaming::io_uring::acceptor acceptor(make_loopback_address(), static_cast<uint16_t>(test_port + 2));
                if (!acceptor.init(server_scheduler))
                    co_return;
                server_ready = true;
                auto conn = co_await acceptor.accept();
                if (!conn.has_value())
                    co_return;
                auto stm = *conn;
                co_await server_scheduler->yield_for(std::chrono::milliseconds{500});
                co_await stm->set_closed();
                acceptor.stop();
            }(),
            [&]() -> coro::task<void>
            {
                while (!server_ready.load())
                    co_await client_scheduler->yield_for(1ms);

                auto stm = co_await connect_iouring_stream(client_scheduler, static_cast<uint16_t>(test_port + 2));
                if (!stm)
                    co_return;

                std::array<char, 64> buf{};
                auto [status, data] = co_await stm->receive(rpc::mutable_byte_span(buf.data(), buf.size()), 100ms);
                timed_out = (status.type == io_kind::timeout);
                co_await stm->set_closed();
            }()));

    server_scheduler->shutdown();
    client_scheduler->shutdown();
    EXPECT_TRUE(timed_out);
}

// ── Test 4: large payload ─────────────────────────────────────────────────────

TEST(
    IoUringStream,
    LargePayload)
{
    auto server_scheduler = make_scheduler();
    auto client_scheduler = make_scheduler();
    constexpr size_t payload_size = 256 * 1024;
    bool send_ok = false;
    bool recv_ok = false;
    std::atomic_bool server_ready = false;

    std::vector<char> send_buf(payload_size);
    for (size_t i = 0; i < payload_size; ++i)
        send_buf[i] = static_cast<char>(i & 0xff);

    coro::sync_wait(
        coro::when_all(
            [&]() -> coro::task<void>
            {
                streaming::io_uring::acceptor acceptor(make_loopback_address(), static_cast<uint16_t>(test_port + 3));
                if (!acceptor.init(server_scheduler))
                    co_return;
                server_ready = true;
                auto conn = co_await acceptor.accept();
                if (!conn.has_value())
                    co_return;
                auto stm = *conn;

                std::vector<char> recv_buf(payload_size);
                if (co_await recv_exact(*stm, rpc::mutable_byte_span(recv_buf.data(), recv_buf.size()), 5000ms))
                {
                    recv_ok = (recv_buf == send_buf);
                }
                co_await stm->set_closed();
                acceptor.stop();
            }(),
            [&]() -> coro::task<void>
            {
                while (!server_ready.load())
                    co_await client_scheduler->yield_for(1ms);

                auto stm = co_await connect_iouring_stream(client_scheduler, static_cast<uint16_t>(test_port + 3));
                if (!stm)
                    co_return;

                auto st = co_await stm->send(rpc::byte_span(send_buf.data(), send_buf.size()));
                send_ok = (st.type == io_kind::ok);
                co_await stm->set_closed();
            }()));

    server_scheduler->shutdown();
    client_scheduler->shutdown();
    EXPECT_TRUE(send_ok);
    EXPECT_TRUE(recv_ok);
}
