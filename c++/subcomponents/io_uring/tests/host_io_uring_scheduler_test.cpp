/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <array>
#include <chrono>

#include <coro/coro.hpp>
#include <io_uring/host_io_uring.h>
#include <io_uring/tcp.h>
#include <rpc/rpc.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
    uint16_t reserve_loopback_port() noexcept
    {
        const auto fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0)
            return 0;

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            ::close(fd);
            return 0;
        }

        socklen_t address_size = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) != 0)
        {
            ::close(fd);
            return 0;
        }

        const auto port = ntohs(address.sin_port);
        ::close(fd);
        return port;
    }
}

TEST(
    IoUringScheduler,
    CreatesControllerAndSubmitsNoOp)
{
    std::shared_ptr<rpc::io_uring::io_uring_scheduler> scheduler_owner;
    rpc::io_uring::linux_io_uring_handle::options options;
    options.queue_depth = 64;
    options.buffer_count = 8;
    options.buffer_size = 4096;
    options.register_buffers = false;
    options.fixed_file_count = 0;
    options.register_fixed_files = false;

    const auto err = rpc::io_uring::create_scheduler(scheduler_owner, options);
    if (err == rpc::error::NATIVE_IO_ERROR())
    {
        GTEST_SKIP() << "io_uring setup is not available on this kernel/runtime";
    }

    ASSERT_EQ(err, rpc::error::OK());
    ASSERT_TRUE(scheduler_owner);

    auto controller = scheduler_owner->get_controller();
    ASSERT_TRUE(controller);
    EXPECT_EQ(coro::sync_wait(controller->no_op()), rpc::error::OK());

    scheduler_owner->shutdown();
}

TEST(
    IoUringScheduler,
    TcpFallbackWorksWithoutFixedFiles)
{
    std::shared_ptr<rpc::io_uring::io_uring_scheduler> scheduler_owner;
    rpc::io_uring::linux_io_uring_handle::options options;
    options.queue_depth = 64;
    options.buffer_count = 8;
    options.buffer_size = 4096;
    options.register_buffers = false;
    options.fixed_file_count = 0;
    options.register_fixed_files = false;

    const auto err = rpc::io_uring::create_scheduler(scheduler_owner, options);
    if (err == rpc::error::NATIVE_IO_ERROR())
    {
        GTEST_SKIP() << "io_uring setup is not available on this kernel/runtime";
    }

    ASSERT_EQ(err, rpc::error::OK());
    ASSERT_TRUE(scheduler_owner);

    auto controller = scheduler_owner->get_controller();
    ASSERT_TRUE(controller);

    const auto port = reserve_loopback_port();
    if (port == 0)
    {
        scheduler_owner->shutdown();
        GTEST_SKIP() << "loopback TCP port reservation failed";
    }

    rpc::io_uring::acceptor acceptor(controller);
    ASSERT_EQ(coro::sync_wait(acceptor.listen_loopback(port)), rpc::error::OK());

    rpc::io_uring::connector connector(controller);
    auto client = coro::sync_wait(connector.connect_loopback_with_result(port));
    ASSERT_EQ(client.error_code, rpc::error::OK()) << client.native_result;
    ASSERT_TRUE(client.descriptor);

    auto server = coro::sync_wait(acceptor.accept_with_result());
    ASSERT_EQ(server.error_code, rpc::error::OK()) << server.native_result;
    ASSERT_TRUE(server.descriptor);

    std::array<uint8_t, 4> payload{1, 2, 3, 4};
    auto send_result = coro::sync_wait(controller->send(client.descriptor->get(), rpc::byte_span(payload)));
    ASSERT_EQ(send_result.error_code, rpc::error::OK()) << send_result.native_result;
    ASSERT_EQ(send_result.bytes_transferred, payload.size());

    std::array<uint8_t, 4> received{};
    auto receive_result = coro::sync_wait(
        controller->receive(server.descriptor->get(), rpc::mutable_byte_span(received), std::chrono::milliseconds{1000}));
    ASSERT_EQ(receive_result.error_code, rpc::error::OK()) << receive_result.native_result;
    ASSERT_EQ(receive_result.bytes_transferred, payload.size());
    EXPECT_EQ(received, payload);

    EXPECT_EQ(coro::sync_wait(client.descriptor->close()), rpc::error::OK());
    EXPECT_EQ(coro::sync_wait(server.descriptor->close()), rpc::error::OK());
    coro::sync_wait(acceptor.close());

    scheduler_owner->shutdown();
}
