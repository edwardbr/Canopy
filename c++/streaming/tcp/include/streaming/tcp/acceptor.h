/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// streaming::tcp::acceptor — TCP listener that produces streaming::stream
// instances. Same shape in coroutine and blocking builds:
//
//   Coroutine builds: wraps a libcoro coro::net::tcp::server; accept() does
//     a polled co_await server->accept(poll_timeout) loop. stop() flips a
//     flag observed by that loop.
//
//   Blocking builds: owns a POSIX listen fd; accept() does poll(POLLIN)
//     with a short timeout and ::accept on readiness. stop() flips a flag
//     AND calls ::shutdown(listen_fd, SHUT_RD) so any thread blocked in
//     poll wakes immediately.
//
// Public API + bind options are exposed through streaming::tcp::endpoint so
// callers don't reference libcoro types directly. A coroutine-only
// convenience constructor accepting coro::net::socket_address is preserved
// for existing callers (tests use it).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <streaming/stream_acceptor.h>
#include <streaming/tcp/stream.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <coro/coro.hpp>
#  include <coro/net/tcp/server.hpp>
#endif

namespace streaming::tcp
{
    struct endpoint
    {
        std::string host = "127.0.0.1";
        uint16_t port = 0;
        bool ipv6 = false;
    };

    class acceptor : public ::streaming::stream_acceptor
    {
    public:
        explicit acceptor(endpoint ep);

#ifdef CANOPY_BUILD_COROUTINE
        // Convenience constructor for existing coroutine callers that hold
        // a libcoro socket_address. Wraps the address into an endpoint.
        acceptor(
            const coro::net::socket_address& endpoint_addr,
            coro::net::tcp::server::options opts = {});
#endif

        ~acceptor() override;

        bool init(std::shared_ptr<rpc::executor> executor) override;
        CORO_TASK(std::optional<std::shared_ptr<::streaming::stream>>) accept() override;
        void stop() override;

    private:
        endpoint endpoint_;
        std::atomic<bool> stop_{false};
        std::chrono::milliseconds poll_timeout_{std::chrono::milliseconds(10)};

#ifdef CANOPY_BUILD_COROUTINE
        std::shared_ptr<rpc::executor> executor_;
        coro::net::tcp::server::options opts_;
        std::shared_ptr<coro::net::tcp::server> server_;
#else
        int listen_fd_{-1};
#endif
    };
} // namespace streaming::tcp
