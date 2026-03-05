// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tls_stream.h - TLS stream wrapper using OpenSSL memory BIOs
#pragma once

#include <memory>
#include <string>
#include <array>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <rpc/rpc.h>

#include "stream.h"

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

        // TLS stream that wraps any stream using OpenSSL memory BIOs.
        // Has no knowledge of sockets — all I/O is delegated to the underlying stream.
        class tls_stream : public stream
        {
        public:
            tls_stream(std::shared_ptr<stream> underlying, std::shared_ptr<tls_context> tls_ctx);
            ~tls_stream();

            // Non-copyable
            tls_stream(const tls_stream&) = delete;
            tls_stream& operator=(const tls_stream&) = delete;

            // Perform TLS handshake
            auto handshake() -> coro::task<bool>;

            // Stream interface
            auto poll(coro::poll_op op, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
                -> coro::task<coro::poll_status> override;

            auto recv(std::span<char> buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
                -> coro::task<std::pair<coro::net::io_status, std::span<char>>> override;

            auto send(std::span<const char> buffer) -> std::pair<coro::net::io_status, std::span<const char>> override;

            bool is_closed() const override { return closed_; }

            void set_closed() override { closed_ = true; }

            peer_info get_peer_info() const override { return underlying_->get_peer_info(); }

        private:
            std::shared_ptr<stream> underlying_;
            std::shared_ptr<tls_context> tls_ctx_;
            SSL* ssl_{nullptr};
            BIO* rbio_{nullptr}; // network → SSL (we write raw bytes here)
            BIO* wbio_{nullptr}; // SSL → network (we drain this to underlying_->send)
            bool closed_{false};
            bool handshake_complete_{false};

            // Read raw bytes from underlying stream and feed them into rbio.
            auto feed_rbio(std::chrono::milliseconds timeout) -> coro::task<bool>;

            // Drain any pending SSL output from wbio to the underlying stream.
            // Returns false if the underlying stream errored.
            bool flush_wbio();
        };

    } // namespace websocket_demo
}
