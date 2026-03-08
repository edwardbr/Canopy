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

    tls_stream::~tls_stream()
    {
        if (ssl_)
        {
            if (handshake_complete_ && !closed_)
            {
                SSL_shutdown(ssl_);
                flush_wbio();
            }
            SSL_free(ssl_); // also frees rbio_ and wbio_
        }
    }

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------

    // Read one chunk of raw bytes from the underlying stream and push them
    // into rbio so OpenSSL can process them.
    auto tls_stream::feed_rbio(std::chrono::milliseconds timeout) -> coro::task<bool>
    {
        std::array<char, 4096> buf;
        auto [status, span] = co_await underlying_->recv(std::span<char>{buf}, timeout);
        if (!status.is_ok() || span.empty())
            co_return false;
        BIO_write(rbio_, span.data(), static_cast<int>(span.size()));
        co_return true;
    }

    // Drain all pending SSL output from wbio to the underlying stream.
    bool tls_stream::flush_wbio()
    {
        std::array<char, 4096> buf;
        int len;
        while ((len = BIO_read(wbio_, buf.data(), static_cast<int>(buf.size()))) > 0)
        {
            std::span<const char> remaining(buf.data(), static_cast<size_t>(len));
            while (!remaining.empty())
            {
                auto [status, unsent] = underlying_->send(remaining);
                if (!status.is_ok() && !status.try_again())
                    return false;
                remaining = unsent;
            }
        }
        return true;
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
            if (!flush_wbio())
            {
                RPC_ERROR("TLS handshake: flush_wbio failed");
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
                // Need more bytes from the peer — fetch them with no timeout during handshake.
                if (!co_await feed_rbio(std::chrono::milliseconds{0}))
                {
                    RPC_WARNING("TLS handshake: peer closed or error while reading");
                    co_return false;
                }
            }
            else if (ssl_error == SSL_ERROR_WANT_WRITE)
            {
                // Data already flushed above; wait for the socket to drain.
                co_await underlying_->poll(coro::poll_op::write);
            }
            else
            {
                RPC_WARNING("TLS handshake rejected by peer (ssl_error={})", ssl_error);
                log_ssl_errors();
                co_return false;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Stream interface
    // -----------------------------------------------------------------------

    auto tls_stream::poll(coro::poll_op op, std::chrono::milliseconds timeout) -> coro::task<coro::poll_status>
    {
        // If decrypted data is already buffered by OpenSSL, report read-ready immediately.
        if (op == coro::poll_op::read && ssl_ && SSL_pending(ssl_) > 0)
            co_return coro::poll_status::read;

        co_return co_await underlying_->poll(op, timeout);
    }

    auto tls_stream::recv(std::span<char> buffer, std::chrono::milliseconds timeout)
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
            auto poll_status = co_await underlying_->poll(coro::poll_op::read, timeout);
            if (poll_status == coro::poll_status::timeout)
                co_return {coro::net::io_status{coro::net::io_status::kind::timeout}, {}};
            if (poll_status != coro::poll_status::read)
            {
                closed_ = true;
                co_return {coro::net::io_status{coro::net::io_status::kind::closed}, {}};
            }

            if (!co_await feed_rbio(std::chrono::milliseconds{0}))
            {
                closed_ = true;
                co_return {coro::net::io_status{coro::net::io_status::kind::closed}, {}};
            }
        }
    }

    auto tls_stream::send(std::span<const char> buffer) -> std::pair<coro::net::io_status, std::span<const char>>
    {
        if (!ssl_ || !handshake_complete_ || closed_)
            return {coro::net::io_status{coro::net::io_status::kind::closed}, buffer};

        int bytes_written = SSL_write(ssl_, buffer.data(), static_cast<int>(buffer.size()));
        if (bytes_written <= 0)
        {
            int ssl_error = SSL_get_error(ssl_, bytes_written);
            if (ssl_error == SSL_ERROR_WANT_WRITE || ssl_error == SSL_ERROR_WANT_READ)
                return {coro::net::io_status{coro::net::io_status::kind::would_block_or_try_again}, buffer};
            closed_ = true;
            return {coro::net::io_status{coro::net::io_status::kind::closed}, buffer};
        }

        // Push the encrypted bytes out to the underlying stream (sync path for wslay compat).
        flush_wbio();

        return {coro::net::io_status{coro::net::io_status::kind::ok},
            std::span<const char>{buffer.data() + bytes_written, buffer.size() - static_cast<size_t>(bytes_written)}};
    }

    auto tls_stream::write(std::span<const char> buffer) -> coro::task<coro::net::io_status>
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

        // Flush encrypted output asynchronously using the async write path.
        if (!co_await flush())
            co_return coro::net::io_status{coro::net::io_status::kind::closed};

        co_return coro::net::io_status{coro::net::io_status::kind::ok};
    }

    auto tls_stream::flush() -> coro::task<bool>
    {
        std::array<char, 4096> buf;
        int len;
        while ((len = BIO_read(wbio_, buf.data(), static_cast<int>(buf.size()))) > 0)
        {
            auto io_status = co_await underlying_->write(std::span<const char>{buf.data(), static_cast<size_t>(len)});
            if (!io_status.is_ok())
                co_return false;
        }
        co_return true;
    }

} // namespace streaming
