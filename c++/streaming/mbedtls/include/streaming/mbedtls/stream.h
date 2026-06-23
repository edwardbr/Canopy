// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <coro/task.hpp>

#include <rpc/rpc.h>

#include <streaming/stream.h>

typedef struct mbedtls_ssl_config mbedtls_ssl_config;

namespace streaming::mbedtls
{
    enum class peer_verification
    {
        none,
        optional,
        required
    };

    struct server_context_options
    {
        // Browser-facing servers usually leave this at none. Peer-to-peer and
        // RA-TLS modes can require a verified peer certificate.
        peer_verification verify_peer{peer_verification::none};
    };

    struct client_context_options
    {
        bool verify_peer{false};
        std::string server_name;
    };

    struct pem_credentials
    {
        std::string certificate;
        std::string private_key;
        std::string trust_anchor;
    };

    struct pem_resource_paths
    {
        std::string certificate;
        std::string private_key;
        std::string trust_anchor;
    };

    struct resource_read_result
    {
        int error_code{rpc::error::OK()};
        std::vector<uint8_t> data;
    };

    struct pem_credentials_result
    {
        int error_code{rpc::error::OK()};
        pem_credentials credentials;
    };

    using resource_reader = std::function<coro::task<resource_read_result>(std::string)>;

    auto load_pem_credentials(
        resource_reader reader,
        pem_resource_paths paths) -> coro::task<pem_credentials_result>;

    class context
    {
    public:
        context(
            const std::string& cert_file,
            const std::string& key_file,
            server_context_options options = {});
        explicit context(
            const pem_credentials& credentials,
            server_context_options options = {});
        ~context();

        context(const context&) = delete;
        auto operator=(const context&) -> context& = delete;

        [[nodiscard]] auto configure_connection(mbedtls_ssl_config& config) const -> bool;
        [[nodiscard]] auto peer_verification_mode() const -> peer_verification;
        [[nodiscard]] auto is_valid() const -> bool;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

    class client_context
    {
    public:
        explicit client_context(bool verify_peer = false);
        explicit client_context(client_context_options options);
        client_context(
            std::string trust_anchor,
            bool verify_peer);
        client_context(
            std::string trust_anchor,
            client_context_options options);
        ~client_context();

        client_context(const client_context&) = delete;
        auto operator=(const client_context&) -> client_context& = delete;

        [[nodiscard]] auto configure_connection(mbedtls_ssl_config& config) const -> bool;
        [[nodiscard]] auto verifies_peer() const -> bool;
        [[nodiscard]] auto is_valid() const -> bool;
        [[nodiscard]] auto server_name() const -> const std::string&;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

    class stream : public ::streaming::stream
    {
    public:
        stream(
            std::shared_ptr<::streaming::stream> underlying,
            std::shared_ptr<context> tls_ctx);
        stream(
            std::shared_ptr<::streaming::stream> underlying,
            std::shared_ptr<client_context> client_ctx);
        ~stream() override;

        stream(const stream&) = delete;
        auto operator=(const stream&) -> stream& = delete;

        auto handshake() -> coro::task<bool>;
        auto client_handshake() -> coro::task<bool>;

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<
                coro::net::io_status,
                rpc::mutable_byte_span>> override;

        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override;
        [[nodiscard]] auto is_closed() const -> bool override;
        auto set_closed() -> coro::task<void> override;

        [[nodiscard]] auto get_peer_info() const -> peer_info override;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
} // namespace streaming::mbedtls
