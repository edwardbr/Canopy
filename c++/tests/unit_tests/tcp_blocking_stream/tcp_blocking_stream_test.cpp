/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// End-to-end smoke test for the blocking-mode TCP stream + acceptor.
// Spins up an acceptor on 127.0.0.1, connects a peer via socketpair-style
// POSIX socket, exchanges bytes through the streaming::blocking::tcp::stream API.

#include <gtest/gtest.h>

#include <rpc/internal/executor/blocking_executor.h>

#include <streaming/tcp_blocking/acceptor.h>
#include <streaming/tcp_blocking/socket.h>
#include <streaming/tcp_blocking/stream.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <future>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    int connect_to(uint16_t port)
    {
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return -1;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    uint16_t pick_free_port()
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        uint16_t p = ntohs(addr.sin_port);
        ::close(fd);
        return p;
    }

    TEST(
        TcpBlockingStream,
        AcceptConnectExchangeBytes)
    {
        auto port = pick_free_port();
        ASSERT_NE(port, 0);

        auto exec = std::make_shared<rpc::blocking_executor>();
        streaming::blocking::tcp::endpoint ep;
        ep.host = "127.0.0.1";
        ep.port = port;
        auto acc = std::make_shared<streaming::blocking::tcp::acceptor>(ep);
        ASSERT_TRUE(acc->init(exec));

        // Drive the acceptor on a worker.
        std::shared_ptr<streaming::stream> server_stream;
        std::atomic<bool> got{false};
        ASSERT_TRUE(exec->post(
            [&]
            {
                auto maybe = acc->accept();
                if (maybe)
                    server_stream = std::move(*maybe);
                got.store(true, std::memory_order_release);
            }));

        int client_fd = -1;
        for (int i = 0; i < 200 && client_fd < 0; ++i)
        {
            client_fd = connect_to(port);
            if (client_fd < 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_GE(client_fd, 0);

        // Wait for acceptor to surface the stream.
        for (int i = 0; i < 200 && !got.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ASSERT_TRUE(got.load());
        ASSERT_NE(server_stream, nullptr);

        // Send from client fd -> read on server stream.
        const char hello[] = "hello-blocking-tcp";
        ASSERT_EQ(::write(client_fd, hello, sizeof(hello)), static_cast<ssize_t>(sizeof(hello)));

        std::vector<uint8_t> buf(64);
        auto [status, span]
            = server_stream->receive(rpc::mutable_byte_span(buf.data(), buf.size()), std::chrono::milliseconds(1000));
        ASSERT_TRUE(status.is_ok()) << "receive status type=" << static_cast<int>(status.type);
        ASSERT_EQ(span.size(), sizeof(hello));
        EXPECT_EQ(std::memcmp(span.data(), hello, sizeof(hello)), 0);

        // Send from server stream -> read on client fd.
        const char world[] = "world-blocking-tcp";
        auto send_status = server_stream->send(rpc::byte_span(reinterpret_cast<const uint8_t*>(world), sizeof(world)));
        ASSERT_TRUE(send_status.is_ok()) << "send status type=" << static_cast<int>(send_status.type);

        std::vector<uint8_t> rbuf(sizeof(world));
        ssize_t r = 0;
        while (r < static_cast<ssize_t>(sizeof(world)))
        {
            ssize_t n = ::read(client_fd, rbuf.data() + r, rbuf.size() - r);
            if (n <= 0)
                break;
            r += n;
        }
        EXPECT_EQ(r, static_cast<ssize_t>(sizeof(world)));
        EXPECT_EQ(std::memcmp(rbuf.data(), world, sizeof(world)), 0);

        // Tear down.
        server_stream->set_closed();
        acc->stop();
        ::close(client_fd);
        exec->shutdown();
    }
} // namespace
