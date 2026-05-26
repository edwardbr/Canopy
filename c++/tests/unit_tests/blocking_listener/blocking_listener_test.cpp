/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// End-to-end test of streaming::listener in blocking mode. Drives the full
// chain acceptor -> listener -> connection callback, ensuring the listener
// orchestrates accept loops via rpc::executor->post() and calls the user's
// connection callback when a peer connects.

#include <gtest/gtest.h>

#include <rpc/internal/executor/blocking_executor.h>
#include <rpc/internal/polyfill/event.h>

#include <streaming/listener.h>
#include <streaming/tcp/acceptor.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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

    TEST(BlockingListener, AcceptsConnectionsAndInvokesCallback)
    {
        auto port = pick_free_port();
        ASSERT_NE(port, 0);

        auto exec = std::make_shared<rpc::blocking_executor>();

        // The listener needs to call service->get_executor() and SPAWN(). Use
        // root_service constructed with our executor.
        auto zone_addr = rpc::to_zone_address(
            rpc::zone_address_args(
                rpc::default_values::version_3,
                rpc::address_type::local,
                0,
                {},
                rpc::default_values::default_subnet_size_bits,
                1,
                rpc::default_values::default_object_id_size_bits,
                0,
                {}));
        std::shared_ptr<rpc::service> svc
            = rpc::root_service::create("blocking_listener_test", rpc::zone{zone_addr}, exec);

        streaming::tcp::endpoint ep;
        ep.host = "127.0.0.1";
        ep.port = port;
        auto acc = std::make_shared<streaming::tcp::acceptor>(ep);

        std::atomic<int> connections_observed{0};
        rpc::event first_seen;

        streaming::listener::connection_callback cb
            = [&](const std::string& /*name*/,
                  std::shared_ptr<rpc::service> /*svc*/,
                  std::shared_ptr<streaming::stream> stm)
        {
            ASSERT_NE(stm, nullptr);
            if (connections_observed.fetch_add(1, std::memory_order_acq_rel) == 0)
                first_seen.set();
            stm->set_closed();
        };

        streaming::listener lst("blocking_listener_test", acc, std::move(cb));
        ASSERT_TRUE(lst.start_listening(svc));

        int client_fd = -1;
        for (int i = 0; i < 200 && client_fd < 0; ++i)
        {
            client_fd = connect_to(port);
            if (client_fd < 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_GE(client_fd, 0);

        first_seen.wait();
        EXPECT_GE(connections_observed.load(), 1);

        lst.stop_listening();
        ::close(client_fd);
        exec->shutdown();
    }
} // namespace
