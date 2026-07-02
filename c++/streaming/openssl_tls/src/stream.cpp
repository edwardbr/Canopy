// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// OpenSSL TLS stream implementation using memory BIOs.
#include <streaming/openssl_tls/stream.h>

#include <array>
#include <chrono>
#include <limits>
#include <mutex>
#include <utility>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

namespace streaming::openssl_tls
{
    // A zero receive timeout is a poll for the current stream implementations,
    // not a blocking wait. Use a bounded wait during handshakes so the peer's
    // coroutine/proxy loops get scheduled without returning to a 1ms busy poll.
    static constexpr auto handshake_receive_timeout = std::chrono::milliseconds{1000};

    static auto is_peer_certificate_alert(unsigned long error) -> bool
    {
        const auto reason = ERR_GET_REASON(error);
        return reason == SSL_R_SSLV3_ALERT_CERTIFICATE_UNKNOWN || reason == SSL_R_TLSV1_ALERT_UNKNOWN_CA
               || reason == SSL_R_SSLV3_ALERT_BAD_CERTIFICATE || reason == SSL_R_TLSV1_ALERT_ACCESS_DENIED;
    }

    // Drain the OpenSSL error queue and emit each entry via telemetry.
    static auto log_ssl_errors(bool peer_certificate_alerts_are_warnings = false) -> bool
    {
        std::array<char, 256> buf{};
        bool saw_peer_certificate_alert = false;
        unsigned long err = 0;
        while ((err = ERR_get_error()) != 0)
        {
            ERR_error_string_n(err, buf.data(), buf.size());
            if (peer_certificate_alerts_are_warnings && is_peer_certificate_alert(err))
            {
                saw_peer_certificate_alert = true;
                RPC_WARNING("OpenSSL peer certificate alert: {}", buf.data());
            }
            else
            {
                RPC_ERROR("OpenSSL: {}", buf.data());
            }
        }
        return saw_peer_certificate_alert;
    }

    static auto create_server_context() -> SSL_CTX*
    {
        auto* ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx)
        {
            RPC_ERROR("Failed to create SSL context");
            log_ssl_errors();
            return nullptr;
        }

        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        return ctx;
    }

    static auto verify_mode(peer_verification mode) -> int
    {
        switch (mode)
        {
        case peer_verification::none:
            return SSL_VERIFY_NONE;
        case peer_verification::optional:
            return SSL_VERIFY_PEER;
        case peer_verification::required:
            return SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
        return SSL_VERIFY_NONE;
    }

    static auto fits_openssl_int(size_t size) -> bool
    {
        return size <= static_cast<size_t>(std::numeric_limits<int>::max());
    }

    static auto release_context_on_error(SSL_CTX*& ctx) -> void
    {
        if (ctx)
        {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
    }

    struct stream_cleanup_state
    {
        stream_cleanup_state(
            SSL* input_ssl,
            std::shared_ptr<::streaming::stream> input_underlying,
            bool input_handshake_complete,
            bool input_already_closed)
            : ssl(input_ssl)
            , underlying(std::move(input_underlying))
            , handshake_complete(input_handshake_complete)
            , already_closed(input_already_closed)
        {
        }

        ~stream_cleanup_state() { free_ssl(); }

        stream_cleanup_state(const stream_cleanup_state&) = delete;
        auto operator=(const stream_cleanup_state&) -> stream_cleanup_state& = delete;

        void free_ssl() noexcept
        {
            if (ssl)
            {
                SSL_free(ssl);
                ssl = nullptr;
            }
        }

        SSL* ssl{nullptr};
        std::shared_ptr<::streaming::stream> underlying;
        bool handshake_complete{false};
        bool already_closed{false};
    };

    auto cleanup_stream_state(std::shared_ptr<stream_cleanup_state> state) -> CORO_TASK(void)
    {
        if (!state)
            CO_RETURN;

        try
        {
            if (!state->already_closed)
            {
                if (state->ssl && state->handshake_complete)
                {
                    SSL_shutdown(state->ssl);
                }

                // The parent stream is intentionally held in state until this
                // lambda has finished TLS-owned cleanup. Releasing that shared
                // pointer then allows the parent destructor to run after this
                // layer has finished with its internal state.
            }
        }
        catch (const std::exception& ex)
        {
            RPC_WARNING("TLS stream async cleanup failed: {}", ex.what());
        }
        catch (...)
        {
            RPC_WARNING("TLS stream async cleanup failed with an unknown exception");
        }

        state->free_ssl();
        CO_RETURN;
    }

    auto spawn_cleanup(
        const rpc::executor_ptr& executor,
        const std::shared_ptr<stream_cleanup_state>& state) noexcept -> bool
    {
        if (!executor || executor->is_shutdown())
            return false;

        try
        {
#ifdef CANOPY_BUILD_COROUTINE
            return executor->spawn_detached(cleanup_stream_state(state));
#else
            return executor->post([state] { cleanup_stream_state(state); });
#endif
        }
        catch (...)
        {
            return false;
        }
    }

    static auto load_file_credentials(
        SSL_CTX*& ctx,
        const std::string& cert_file,
        const std::string& key_file) -> bool
    {
        if (SSL_CTX_use_certificate_file(ctx, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0)
        {
            RPC_ERROR("Failed to load certificate: {}", cert_file);
            log_ssl_errors();
            release_context_on_error(ctx);
            return false;
        }

        if (SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(), SSL_FILETYPE_PEM) <= 0)
        {
            RPC_ERROR("Failed to load private key: {}", key_file);
            log_ssl_errors();
            release_context_on_error(ctx);
            return false;
        }

        return true;
    }

    static auto load_pem_credentials(
        SSL_CTX*& ctx,
        const pem_credentials& credentials) -> bool
    {
        if (credentials.certificate.size() > static_cast<size_t>(std::numeric_limits<int>::max())
            || credentials.private_key.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            RPC_ERROR("TLS PEM credential is too large");
            release_context_on_error(ctx);
            return false;
        }

        BIO* cert_bio = BIO_new_mem_buf(credentials.certificate.data(), static_cast<int>(credentials.certificate.size()));
        if (!cert_bio)
        {
            RPC_ERROR("Failed to create certificate memory BIO");
            log_ssl_errors();
            release_context_on_error(ctx);
            return false;
        }

        X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
        BIO_free(cert_bio);
        if (!cert)
        {
            RPC_ERROR("Failed to parse PEM certificate");
            log_ssl_errors();
            release_context_on_error(ctx);
            return false;
        }

        const int cert_result = SSL_CTX_use_certificate(ctx, cert);
        X509_free(cert);
        if (cert_result <= 0)
        {
            RPC_ERROR("Failed to apply PEM certificate");
            log_ssl_errors();
            release_context_on_error(ctx);
            return false;
        }

        BIO* key_bio = BIO_new_mem_buf(credentials.private_key.data(), static_cast<int>(credentials.private_key.size()));
        if (!key_bio)
        {
            RPC_ERROR("Failed to create private key memory BIO");
            log_ssl_errors();
            release_context_on_error(ctx);
            return false;
        }

        EVP_PKEY* private_key = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
        BIO_free(key_bio);
        if (!private_key)
        {
            RPC_ERROR("Failed to parse PEM private key");
            log_ssl_errors();
            release_context_on_error(ctx);
            return false;
        }

        const int key_result = SSL_CTX_use_PrivateKey(ctx, private_key);
        EVP_PKEY_free(private_key);
        if (key_result <= 0)
        {
            RPC_ERROR("Failed to apply PEM private key");
            log_ssl_errors();
            release_context_on_error(ctx);
            return false;
        }

        return true;
    }

    static auto load_trust_anchor_pem(
        SSL_CTX*& ctx,
        const std::string& trust_anchor) -> bool
    {
        if (trust_anchor.empty())
            return true;
        if (trust_anchor.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            RPC_ERROR("TLS trust anchor PEM is too large");
            release_context_on_error(ctx);
            return false;
        }

        BIO* ca_bio = BIO_new_mem_buf(trust_anchor.data(), static_cast<int>(trust_anchor.size()));
        if (!ca_bio)
        {
            RPC_ERROR("Failed to create trust anchor memory BIO");
            log_ssl_errors();
            release_context_on_error(ctx);
            return false;
        }

        auto* store = SSL_CTX_get_cert_store(ctx);
        bool loaded_any = false;
        while (true)
        {
            X509* cert = PEM_read_bio_X509(ca_bio, nullptr, nullptr, nullptr);
            if (!cert)
                break;

            if (X509_STORE_add_cert(store, cert) != 1)
            {
                X509_free(cert);
                BIO_free(ca_bio);
                RPC_ERROR("Failed to add trust anchor certificate");
                log_ssl_errors();
                release_context_on_error(ctx);
                return false;
            }
            X509_free(cert);
            loaded_any = true;
        }

        BIO_free(ca_bio);
        if (!loaded_any)
        {
            RPC_ERROR("Failed to parse trust anchor PEM");
            log_ssl_errors();
            release_context_on_error(ctx);
            return false;
        }

        ERR_clear_error();
        return true;
    }

    static auto configure_server_peer_verification(
        SSL_CTX*& ctx,
        const pem_credentials& credentials,
        server_context_options options) -> bool
    {
        if (options.verify_peer != peer_verification::none && credentials.trust_anchor.empty())
        {
            RPC_ERROR("TLS server peer verification requires a trust anchor");
            release_context_on_error(ctx);
            return false;
        }

        if (!load_trust_anchor_pem(ctx, credentials.trust_anchor))
            return false;

        SSL_CTX_set_verify(ctx, verify_mode(options.verify_peer), nullptr);
        return true;
    }

    static auto verify_private_key(SSL_CTX*& ctx) -> bool
    {
        if (!SSL_CTX_check_private_key(ctx))
        {
            RPC_ERROR("Private key does not match certificate");
            log_ssl_errors();
            release_context_on_error(ctx);
            return false;
        }

        return true;
    }

    // TLS context implementation
    context::context(
        const std::string& cert_file,
        const std::string& key_file,
        server_context_options options)
        : ctx_(create_server_context())
    {
        if (!ctx_)
            return;

        if (!load_file_credentials(ctx_, cert_file, key_file) || !verify_private_key(ctx_))
            return;

        pem_credentials credentials;
        if (!configure_server_peer_verification(ctx_, credentials, options))
            return;

        RPC_DEBUG("TLS context initialized with certificate: {}", cert_file);
    }

    context::context(
        const pem_credentials& credentials,
        server_context_options options)
        : ctx_(create_server_context())
    {
        if (!ctx_)
            return;

        if (!load_pem_credentials(ctx_, credentials) || !verify_private_key(ctx_))
            return;
        if (!configure_server_peer_verification(ctx_, credentials, options))
            return;

        RPC_DEBUG("TLS context initialized from PEM credentials");
    }

    context::~context()
    {
        if (ctx_)
        {
            SSL_CTX_free(ctx_);
        }
    }

    // TLS client context implementation
    client_context::client_context(bool verify_peer)
        : client_context(
              [verify_peer]
              {
                  client_context_options options;
                  options.verify_peer = verify_peer;
                  return options;
              }())
    {
    }

    client_context::client_context(client_context_options options)
        : ctx_(SSL_CTX_new(TLS_client_method()))
        , verify_peer_(options.verify_peer)
        , server_name_(std::move(options.server_name))
    {
        if (!ctx_)
        {
            RPC_ERROR("Failed to create TLS client context");
            log_ssl_errors();
            return;
        }

        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        if (options.verify_peer && SSL_CTX_set_default_verify_paths(ctx_) != 1)
        {
            RPC_WARNING("Failed to load default TLS trust store paths");
            log_ssl_errors();
        }

        SSL_CTX_set_verify(ctx_, options.verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);

        RPC_DEBUG("TLS client context initialized (verify_peer={}, server_name={})", options.verify_peer, server_name_);
    }

    client_context::client_context(
        std::string trust_anchor,
        bool verify_peer)
        : client_context(
              std::move(trust_anchor),
              [verify_peer]
              {
                  client_context_options options;
                  options.verify_peer = verify_peer;
                  return options;
              }())
    {
    }

    client_context::client_context(
        std::string trust_anchor,
        client_context_options options)
        : client_context(options)
    {
        if (!ctx_ || trust_anchor.empty())
            return;

        if (!load_trust_anchor_pem(ctx_, trust_anchor))
            return;
    }

    client_context::~client_context()
    {
        if (ctx_)
            SSL_CTX_free(ctx_);
    }

    // TLS stream implementation
    stream::stream(
        std::shared_ptr<::streaming::stream> underlying,
        std::shared_ptr<context> tls_ctx,
        rpc::executor_ptr executor)
        : underlying_(std::move(underlying))
        , tls_ctx_(std::move(tls_ctx))
        , executor_(std::move(executor))
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

    stream::stream(
        std::shared_ptr<::streaming::stream> underlying,
        std::shared_ptr<client_context> client_ctx,
        rpc::executor_ptr executor)
        : underlying_(std::move(underlying))
        , tls_client_ctx_(std::move(client_ctx))
        , executor_(std::move(executor))
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

    stream::~stream()
    {
        close_detached();
    }

    void stream::close_detached() noexcept
    {
        auto* ssl = std::exchange(ssl_, nullptr);
        rbio_ = nullptr;
        wbio_ = nullptr;

        const auto already_closed = closed_.exchange(true, std::memory_order_acq_rel);

        if (!ssl)
            return;

        if (already_closed)
        {
            SSL_free(ssl);
            return;
        }

        try
        {
            auto state = std::make_shared<stream_cleanup_state>(ssl, underlying_, handshake_complete_, already_closed);
            if (spawn_cleanup(executor_, state))
                return;

            // Destructors cannot block or drive coroutine work. If no executor
            // was supplied, free the OpenSSL state locally and release the
            // underlying stream so its own RAII cleanup can run.
            RPC_WARNING("TLS stream cleanup executor unavailable; freeing TLS state without async cleanup");
            state->free_ssl();
        }
        catch (...)
        {
            RPC_WARNING("TLS stream cleanup state allocation failed; freeing TLS state without async close");
            SSL_free(ssl);
        }
    }

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------

    auto stream::feed_rbio(std::chrono::milliseconds timeout) -> CORO_TASK(rpc::io_status)
    {
        std::array<char, 4096> buf{};
        auto [status, span] = CO_AWAIT underlying_->receive(rpc::mutable_byte_span{buf.data(), buf.size()}, timeout);
        if (status.is_ok() && span.empty())
            CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::timeout};
        if (status.is_ok() && !span.empty())
        {
            if (!fits_openssl_int(span.size()))
                CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::native, FLD(native_code) SSL_ERROR_SSL};
#ifdef CANOPY_BUILD_COROUTINE
            auto tls_lock = CO_AWAIT tls_mtx_.scoped_lock();
#else
            std::unique_lock<std::mutex> tls_lock(tls_mtx_);
#endif
            const auto written = BIO_write(rbio_, span.data(), static_cast<int>(span.size()));
            if (written != static_cast<int>(span.size()))
                CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::native, FLD(native_code) SSL_ERROR_SSL};
        }
        CO_RETURN status;
    }

    auto stream::drain_wbio() -> CORO_TASK(bool)
    {
        std::array<char, 4096> buf{};
        while (true)
        {
            int len = 0;
            {
#ifdef CANOPY_BUILD_COROUTINE
                auto tls_lock = CO_AWAIT tls_mtx_.scoped_lock();
#else
                std::unique_lock<std::mutex> tls_lock(tls_mtx_);
#endif
                len = BIO_read(wbio_, buf.data(), static_cast<int>(buf.size()));
            }
            if (len <= 0)
                break;

            auto status = CO_AWAIT underlying_->send(rpc::byte_span{buf.data(), static_cast<size_t>(len)});
            if (!status.is_ok())
                CO_RETURN false;
        }
        CO_RETURN true;
    }

    // -----------------------------------------------------------------------
    // Handshake
    // -----------------------------------------------------------------------

    auto stream::handshake() -> CORO_TASK(bool)
    {
        {
#ifdef CANOPY_BUILD_COROUTINE
            auto tls_lock = CO_AWAIT tls_mtx_.scoped_lock();
#else
            std::unique_lock<std::mutex> tls_lock(tls_mtx_);
#endif
            if (!ssl_)
            {
                RPC_ERROR("TLS handshake failed: SSL not initialized");
                CO_RETURN false;
            }
        }

        while (true)
        {
            int result = 0;
            int ssl_error = 0;
            {
#ifdef CANOPY_BUILD_COROUTINE
                auto tls_lock = CO_AWAIT tls_mtx_.scoped_lock();
#else
                std::unique_lock<std::mutex> tls_lock(tls_mtx_);
#endif
                result = SSL_accept(ssl_);
                if (result != 1)
                    ssl_error = SSL_get_error(ssl_, result);
            }

            // Always flush any handshake data SSL wants to send to the peer.
            if (!CO_AWAIT drain_wbio())
            {
                RPC_ERROR("TLS handshake: drain_wbio failed");
                CO_RETURN false;
            }

            if (result == 1)
            {
#ifdef CANOPY_BUILD_COROUTINE
                auto tls_lock = CO_AWAIT tls_mtx_.scoped_lock();
#else
                std::unique_lock<std::mutex> tls_lock(tls_mtx_);
#endif
                handshake_complete_ = true;
                RPC_DEBUG("TLS handshake completed successfully");
                CO_RETURN true;
            }

            if (ssl_error == SSL_ERROR_WANT_READ)
            {
                auto feed_status = CO_AWAIT feed_rbio(handshake_receive_timeout);
                if (feed_status.is_ok())
                    continue; // data fed; let SSL have another go
                if (feed_status.is_timeout())
                    RPC_WARNING("TLS handshake: underlying stream timed out while waiting for peer data");
                RPC_WARNING("TLS handshake: peer closed or error while reading");
                CO_RETURN false;
            }
            else if (ssl_error == SSL_ERROR_WANT_WRITE)
            {
                // Data already flushed above via drain_wbio(); nothing more to do.
            }
            else
            {
                const bool peer_certificate_alert = log_ssl_errors(true);
                if (peer_certificate_alert)
                {
                    RPC_WARNING("TLS handshake rejected by peer certificate validation (ssl_error={})", ssl_error);
                }
                else
                {
                    RPC_WARNING("TLS handshake rejected by peer (ssl_error={})", ssl_error);
                }
                CO_RETURN false;
            }
        }
    }

    auto stream::client_handshake() -> CORO_TASK(bool)
    {
        {
#ifdef CANOPY_BUILD_COROUTINE
            auto tls_lock = CO_AWAIT tls_mtx_.scoped_lock();
#else
            std::unique_lock<std::mutex> tls_lock(tls_mtx_);
#endif
            if (!ssl_)
            {
                RPC_ERROR("TLS client handshake failed: SSL not initialized");
                CO_RETURN false;
            }

            if (!client_handshake_configured_)
            {
                if (tls_client_ctx_ && !tls_client_ctx_->server_name().empty())
                {
                    if (SSL_set_tlsext_host_name(ssl_, tls_client_ctx_->server_name().c_str()) != 1)
                    {
                        RPC_ERROR("Failed to configure TLS SNI for {}", tls_client_ctx_->server_name());
                        log_ssl_errors();
                        CO_RETURN false;
                    }
                    if (tls_client_ctx_->verify_peer() && SSL_set1_host(ssl_, tls_client_ctx_->server_name().c_str()) != 1)
                    {
                        RPC_ERROR(
                            "Failed to configure TLS certificate hostname verification for {}",
                            tls_client_ctx_->server_name());
                        log_ssl_errors();
                        CO_RETURN false;
                    }
                }
                client_handshake_configured_ = true;
            }
        }

        while (true)
        {
            int result = 0;
            int ssl_error = 0;
            {
#ifdef CANOPY_BUILD_COROUTINE
                auto tls_lock = CO_AWAIT tls_mtx_.scoped_lock();
#else
                std::unique_lock<std::mutex> tls_lock(tls_mtx_);
#endif
                result = SSL_connect(ssl_);
                if (result != 1)
                    ssl_error = SSL_get_error(ssl_, result);
            }

            // Always flush any handshake data SSL wants to send to the peer.
            if (!CO_AWAIT drain_wbio())
            {
                RPC_ERROR("TLS client handshake: drain_wbio failed");
                CO_RETURN false;
            }

            if (result == 1)
            {
#ifdef CANOPY_BUILD_COROUTINE
                auto tls_lock = CO_AWAIT tls_mtx_.scoped_lock();
#else
                std::unique_lock<std::mutex> tls_lock(tls_mtx_);
#endif
                handshake_complete_ = true;
                RPC_DEBUG("TLS client handshake completed successfully");
                CO_RETURN true;
            }

            if (ssl_error == SSL_ERROR_WANT_READ)
            {
                auto feed_status = CO_AWAIT feed_rbio(handshake_receive_timeout);
                if (feed_status.is_ok())
                    continue;
                if (feed_status.is_timeout())
                    RPC_WARNING("TLS client handshake: underlying stream timed out while waiting for peer data");
                RPC_WARNING("TLS client handshake: peer closed or error while reading");
                CO_RETURN false;
            }
            else if (ssl_error == SSL_ERROR_WANT_WRITE)
            {
                // Already flushed above via drain_wbio().
            }
            else
            {
                RPC_WARNING("TLS client handshake failed (ssl_error={})", ssl_error);
                log_ssl_errors();
                CO_RETURN false;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Stream interface
    // -----------------------------------------------------------------------

    auto stream::receive(
        rpc::mutable_byte_span buffer,
        std::chrono::milliseconds timeout) -> CORO_TASK(::streaming::receive_result)
    {
        if (!fits_openssl_int(buffer.size()))
            CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::native, FLD(native_code) SSL_ERROR_SSL}, {}};

        while (true)
        {
            {
#ifdef CANOPY_BUILD_COROUTINE
                auto tls_lock = CO_AWAIT tls_mtx_.scoped_lock();
#else
                std::unique_lock<std::mutex> tls_lock(tls_mtx_);
#endif
                if (!ssl_ || !handshake_complete_ || closed_.load(std::memory_order_acquire))
                    CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};

                // Try to decrypt whatever is already in rbio.
                int bytes_read = SSL_read(ssl_, buffer.data(), static_cast<int>(buffer.size()));
                if (bytes_read > 0)
                    CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::ok},
                        rpc::mutable_byte_span{buffer.data(), static_cast<size_t>(bytes_read)}};

                int ssl_error = SSL_get_error(ssl_, bytes_read);
                if (ssl_error == SSL_ERROR_ZERO_RETURN)
                {
                    closed_.store(true, std::memory_order_release);
                    CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};
                }
                if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE)
                {
                    closed_.store(true, std::memory_order_release);
                    CO_RETURN{rpc::io_status{FLD(type) rpc::io_status::kind::closed}, {}};
                }
            }

            // Need more raw bytes — wait on the underlying stream with the caller's timeout.
            auto feed_status = CO_AWAIT feed_rbio(timeout);
            if (!feed_status.is_ok())
            {
                if (feed_status.is_closed())
                    closed_.store(true, std::memory_order_release);
                CO_RETURN{feed_status, {}};
            }
        }
    }

    auto stream::send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status)
    {
        if (!fits_openssl_int(buffer.size()))
            CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::native, FLD(native_code) SSL_ERROR_SSL};

        {
#ifdef CANOPY_BUILD_COROUTINE
            auto tls_lock = CO_AWAIT tls_mtx_.scoped_lock();
#else
            std::unique_lock<std::mutex> tls_lock(tls_mtx_);
#endif
            if (!ssl_ || !handshake_complete_ || closed_.load(std::memory_order_acquire))
                CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::closed};

            int bytes_written = SSL_write(ssl_, buffer.data(), static_cast<int>(buffer.size()));
            if (bytes_written <= 0)
            {
                int ssl_error = SSL_get_error(ssl_, bytes_written);
                if (ssl_error == SSL_ERROR_WANT_WRITE || ssl_error == SSL_ERROR_WANT_READ)
                    CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::would_block_or_try_again};
                closed_.store(true, std::memory_order_release);
                CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::closed};
            }
        }

        if (!CO_AWAIT drain_wbio())
            CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::closed};

        CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::ok};
    }

    auto stream::set_closed() -> CORO_TASK(void)
    {
        if (closed_.exchange(true, std::memory_order_acq_rel))
            CO_RETURN;

        bool drain_close_notify = false;
        {
#ifdef CANOPY_BUILD_COROUTINE
            auto tls_lock = CO_AWAIT tls_mtx_.scoped_lock();
#else
            std::unique_lock<std::mutex> tls_lock(tls_mtx_);
#endif
            if (ssl_ && handshake_complete_)
            {
                SSL_shutdown(ssl_);
                drain_close_notify = true;
            }
        }

        if (drain_close_notify)
            (void)CO_AWAIT drain_wbio();

        if (underlying_)
            CO_AWAIT underlying_->set_closed();

        CO_RETURN;
    }

} // namespace streaming::openssl_tls
