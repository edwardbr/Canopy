// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tls_stream.cpp - TLS stream implementation using OpenSSL memory BIOs
#include <streaming/tls_stream.h>
#include <array>

namespace streaming
{
    // Drain the OpenSSL error queue and emit each entry via RPC_ERROR.
    static void log_ssl_errors()
    {
        char buf[256];
        unsigned long err;
        while ((err = ERR_get_error()) != 0)
        {
            ERR_error_string_n(err, buf, sizeof(buf));
            RPC_ERROR("OpenSSL: {}", buf);
        }
    }

    // TLS context implementation
    tls_context::tls_context(const std::string& cert_file, const std::string& key_file)
    {
        ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ctx_)
        {
            RPC_ERROR("Failed to create SSL context");
            log_ssl_errors();
            return;
        }

        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

        if (SSL_CTX_use_certificate_file(ctx_, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0)
        {
            RPC_ERROR("Failed to load certificate: {}", cert_file);
            log_ssl_errors();
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
            return;
        }

        if (SSL_CTX_use_PrivateKey_file(ctx_, key_file.c_str(), SSL_FILETYPE_PEM) <= 0)
        {
            RPC_ERROR("Failed to load private key: {}", key_file);
            log_ssl_errors();
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
            return;
        }

        if (!SSL_CTX_check_private_key(ctx_))
        {
            RPC_ERROR("Private key does not match certificate");
            log_ssl_errors();
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
            return;
        }

        RPC_INFO("TLS context initialized with certificate: {}", cert_file);
    }

    tls_context::~tls_context()
    {
        if (ctx_)
        {
            SSL_CTX_free(ctx_);
        }
    }

    // TLS client context implementation
    tls_client_context::tls_client_context(bool verify_peer)
    {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_)
        {
            RPC_ERROR("Failed to create TLS client context");
            log_ssl_errors();
            return;
        }

        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

        if (!verify_peer)
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);

        RPC_INFO("TLS client context initialized (verify_peer={})", verify_peer);
    }

    tls_client_context::~tls_client_context()
    {
        if (ctx_)
            SSL_CTX_free(ctx_);
    }

    // TLS stream implementation
    tls_stream::tls_stream(std::shared_ptr<stream> underlying, std::shared_ptr<tls_context> tls_ctx)
        : underlying_(std::move(underlying))
        , tls_ctx_(std::move(tls_ctx))
    {
        if (tls_ctx_ && tls_ctx_->is_valid())
        {
            ssl_ = SSL_new(tls_ctx_->get());
            if (ssl_)
            {
                // Use memory BIOs so SSL has no direct socket access.
                // rbio: we write raw network bytes here for SSL to read.
                // wbio: SSL writes encrypted output here; we drain it to underlying_.
                rbio_ = BIO_new(BIO_s_mem());
                wbio_ = BIO_new(BIO_s_mem());
                SSL_set_bio(ssl_, rbio_, wbio_);
            }
        }
    }

    tls_stream::tls_stream(std::shared_ptr<stream> underlying, std::shared_ptr<tls_client_context> client_ctx)
        : underlying_(std::move(underlying))
        , tls_client_ctx_(std::move(client_ctx))
    {
        if (tls_client_ctx_ && tls_client_ctx_->is_valid())
        {
            ssl_ = SSL_new(tls_client_ctx_->get());
            if (ssl_)
            {
                rbio_ = BIO_new(BIO_s_mem());
                wbio_ = BIO_new(BIO_s_mem());
                SSL_set_bio(ssl_, rbio_, wbio_);
            }
        }
    }

    tls_stream::~tls_stream()
    {
        if (ssl_)
        {
            if (handshake_complete_ && !closed_)
            {
                // Queue the TLS close_notify alert into wbio; the bytes are
                // dropped here since we cannot await in a destructor. The
                // underlying socket close signals connection termination.
                SSL_shutdown(ssl_);
            }
            SSL_free(ssl_); // also frees rbio_ and wbio_
        }
        // Propagate closure to the underlying stream so that any proxy loops
        // (e.g. spsc_wrapping_stream's recv_proxy_loop / send_proxy_loop) detect
        // the closed_ flag and exit cleanly.
        if (underlying_)
            underlying_->set_closed();
    }

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------

    auto tls_stream::feed_rbio(std::chrono::milliseconds timeout) -> coro::task<coro::net::io_status>
    {
        std::array<char, 4096> buf;
        auto [status, span] = co_await underlying_->receive(std::span<char>{buf}, timeout);
        if (status.is_ok() && !span.empty())
            BIO_write(rbio_, span.data(), static_cast<int>(span.size()));
        co_return status;
    }

    auto tls_stream::drain_wbio() -> coro::task<bool>
    {
        std::array<char, 4096> buf;
        int len;
        while ((len = BIO_read(wbio_, buf.data(), static_cast<int>(buf.size()))) > 0)
        {
            auto status = co_await underlying_->send(std::span<const char>{buf.data(), static_cast<size_t>(len)});
            if (!status.is_ok())
                co_return false;
        }
        co_return true;
    }

    // -----------------------------------------------------------------------
    // Handshake
    // -----------------------------------------------------------------------

    auto tls_stream::handshake() -> coro::task<bool>
    {
        if (!ssl_)
        {
            RPC_ERROR("TLS handshake failed: SSL not initialized");
            co_return false;
        }

        while (true)
        {
            int result = SSL_accept(ssl_);

            // Always flush any handshake data SSL wants to send to the peer.
            if (!co_await drain_wbio())
            {
                RPC_ERROR("TLS handshake: drain_wbio failed");
                co_return false;
            }

            if (result == 1)
            {
                handshake_complete_ = true;
                RPC_INFO("TLS handshake completed successfully");
                co_return true;
            }

            int ssl_error = SSL_get_error(ssl_, result);
            if (ssl_error == SSL_ERROR_WANT_READ)
            {
                // Need more bytes from the peer. Retry until data arrives or the peer
                // closes — underlying streams (SPSC etc.) may return timeout when
                // temporarily empty, so loop on timeout rather than treating it as an error.
                while (true)
                {
                    auto feed_status = co_await feed_rbio(std::chrono::milliseconds{1});
                    if (feed_status.is_ok())
                        break; // data fed — let SSL have another go
                    if (feed_status.is_timeout())
                        continue; // SPSC queue temporarily empty — spin
                    RPC_WARNING("TLS handshake: peer closed or error while reading");
                    co_return false;
                }
            }
            else if (ssl_error == SSL_ERROR_WANT_WRITE)
            {
                // Data already flushed above via drain_wbio(); nothing more to do.
            }
            else
            {
                RPC_WARNING("TLS handshake rejected by peer (ssl_error={})", ssl_error);
                log_ssl_errors();
                co_return false;
            }
        }
    }

    auto tls_stream::client_handshake() -> coro::task<bool>
    {
        if (!ssl_)
        {
            RPC_ERROR("TLS client handshake failed: SSL not initialized");
            co_return false;
        }

        while (true)
        {
            int result = SSL_connect(ssl_);

            // Always flush any handshake data SSL wants to send to the peer.
            if (!co_await drain_wbio())
            {
                RPC_ERROR("TLS client handshake: drain_wbio failed");
                co_return false;
            }

            if (result == 1)
            {
                handshake_complete_ = true;
                RPC_INFO("TLS client handshake completed successfully");
                co_return true;
            }

            int ssl_error = SSL_get_error(ssl_, result);
            if (ssl_error == SSL_ERROR_WANT_READ)
            {
                while (true)
                {
                    auto feed_status = co_await feed_rbio(std::chrono::milliseconds{1});
                    if (feed_status.is_ok())
                        break;
                    if (feed_status.is_timeout())
                        continue;
                    RPC_WARNING("TLS client handshake: peer closed or error while reading");
                    co_return false;
                }
            }
            else if (ssl_error == SSL_ERROR_WANT_WRITE)
            {
                // Already flushed above via drain_wbio().
            }
            else
            {
                RPC_WARNING("TLS client handshake failed (ssl_error={})", ssl_error);
                log_ssl_errors();
                co_return false;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Stream interface
    // -----------------------------------------------------------------------

    auto tls_stream::receive(std::span<char> buffer, std::chrono::milliseconds timeout)
        -> coro::task<std::pair<coro::net::io_status, std::span<char>>>
    {
        if (!ssl_ || !handshake_complete_ || closed_)
            co_return {coro::net::io_status{coro::net::io_status::kind::closed}, {}};

        while (true)
        {
            // Try to decrypt whatever is already in rbio.
            int bytes_read = SSL_read(ssl_, buffer.data(), static_cast<int>(buffer.size()));
            if (bytes_read > 0)
                co_return {coro::net::io_status{coro::net::io_status::kind::ok},
                    std::span<char>{buffer.data(), static_cast<size_t>(bytes_read)}};

            int ssl_error = SSL_get_error(ssl_, bytes_read);
            if (ssl_error == SSL_ERROR_ZERO_RETURN)
            {
                closed_ = true;
                co_return {coro::net::io_status{coro::net::io_status::kind::closed}, {}};
            }
            if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE)
            {
                closed_ = true;
                co_return {coro::net::io_status{coro::net::io_status::kind::closed}, {}};
            }

            // Need more raw bytes — wait on the underlying stream with the caller's timeout.
            auto feed_status = co_await feed_rbio(timeout);
            if (!feed_status.is_ok())
            {
                if (feed_status.is_closed())
                    closed_ = true;
                co_return {feed_status, {}};
            }
        }
    }

    auto tls_stream::send(std::span<const char> buffer) -> coro::task<coro::net::io_status>
    {
        if (!ssl_ || !handshake_complete_ || closed_)
            co_return coro::net::io_status{coro::net::io_status::kind::closed};

        int bytes_written = SSL_write(ssl_, buffer.data(), static_cast<int>(buffer.size()));
        if (bytes_written <= 0)
        {
            int ssl_error = SSL_get_error(ssl_, bytes_written);
            if (ssl_error == SSL_ERROR_WANT_WRITE || ssl_error == SSL_ERROR_WANT_READ)
                co_return coro::net::io_status{coro::net::io_status::kind::would_block_or_try_again};
            closed_ = true;
            co_return coro::net::io_status{coro::net::io_status::kind::closed};
        }

        if (!co_await drain_wbio())
            co_return coro::net::io_status{coro::net::io_status::kind::closed};

        co_return coro::net::io_status{coro::net::io_status::kind::ok};
    }

} // namespace streaming
