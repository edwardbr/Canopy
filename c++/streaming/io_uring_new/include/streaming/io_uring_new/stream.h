/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include <io_uring/direct_descriptor.h>
#include <streaming/stream.h>

namespace streaming::io_uring_new
{
    class stream : public streaming::stream
    {
    public:
        // Adapts an io_uring direct descriptor into the generic streaming API.
        // The descriptor may come from any future byte-oriented io_uring
        // resource; this class deliberately does not know which.
        explicit stream(
            std::shared_ptr<rpc::io_uring::direct_descriptor> descriptor,
            uint16_t peer_port = 0) noexcept;

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<
                coro::net::io_status,
                rpc::mutable_byte_span>> override;

        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override;

        [[nodiscard]] bool is_closed() const override;
        auto set_closed() -> coro::task<void> override;
        [[nodiscard]] streaming::peer_info get_peer_info() const override;

    private:
        std::shared_ptr<rpc::io_uring::direct_descriptor> descriptor_;
        uint16_t peer_port_{0};
        std::atomic<bool> closed_{false};
    };

    struct stream_result
    {
        int error_code{rpc::error::OK()};
        int32_t native_result{0};
        uint32_t cqe_flags{0};
        std::shared_ptr<streaming::stream> connection;
    };

    [[nodiscard]] std::shared_ptr<streaming::stream> make_stream(
        std::shared_ptr<rpc::io_uring::direct_descriptor> descriptor,
        uint16_t peer_port = 0) noexcept;

    [[nodiscard]] stream_result make_stream_result(
        const rpc::io_uring::direct_descriptor_result& result,
        uint16_t peer_port = 0) noexcept;
} // namespace streaming::io_uring_new
