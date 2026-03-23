// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <array>
#include <memory>
#include <string>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <rpc/rpc.h>

#include <streaming/stream.h>

namespace streaming::tls
{
    class context
    {
    public:
        context(const std::string& cert_file, const std::string& key_file);
        ~context();

        context(const context&) = delete;
        auto operator=(const context&) -> context& = delete;

        [[nodiscard]] auto get() const -> SSL_CTX* { return ctx_; }
        [[nodiscard]] auto is_valid() const -> bool { return ctx_ != nullptr; }

    private:
        SSL_CTX* ctx_{nullptr};
    };

    class client_context
    {
    public:
        explicit client_context(bool verify_peer = false);
        ~client_context();

        client_context(const client_context&) = delete;
        auto operator=(const client_context&) -> client_context& = delete;

        [[nodiscard]] auto get() const -> SSL_CTX* { return ctx_; }
        [[nodiscard]] auto is_valid() const -> bool { return ctx_ != nullptr; }

    private:
        SSL_CTX* ctx_{nullptr};
    };

    class stream : public ::streaming::stream
    {
    public:
        stream(std::shared_ptr<::streaming::stream> underlying, std::shared_ptr<context> tls_ctx);
        stream(std::shared_ptr<::streaming::stream> underlying, std::shared_ptr<client_context> client_ctx);
        ~stream() override;

        stream(const stream&) = delete;
        auto operator=(const stream&) -> stream& = delete;

        auto handshake() -> coro::task<bool>;
        auto client_handshake() -> coro::task<bool>;

        auto receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>> override;

        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override;
        [[nodiscard]] auto is_closed() const -> bool override { return closed_; }
        auto set_closed() -> coro::task<void> override;

        [[nodiscard]] auto get_peer_info() const -> peer_info override { return underlying_->get_peer_info(); }

    private:
        std::shared_ptr<::streaming::stream> underlying_;
        std::shared_ptr<context> tls_ctx_;
        std::shared_ptr<client_context> tls_client_ctx_;
        SSL* ssl_{nullptr};
        BIO* rbio_{nullptr};
        BIO* wbio_{nullptr};
        bool closed_{false};
        bool handshake_complete_{false};

        auto feed_rbio(std::chrono::milliseconds timeout) -> coro::task<coro::net::io_status>;
        auto drain_wbio() -> coro::task<bool>;
    };
} // namespace streaming::tls
