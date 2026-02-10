// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tls_stream.h - TLS stream wrapper using OpenSSL
#pragma once

#include "stream.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <memory>
#include <string>

namespace websocket_demo
{
    namespace v1
    {
        // TLS context holder (shared across connections)
        class tls_context
        {
        public:
            tls_context(const std::string& cert_file, const std::string& key_file);
            ~tls_context();

            // Non-copyable
            tls_context(const tls_context&) = delete;
            tls_context& operator=(const tls_context&) = delete;

            SSL_CTX* get() const { return ctx_; }

            bool is_valid() const { return ctx_ != nullptr; }

        private:
            SSL_CTX* ctx_{nullptr};
        };

        // TLS stream that wraps a TCP connection
        class tls_stream : public stream
        {
        public:
            tls_stream(coro::net::tcp::client&& client, std::shared_ptr<tls_context> tls_ctx);
            ~tls_stream();

            // Non-copyable
            tls_stream(const tls_stream&) = delete;
            tls_stream& operator=(const tls_stream&) = delete;

            // Perform TLS handshake
            auto handshake() -> coro::task<bool>;

            // Stream interface
            auto poll(coro::poll_op op, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
                -> coro::task<coro::poll_status> override;

            auto recv(std::span<char> buffer) -> std::pair<coro::net::recv_status, std::span<char>> override;

            auto send(std::span<const char> buffer) -> std::pair<coro::net::send_status, std::span<const char>> override;

            bool is_closed() const override { return closed_; }

            void set_closed() override { closed_ = true; }

        private:
            coro::net::tcp::client client_;
            std::shared_ptr<tls_context> tls_ctx_;
            SSL* ssl_{nullptr};
            bool closed_{false};
            bool handshake_complete_{false};

            // Handle SSL errors and determine if we need to retry
            coro::poll_op get_poll_op_for_ssl_error(int ssl_error);
        };

    } // namespace websocket_demo
}
