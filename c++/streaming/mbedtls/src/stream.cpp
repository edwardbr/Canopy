// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include <streaming/mbedtls/stream.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#include <rpc/internal/coro_runtime/mutex.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#ifndef MBEDTLS_ERR_SSL_CONN_EOF
#  define MBEDTLS_ERR_SSL_CONN_EOF MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY
#endif

namespace streaming::mbedtls
{
    namespace
    {
        constexpr size_t transfer_buffer_size = 4096;

        auto ok_status() noexcept -> coro::net::io_status
        {
            return coro::net::io_status{.type = coro::net::io_status::kind::ok};
        }

        auto closed_status() noexcept -> coro::net::io_status
        {
            return coro::net::io_status{.type = coro::net::io_status::kind::closed};
        }

        auto timeout_status() noexcept -> coro::net::io_status
        {
            return coro::net::io_status{.type = coro::net::io_status::kind::timeout};
        }

        auto native_status(int error_code) noexcept -> coro::net::io_status
        {
            return coro::net::io_status{.type = coro::net::io_status::kind::native, .native_code = error_code};
        }

        auto ssl_internal_error() noexcept -> int
        {
#if defined(MBEDTLS_ERR_SSL_INTERNAL_ERROR)
            return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
#else
            return -1;
#endif
        }

        auto is_want_io(int result) noexcept -> bool
        {
            return result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE;
        }

        auto is_peer_closed(int result) noexcept -> bool
        {
            return result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || result == MBEDTLS_ERR_SSL_CONN_EOF;
        }

        auto log_mbedtls_error(
            const char* operation,
            int error_code) -> void
        {
            char buffer[256]{};
            mbedtls_strerror(error_code, buffer, sizeof(buffer));
            RPC_ERROR("{} failed: {} ({})", operation, buffer, error_code);
        }

        auto log_mbedtls_handshake_error(
            const char* role,
            int error_code) -> void
        {
            char buffer[256]{};
            mbedtls_strerror(error_code, buffer, sizeof(buffer));

#if defined(MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE)
            if (error_code == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE)
            {
                RPC_WARNING(
                    "{} TLS handshake stopped because the peer sent a fatal TLS alert: {} ({}). "
                    "For browser clients this is commonly certificate rejection, a cancelled/speculative connection, "
                    "or a protocol/cipher mismatch.",
                    role,
                    buffer,
                    error_code);
                return;
            }
#endif

#if defined(MBEDTLS_ERR_SSL_INVALID_MAC)
            if (error_code == MBEDTLS_ERR_SSL_INVALID_MAC)
            {
                RPC_WARNING(
                    "{} TLS handshake stopped after an invalid TLS record/MAC: {} ({}). "
                    "This can happen when a client abandons certificate validation, sends non-TLS data to the TLS "
                    "port, "
                    "or the encrypted handshake data is corrupt.",
                    role,
                    buffer,
                    error_code);
                return;
            }
#endif

            if (is_peer_closed(error_code))
            {
                RPC_WARNING(
                    "{} TLS handshake ended because the peer closed the connection: {} ({})", role, buffer, error_code);
                return;
            }

            RPC_ERROR("{} TLS handshake failed: {} ({})", role, buffer, error_code);
        }

        auto seed_entropy(
            void* entropy_context,
            unsigned char* output,
            size_t output_size) -> int
        {
            return mbedtls_entropy_func(entropy_context, output, output_size);
        }

        auto parse_key(
            mbedtls_pk_context* key,
            const std::string& pem,
            mbedtls_ctr_drbg_context* rng) -> int
        {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
            return mbedtls_pk_parse_key(
                key, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, rng);
#else
            (void)rng;
            return mbedtls_pk_parse_key(
                key, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1, nullptr, 0);
#endif
        }

        struct rng_context
        {
            rng_context()
            {
                mbedtls_entropy_init(&entropy);
                mbedtls_ctr_drbg_init(&ctr_drbg);
            }

            ~rng_context()
            {
                mbedtls_ctr_drbg_free(&ctr_drbg);
                mbedtls_entropy_free(&entropy);
            }

            rng_context(const rng_context&) = delete;
            auto operator=(const rng_context&) -> rng_context& = delete;

            auto seed(const char* personalization) -> bool
            {
                auto result = mbedtls_ctr_drbg_seed(
                    &ctr_drbg,
                    seed_entropy,
                    &entropy,
                    reinterpret_cast<const unsigned char*>(personalization),
                    std::strlen(personalization));
                if (result != 0)
                {
                    log_mbedtls_error("mbedtls_ctr_drbg_seed", result);
                    return false;
                }
                return true;
            }

            mbedtls_entropy_context entropy{};
            mbedtls_ctr_drbg_context ctr_drbg{};
        };

        auto configure_tls_versions(mbedtls_ssl_config& config) -> void
        {
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#  if MBEDTLS_VERSION_NUMBER >= 0x03000000
            mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
#  else
            mbedtls_ssl_conf_min_version(&config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
#  endif
#else
            (void)config;
#endif
        }

        auto initialise_psa_crypto() -> bool
        {
#if defined(MBEDTLS_PSA_CRYPTO_C)
            auto status = psa_crypto_init();
            if (status != PSA_SUCCESS)
            {
                RPC_ERROR("psa_crypto_init failed: {}", static_cast<int>(status));
                return false;
            }
#endif
            return true;
        }

        auto auth_mode(peer_verification mode) -> int
        {
            switch (mode)
            {
            case peer_verification::none:
                return MBEDTLS_SSL_VERIFY_NONE;
            case peer_verification::optional:
                return MBEDTLS_SSL_VERIFY_OPTIONAL;
            case peer_verification::required:
                return MBEDTLS_SSL_VERIFY_REQUIRED;
            }
            return MBEDTLS_SSL_VERIFY_NONE;
        }

        auto bytes_to_string(const std::vector<uint8_t>& data) -> std::string
        {
            return std::string(reinterpret_cast<const char*>(data.data()), data.size());
        }

        auto read_text_file(
            const std::string& path,
            std::string& out) -> bool
        {
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
            {
                RPC_ERROR("Failed to open Mbed TLS credential file: {}", path);
                return false;
            }

            std::ostringstream buffer;
            buffer << input.rdbuf();
            out = buffer.str();
            return true;
        }

        auto read_file_credentials(
            const std::string& cert_file,
            const std::string& key_file,
            pem_credentials& credentials) -> bool
        {
            return read_text_file(cert_file, credentials.certificate) && read_text_file(key_file, credentials.private_key);
        }
    }

    struct context::impl
    {
        impl()
        {
            mbedtls_x509_crt_init(&certificate);
            mbedtls_x509_crt_init(&trust_anchor);
            mbedtls_pk_init(&private_key);
        }

        ~impl()
        {
            mbedtls_pk_free(&private_key);
            mbedtls_x509_crt_free(&trust_anchor);
            mbedtls_x509_crt_free(&certificate);
        }

        auto initialise(
            const pem_credentials& credentials,
            server_context_options context_options) -> void
        {
            if (credentials.certificate.empty() || credentials.private_key.empty())
            {
                RPC_ERROR("Mbed TLS server credentials require certificate and private key PEM data");
                return;
            }

            if (context_options.verify_peer != peer_verification::none && credentials.trust_anchor.empty())
            {
                RPC_ERROR("Mbed TLS server peer verification requires a trust anchor");
                return;
            }

            if (!initialise_psa_crypto())
                return;

            rng_context parse_rng;
            if (!parse_rng.seed("canopy-mbedtls-server-context"))
                return;

            auto result = mbedtls_x509_crt_parse(
                &certificate,
                reinterpret_cast<const unsigned char*>(credentials.certificate.c_str()),
                credentials.certificate.size() + 1);
            if (result != 0)
            {
                log_mbedtls_error("mbedtls_x509_crt_parse(certificate)", result);
                return;
            }

            result = parse_key(&private_key, credentials.private_key, &parse_rng.ctr_drbg);
            if (result != 0)
            {
                log_mbedtls_error("mbedtls_pk_parse_key", result);
                return;
            }

            if (!credentials.trust_anchor.empty())
            {
                result = mbedtls_x509_crt_parse(
                    &trust_anchor,
                    reinterpret_cast<const unsigned char*>(credentials.trust_anchor.c_str()),
                    credentials.trust_anchor.size() + 1);
                if (result != 0)
                {
                    log_mbedtls_error("mbedtls_x509_crt_parse(trust_anchor)", result);
                    return;
                }
            }

            options = context_options;
            valid = true;
        }

        auto configure_connection(mbedtls_ssl_config& config) const -> bool
        {
            if (!valid)
                return false;

            auto result = mbedtls_ssl_config_defaults(
                &config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
            if (result != 0)
            {
                log_mbedtls_error("mbedtls_ssl_config_defaults(server)", result);
                return false;
            }

            configure_tls_versions(config);
            mbedtls_ssl_conf_authmode(&config, auth_mode(options.verify_peer));
            if (options.verify_peer != peer_verification::none)
                mbedtls_ssl_conf_ca_chain(&config, const_cast<mbedtls_x509_crt*>(&trust_anchor), nullptr);

            result = mbedtls_ssl_conf_own_cert(
                &config, const_cast<mbedtls_x509_crt*>(&certificate), const_cast<mbedtls_pk_context*>(&private_key));
            if (result != 0)
            {
                log_mbedtls_error("mbedtls_ssl_conf_own_cert", result);
                return false;
            }

            return true;
        }

        mbedtls_x509_crt certificate{};
        mbedtls_x509_crt trust_anchor{};
        mbedtls_pk_context private_key{};
        server_context_options options{};
        bool valid{false};
    };

    struct client_context::impl
    {
        impl() { mbedtls_x509_crt_init(&trust_anchor); }

        ~impl() { mbedtls_x509_crt_free(&trust_anchor); }

        auto initialise(
            const std::string& trust_anchor_pem,
            client_context_options context_options) -> void
        {
            if (!initialise_psa_crypto())
                return;

            if (!trust_anchor_pem.empty())
            {
                auto result = mbedtls_x509_crt_parse(
                    &trust_anchor,
                    reinterpret_cast<const unsigned char*>(trust_anchor_pem.c_str()),
                    trust_anchor_pem.size() + 1);
                if (result != 0)
                {
                    log_mbedtls_error("mbedtls_x509_crt_parse(client trust_anchor)", result);
                    return;
                }
                has_trust_anchor = true;
            }

            options = context_options;
            valid = true;
        }

        auto configure_connection(mbedtls_ssl_config& config) const -> bool
        {
            if (!valid)
                return false;

            auto result = mbedtls_ssl_config_defaults(
                &config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
            if (result != 0)
            {
                log_mbedtls_error("mbedtls_ssl_config_defaults(client)", result);
                return false;
            }

            configure_tls_versions(config);
            mbedtls_ssl_conf_authmode(&config, options.verify_peer ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
            if (has_trust_anchor)
                mbedtls_ssl_conf_ca_chain(&config, const_cast<mbedtls_x509_crt*>(&trust_anchor), nullptr);

            return true;
        }

        mbedtls_x509_crt trust_anchor{};
        client_context_options options{};
        bool has_trust_anchor{false};
        bool valid{false};
    };

    struct stream::impl
    {
        impl(
            std::shared_ptr<::streaming::stream> raw_stream,
            std::shared_ptr<context> server_context)
            : underlying(std::move(raw_stream))
            , tls_context(std::move(server_context))
        {
            initialise_server();
        }

        impl(
            std::shared_ptr<::streaming::stream> raw_stream,
            std::shared_ptr<client_context> client_context)
            : underlying(std::move(raw_stream))
            , tls_client_context(std::move(client_context))
        {
            initialise_client();
        }

        ~impl()
        {
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&config);
        }

        auto initialise_server() -> void
        {
            mbedtls_ssl_init(&ssl);
            mbedtls_ssl_config_init(&config);
            if (!underlying || !tls_context || !tls_context->is_valid())
            {
                RPC_ERROR("Mbed TLS stream missing underlying stream or TLS context");
                closed = true;
                return;
            }

            if (!rng.seed("canopy-mbedtls-server-stream"))
            {
                closed = true;
                return;
            }

            if (!tls_context->configure_connection(config))
            {
                closed = true;
                return;
            }
            peer_verification_policy = tls_context->peer_verification_mode();

            mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &rng.ctr_drbg);

            auto result = mbedtls_ssl_setup(&ssl, &config);
            if (result != 0)
            {
                log_mbedtls_error("mbedtls_ssl_setup", result);
                closed = true;
                return;
            }

            mbedtls_ssl_set_bio(&ssl, this, send_callback, receive_callback, nullptr);
        }

        auto initialise_client() -> void
        {
            mbedtls_ssl_init(&ssl);
            mbedtls_ssl_config_init(&config);
            if (!underlying || !tls_client_context || !tls_client_context->is_valid())
            {
                RPC_ERROR("Mbed TLS stream missing underlying stream or TLS client context");
                closed = true;
                return;
            }

            if (!rng.seed("canopy-mbedtls-client-stream"))
            {
                closed = true;
                return;
            }

            if (!tls_client_context->configure_connection(config))
            {
                closed = true;
                return;
            }
            peer_verification_policy
                = tls_client_context->verifies_peer() ? peer_verification::required : peer_verification::none;

            mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &rng.ctr_drbg);

            auto result = mbedtls_ssl_setup(&ssl, &config);
            if (result != 0)
            {
                log_mbedtls_error("mbedtls_ssl_setup", result);
                closed = true;
                return;
            }

            mbedtls_ssl_set_bio(&ssl, this, send_callback, receive_callback, nullptr);
        }

        static auto send_callback(
            void* context,
            const unsigned char* buffer,
            size_t size) -> int
        {
            return static_cast<impl*>(context)->append_output(buffer, size);
        }

        static auto receive_callback(
            void* context,
            unsigned char* buffer,
            size_t size) -> int
        {
            return static_cast<impl*>(context)->consume_input(buffer, size);
        }

        auto append_output(
            const unsigned char* buffer,
            size_t size) -> int
        {
            if (!buffer && size != 0)
                return ssl_internal_error();
            if (size > static_cast<size_t>(std::numeric_limits<int>::max()))
                return ssl_internal_error();

            try
            {
                encrypted_output.insert(encrypted_output.end(), buffer, buffer + size);
            }
            catch (...)
            {
                return ssl_internal_error();
            }

            return static_cast<int>(size);
        }

        auto consume_input(
            unsigned char* buffer,
            size_t size) -> int
        {
            if (!buffer && size != 0)
                return ssl_internal_error();

            const auto available = encrypted_input.size() - encrypted_input_offset;
            if (available == 0)
                return MBEDTLS_ERR_SSL_WANT_READ;

            const auto to_copy = std::min(size, available);
            if (to_copy > static_cast<size_t>(std::numeric_limits<int>::max()))
                return ssl_internal_error();

            std::memcpy(buffer, encrypted_input.data() + encrypted_input_offset, to_copy);
            encrypted_input_offset += to_copy;
            if (encrypted_input_offset == encrypted_input.size())
            {
                encrypted_input.clear();
                encrypted_input_offset = 0;
            }
            return static_cast<int>(to_copy);
        }

        auto feed_input(std::chrono::milliseconds timeout) -> coro::task<coro::net::io_status>
        {
            std::array<uint8_t, transfer_buffer_size> buffer{};
            auto [status, span] = co_await underlying->receive(rpc::mutable_byte_span{buffer}, timeout);
            if (!status.is_ok())
                co_return status;
            if (span.empty())
                co_return timeout_status();

            try
            {
                encrypted_input.insert(encrypted_input.end(), span.begin(), span.end());
            }
            catch (...)
            {
                co_return native_status(ssl_internal_error());
            }
            co_return ok_status();
        }

        auto drain_output() -> coro::task<coro::net::io_status>
        {
            while (!encrypted_output.empty())
            {
                auto output = std::move(encrypted_output);
                encrypted_output.clear();
                auto status = co_await underlying->send(rpc::byte_span{output});
                if (!status.is_ok())
                    co_return status;
            }
            co_return ok_status();
        }

        auto verify_peer_certificate(bool client) -> bool
        {
            if (peer_verification_policy == peer_verification::none)
                return true;

            const auto* peer_certificate = mbedtls_ssl_get_peer_cert(&ssl);
            if (!peer_certificate)
            {
                if (peer_verification_policy == peer_verification::required)
                {
                    RPC_WARNING(
                        "{} TLS handshake did not provide a required peer certificate",
                        client ? "Mbed TLS client" : "Mbed TLS server");
                    return false;
                }
                return true;
            }

            const auto verification_flags = mbedtls_ssl_get_verify_result(&ssl);
            if (verification_flags == 0)
                return true;

            std::array<char, 512> verify_info{};
            mbedtls_x509_crt_verify_info(verify_info.data(), verify_info.size(), "", verification_flags);
            RPC_WARNING(
                "{} peer certificate verification failed: {}",
                client ? "Mbed TLS client" : "Mbed TLS server",
                verify_info.data());
            return false;
        }

        auto perform_handshake(bool client) -> coro::task<bool>
        {
            if (closed)
                co_return false;

            auto lock = co_await ssl_mutex.scoped_lock();
            while (!closed)
            {
                auto result = mbedtls_ssl_handshake(&ssl);
                auto drain_status = co_await drain_output();
                if (!drain_status.is_ok())
                {
                    closed = drain_status.is_closed();
                    co_return false;
                }

                if (result == 0)
                {
                    if (!verify_peer_certificate(client))
                    {
                        closed = true;
                        co_return false;
                    }

                    handshake_complete = true;
                    co_return true;
                }

                if (result == MBEDTLS_ERR_SSL_WANT_READ)
                {
                    auto feed_status = co_await feed_input(std::chrono::milliseconds{0});
                    if (!feed_status.is_ok())
                    {
                        if (feed_status.is_timeout())
                            RPC_WARNING("Mbed TLS handshake: underlying stream timed out while waiting for peer data");
                        closed = feed_status.is_closed();
                        co_return false;
                    }
                    continue;
                }

                if (result == MBEDTLS_ERR_SSL_WANT_WRITE)
                    continue;

                log_mbedtls_handshake_error(client ? "Mbed TLS client" : "Mbed TLS server", result);
                closed = true;
                co_return false;
            }

            co_return false;
        }

        auto close_notify() -> coro::task<void>
        {
            auto lock = co_await ssl_mutex.scoped_lock();
            if (handshake_complete)
            {
                while (true)
                {
                    auto result = mbedtls_ssl_close_notify(&ssl);
                    auto drain_status = co_await drain_output();
                    if (!drain_status.is_ok() || result == 0)
                        break;
                    if (!is_want_io(result))
                        break;
                    if (result == MBEDTLS_ERR_SSL_WANT_READ)
                    {
                        auto feed_status = co_await feed_input(std::chrono::milliseconds{0});
                        if (!feed_status.is_ok())
                            break;
                    }
                }
            }
        }

        const std::shared_ptr<::streaming::stream> underlying;
        const std::shared_ptr<context> tls_context{};
        const std::shared_ptr<client_context> tls_client_context{};
        mbedtls_ssl_config config{};
        mbedtls_ssl_context ssl{};
        rng_context rng;
        std::vector<uint8_t> encrypted_input;
        std::vector<uint8_t> encrypted_output;
        size_t encrypted_input_offset{0};
        rpc::coro::mutex ssl_mutex;
        bool closed{false};
        bool handshake_complete{false};
        peer_verification peer_verification_policy{peer_verification::none};
    };

    auto load_pem_credentials(
        resource_reader reader,
        pem_resource_paths paths) -> coro::task<pem_credentials_result>
    {
        if (!reader || paths.certificate.empty() || paths.private_key.empty())
            co_return pem_credentials_result{rpc::error::INVALID_DATA(), {}};

        auto certificate = co_await reader(std::move(paths.certificate));
        if (certificate.error_code != rpc::error::OK())
            co_return pem_credentials_result{certificate.error_code, {}};

        auto private_key = co_await reader(std::move(paths.private_key));
        if (private_key.error_code != rpc::error::OK())
            co_return pem_credentials_result{private_key.error_code, {}};

        pem_credentials credentials{.certificate = bytes_to_string(certificate.data),
            .private_key = bytes_to_string(private_key.data),
            .trust_anchor = {}};

        if (!paths.trust_anchor.empty())
        {
            auto trust_anchor = co_await reader(std::move(paths.trust_anchor));
            if (trust_anchor.error_code != rpc::error::OK())
                co_return pem_credentials_result{trust_anchor.error_code, {}};
            credentials.trust_anchor = bytes_to_string(trust_anchor.data);
        }

        co_return pem_credentials_result{rpc::error::OK(), std::move(credentials)};
    }

    context::context(
        const std::string& cert_file,
        const std::string& key_file,
        server_context_options options)
        : impl_(std::make_unique<impl>())
    {
        pem_credentials credentials;
        if (!read_file_credentials(cert_file, key_file, credentials))
            return;

        impl_->initialise(credentials, options);
        RPC_DEBUG("Mbed TLS context initialized with certificate: {}", cert_file);
    }

    context::context(
        const pem_credentials& credentials,
        server_context_options options)
        : impl_(std::make_unique<impl>())
    {
        impl_->initialise(credentials, options);
        RPC_DEBUG("Mbed TLS context initialized from PEM credentials");
    }

    context::~context() = default;

    auto context::configure_connection(mbedtls_ssl_config& config) const -> bool
    {
        return impl_ && impl_->configure_connection(config);
    }

    auto context::peer_verification_mode() const -> peer_verification
    {
        return impl_ ? impl_->options.verify_peer : peer_verification::none;
    }

    auto context::is_valid() const -> bool
    {
        return impl_ && impl_->valid;
    }

    client_context::client_context(bool verify_peer)
        : impl_(std::make_unique<impl>())
    {
        impl_->initialise({}, client_context_options{.verify_peer = verify_peer});
    }

    client_context::client_context(client_context_options options)
        : impl_(std::make_unique<impl>())
    {
        impl_->initialise({}, options);
    }

    client_context::client_context(
        std::string trust_anchor,
        bool verify_peer)
        : impl_(std::make_unique<impl>())
    {
        impl_->initialise(trust_anchor, client_context_options{.verify_peer = verify_peer});
    }

    client_context::client_context(
        std::string trust_anchor,
        client_context_options options)
        : impl_(std::make_unique<impl>())
    {
        impl_->initialise(trust_anchor, options);
    }

    client_context::~client_context() = default;

    auto client_context::configure_connection(mbedtls_ssl_config& config) const -> bool
    {
        return impl_ && impl_->configure_connection(config);
    }

    auto client_context::verifies_peer() const -> bool
    {
        return impl_ && impl_->options.verify_peer;
    }

    auto client_context::is_valid() const -> bool
    {
        return impl_ && impl_->valid;
    }

    stream::stream(
        std::shared_ptr<::streaming::stream> underlying,
        std::shared_ptr<context> tls_ctx)
        : impl_(
              std::make_unique<impl>(
                  std::move(underlying),
                  std::move(tls_ctx)))
    {
    }

    stream::stream(
        std::shared_ptr<::streaming::stream> underlying,
        std::shared_ptr<client_context> client_ctx)
        : impl_(
              std::make_unique<impl>(
                  std::move(underlying),
                  std::move(client_ctx)))
    {
    }

    stream::~stream() = default;

    auto stream::handshake() -> coro::task<bool>
    {
        if (!impl_)
            co_return false;
        co_return co_await impl_->perform_handshake(false);
    }

    auto stream::client_handshake() -> coro::task<bool>
    {
        if (!impl_)
            co_return false;
        co_return co_await impl_->perform_handshake(true);
    }

    auto stream::receive(
        rpc::mutable_byte_span buffer,
        std::chrono::milliseconds timeout)
        -> coro::task<std::pair<
            coro::net::io_status,
            rpc::mutable_byte_span>>
    {
        if (!impl_ || impl_->closed || !impl_->handshake_complete)
            co_return {closed_status(), {}};

        auto lock = co_await impl_->ssl_mutex.scoped_lock();
        while (!impl_->closed)
        {
            auto bytes_read = mbedtls_ssl_read(&impl_->ssl, buffer.data(), buffer.size());
            auto drain_status = co_await impl_->drain_output();
            if (!drain_status.is_ok())
            {
                impl_->closed = drain_status.is_closed();
                co_return {drain_status, {}};
            }

            if (bytes_read > 0)
                co_return {ok_status(), buffer.subspan(0, static_cast<size_t>(bytes_read))};

            if (is_peer_closed(bytes_read))
            {
                impl_->closed = true;
                co_return {closed_status(), {}};
            }

            if (bytes_read == MBEDTLS_ERR_SSL_WANT_READ)
            {
                auto feed_status = co_await impl_->feed_input(timeout);
                if (!feed_status.is_ok())
                {
                    if (feed_status.is_closed())
                        impl_->closed = true;
                    co_return {feed_status, {}};
                }
                continue;
            }

            if (bytes_read == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;

            log_mbedtls_error("mbedtls_ssl_read", bytes_read);
            impl_->closed = true;
            co_return {closed_status(), {}};
        }

        co_return {closed_status(), {}};
    }

    auto stream::send(rpc::byte_span buffer) -> coro::task<coro::net::io_status>
    {
        if (!impl_ || impl_->closed || !impl_->handshake_complete)
            co_return closed_status();

        auto lock = co_await impl_->ssl_mutex.scoped_lock();
        size_t offset = 0;
        while (offset < buffer.size() && !impl_->closed)
        {
            const auto remaining = buffer.size() - offset;
            const auto chunk_size = std::min(remaining, static_cast<size_t>(std::numeric_limits<int>::max()));
            auto written = mbedtls_ssl_write(&impl_->ssl, buffer.data() + offset, chunk_size);
            auto drain_status = co_await impl_->drain_output();
            if (!drain_status.is_ok())
            {
                impl_->closed = drain_status.is_closed();
                co_return drain_status;
            }

            if (written > 0)
            {
                offset += static_cast<size_t>(written);
                continue;
            }

            if (is_peer_closed(written))
            {
                impl_->closed = true;
                co_return closed_status();
            }

            if (written == MBEDTLS_ERR_SSL_WANT_READ)
            {
                auto feed_status = co_await impl_->feed_input(std::chrono::milliseconds{0});
                if (!feed_status.is_ok())
                {
                    impl_->closed = feed_status.is_closed();
                    co_return feed_status;
                }
                continue;
            }

            if (written == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;

            log_mbedtls_error("mbedtls_ssl_write", written);
            impl_->closed = true;
            co_return closed_status();
        }

        co_return impl_->closed ? closed_status() : ok_status();
    }

    auto stream::is_closed() const -> bool
    {
        return !impl_ || impl_->closed;
    }

    auto stream::set_closed() -> coro::task<void>
    {
        if (!impl_ || impl_->closed)
            co_return;

        impl_->closed = true;
        co_await impl_->close_notify();

        if (impl_->underlying)
            co_await impl_->underlying->set_closed();

        co_return;
    }

    auto stream::get_peer_info() const -> peer_info
    {
        return impl_ && impl_->underlying ? impl_->underlying->get_peer_info() : peer_info{};
    }
} // namespace streaming::mbedtls
