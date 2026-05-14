/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <chrono>
#include <memory>

#include <coro/task.hpp>

#include <security/attestation/service.h>
#include <streaming/stream.h>

namespace streaming::attestation
{
    enum class handshake_role
    {
        client,
        server
    };

    struct stream_options
    {
        std::shared_ptr<canopy::security::attestation::attestation_service> service;
        uint64_t transcript_id{1};
        std::chrono::milliseconds handshake_timeout{std::chrono::milliseconds{5000}};
    };

    class stream final : public ::streaming::stream
    {
    public:
        stream(
            std::shared_ptr<::streaming::stream> underlying,
            stream_options options);
        ~stream() override;

        stream(const stream&) = delete;
        auto operator=(const stream&) -> stream& = delete;

        auto client_handshake() -> coro::task<bool>;
        auto server_handshake() -> coro::task<bool>;
        auto handshake(handshake_role role) -> coro::task<bool>;

        [[nodiscard]] auto security_context() const -> canopy::security::attestation::security_context;
        [[nodiscard]] auto handshake_complete() const -> bool;

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<
                coro::net::io_status,
                rpc::mutable_byte_span>> override;

        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override;
        [[nodiscard]] bool is_closed() const override;
        auto set_closed() -> coro::task<void> override;
        [[nodiscard]] auto get_peer_info() const -> peer_info override;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
} // namespace streaming::attestation
