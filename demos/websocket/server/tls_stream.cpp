// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tls_stream.cpp - TLS stream implementation
#include "tls_stream.h"
#include <iostream>

namespace websocket_demo
{
    namespace v1
    {
        // TLS context implementation
        tls_context::tls_context(const std::string& cert_file, const std::string& key_file)
        {
            // Create SSL context for TLS server
            ctx_ = SSL_CTX_new(TLS_server_method());
            if (!ctx_)
            {
                std::cerr << "Failed to create SSL context" << std::endl;
                ERR_print_errors_fp(stderr);
                return;
            }

            // Set minimum TLS version to 1.2
            SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

            // Load certificate
            if (SSL_CTX_use_certificate_file(ctx_, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0)
            {
                std::cerr << "Failed to load certificate: " << cert_file << std::endl;
                ERR_print_errors_fp(stderr);
                SSL_CTX_free(ctx_);
                ctx_ = nullptr;
                return;
            }

            // Load private key
            if (SSL_CTX_use_PrivateKey_file(ctx_, key_file.c_str(), SSL_FILETYPE_PEM) <= 0)
            {
                std::cerr << "Failed to load private key: " << key_file << std::endl;
                ERR_print_errors_fp(stderr);
                SSL_CTX_free(ctx_);
                ctx_ = nullptr;
                return;
            }

            // Verify private key matches certificate
            if (!SSL_CTX_check_private_key(ctx_))
            {
                std::cerr << "Private key does not match certificate" << std::endl;
                ERR_print_errors_fp(stderr);
                SSL_CTX_free(ctx_);
                ctx_ = nullptr;
                return;
            }

            std::cout << "TLS context initialized with certificate: " << cert_file << std::endl;
        }

        tls_context::~tls_context()
        {
            if (ctx_)
            {
                SSL_CTX_free(ctx_);
            }
        }

        // TLS stream implementation
        tls_stream::tls_stream(coro::net::tcp::client&& client, std::shared_ptr<tls_context> tls_ctx)
            : client_(std::move(client))
            , tls_ctx_(std::move(tls_ctx))
        {
            if (tls_ctx_ && tls_ctx_->is_valid())
            {
                ssl_ = SSL_new(tls_ctx_->get());
                if (ssl_)
                {
                    // Set the file descriptor for the SSL connection
                    SSL_set_fd(ssl_, client_.socket().native_handle());
                }
            }
        }

        tls_stream::~tls_stream()
        {
            if (ssl_)
            {
                if (handshake_complete_ && !closed_)
                {
                    // Try to send close_notify, but don't block
                    SSL_shutdown(ssl_);
                }
                SSL_free(ssl_);
            }
        }

        coro::poll_op tls_stream::get_poll_op_for_ssl_error(int ssl_error)
        {
            switch (ssl_error)
            {
            case SSL_ERROR_WANT_READ:
                return coro::poll_op::read;
            case SSL_ERROR_WANT_WRITE:
                return coro::poll_op::write;
            default:
                return coro::poll_op::read; // Default to read
            }
        }

        auto tls_stream::handshake() -> coro::task<bool>
        {
            if (!ssl_)
            {
                std::cerr << "TLS handshake failed: SSL not initialized" << std::endl;
                co_return false;
            }

            while (true)
            {
                int result = SSL_accept(ssl_);
                if (result == 1)
                {
                    // Handshake completed successfully
                    handshake_complete_ = true;
                    std::cout << "TLS handshake completed successfully" << std::endl;
                    co_return true;
                }

                int ssl_error = SSL_get_error(ssl_, result);
                if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
                {
                    // Need to wait for I/O
                    coro::poll_op op = get_poll_op_for_ssl_error(ssl_error);
                    auto poll_result = co_await client_.poll(op);
                    if (poll_result != coro::poll_status::event)
                    {
                        std::cerr << "TLS handshake poll failed" << std::endl;
                        co_return false;
                    }
                    continue;
                }

                // Fatal error
                std::cerr << "TLS handshake failed with error: " << ssl_error << std::endl;
                ERR_print_errors_fp(stderr);
                co_return false;
            }
        }

        auto tls_stream::poll(coro::poll_op op, std::chrono::milliseconds timeout) -> coro::task<coro::poll_status>
        {
            // For TLS, we need to check if there's pending data in the SSL buffer first
            if (op == coro::poll_op::read && SSL_pending(ssl_) > 0)
            {
                co_return coro::poll_status::event;
            }

            // Otherwise, poll the underlying socket
            co_return co_await client_.poll(op, timeout);
        }

        auto tls_stream::recv(std::span<char> buffer) -> std::pair<coro::net::recv_status, std::span<char>>
        {
            if (!ssl_ || !handshake_complete_ || closed_)
            {
                return {coro::net::recv_status::closed, std::span<char>{}};
            }

            int bytes_read = SSL_read(ssl_, buffer.data(), static_cast<int>(buffer.size()));
            if (bytes_read > 0)
            {
                return {coro::net::recv_status::ok, std::span<char>{buffer.data(), static_cast<size_t>(bytes_read)}};
            }

            int ssl_error = SSL_get_error(ssl_, bytes_read);
            switch (ssl_error)
            {
            case SSL_ERROR_ZERO_RETURN:
                // Clean shutdown
                closed_ = true;
                return {coro::net::recv_status::closed, std::span<char>{}};

            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE:
                // Would block - return as if no data available
                return {static_cast<coro::net::recv_status>(EAGAIN), std::span<char>{}};

            default:
                // Error occurred
                closed_ = true;
                return {coro::net::recv_status::closed, std::span<char>{}};
            }
        }

        auto tls_stream::send(std::span<const char> buffer) -> std::pair<coro::net::send_status, std::span<const char>>
        {
            if (!ssl_ || !handshake_complete_ || closed_)
            {
                return {coro::net::send_status::closed, buffer};
            }

            int bytes_written = SSL_write(ssl_, buffer.data(), static_cast<int>(buffer.size()));
            if (bytes_written > 0)
            {
                return {coro::net::send_status::ok,
                    std::span<const char>{buffer.data() + bytes_written, buffer.size() - bytes_written}};
            }

            int ssl_error = SSL_get_error(ssl_, bytes_written);
            switch (ssl_error)
            {
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE:
                // Would block
                return {coro::net::send_status::would_block, buffer};

            default:
                // Error occurred
                closed_ = true;
                return {coro::net::send_status::closed, buffer};
            }
        }

    } // namespace websocket_demo
}
