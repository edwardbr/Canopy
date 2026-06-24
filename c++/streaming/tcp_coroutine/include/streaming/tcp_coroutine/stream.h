/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <io_uring/direct_descriptor.h>
#include <tcp_coroutine_stream/tcp_coroutine_stream_config.h>
#include <streaming/stream.h>
#include <streaming/tcp/namespace.h>

namespace streaming::coroutine::tcp
{
    using endpoint = ::rpc::tcp_coroutine_stream::endpoint;
    using receive_timeout_strategy = ::rpc::tcp_coroutine_stream::receive_timeout_strategy;
    using stream_options = ::rpc::tcp_coroutine_stream::stream_options;

    class stream : public streaming::stream
    {
    public:
        using options = stream_options;

        // Adapts an io_uring direct descriptor into the generic streaming API.
        // The descriptor may come from any future byte-oriented io_uring
        // resource; this class deliberately does not know which.
        stream(
            std::shared_ptr<rpc::io_uring::direct_descriptor> descriptor,
            uint16_t peer_port,
            options stream_options,
            rpc::executor_ptr executor) noexcept;
        ~stream() override;

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> CORO_TASK(::streaming::receive_result) override;

        auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) override;

        [[nodiscard]] bool is_closed() const override;
        auto set_closed() -> CORO_TASK(void) override;
        [[nodiscard]] streaming::peer_info get_peer_info() const override;

    private:
        void close_detached() noexcept;

        const std::shared_ptr<rpc::io_uring::direct_descriptor> descriptor_;
        const uint16_t peer_port_{0};
        const options options_;
        const rpc::executor_ptr executor_;
        std::atomic<bool> closed_{false};
    };

    [[nodiscard]] stream::options default_stream_options() noexcept;

    struct stream_result
    {
        int error_code{rpc::error::OK()};
        int32_t native_result{0};
        uint32_t cqe_flags{0};
        std::shared_ptr<streaming::stream> connection;
    };

    [[nodiscard]] std::shared_ptr<streaming::stream> make_stream(
        std::shared_ptr<rpc::io_uring::direct_descriptor> descriptor,
        uint16_t peer_port,
        stream::options stream_options,
        rpc::executor_ptr executor) noexcept;

    [[nodiscard]] stream_result make_stream_result(
        const rpc::io_uring::direct_descriptor_result& result,
        uint16_t peer_port,
        stream::options stream_options,
        rpc::executor_ptr executor) noexcept;
} // namespace streaming::coroutine::tcp
