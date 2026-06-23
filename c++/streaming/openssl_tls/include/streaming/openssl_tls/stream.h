// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <rpc/rpc.h>
#ifdef CANOPY_BUILD_COROUTINE
#  include <rpc/internal/coro_runtime/mutex.h>
#endif

#include <streaming/stream.h>

typedef struct bio_st BIO;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace streaming::openssl_tls
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

        [[nodiscard]] auto get() const -> SSL_CTX* { return ctx_; }
        [[nodiscard]] auto is_valid() const -> bool { return ctx_ != nullptr; }

    private:
        SSL_CTX* ctx_{nullptr};
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

        [[nodiscard]] auto get() const -> SSL_CTX* { return ctx_; }
        [[nodiscard]] auto is_valid() const -> bool { return ctx_ != nullptr; }
        [[nodiscard]] const std::string& server_name() const { return server_name_; }
        [[nodiscard]] bool verify_peer() const { return verify_peer_; }

    private:
        SSL_CTX* ctx_{nullptr};
        bool verify_peer_{false};
        std::string server_name_;
    };

    class stream : public ::streaming::stream
    {
    public:
        stream(
            std::shared_ptr<::streaming::stream> underlying,
            std::shared_ptr<context> tls_ctx,
            rpc::executor_ptr executor);
        stream(
            std::shared_ptr<::streaming::stream> underlying,
            std::shared_ptr<client_context> client_ctx,
            rpc::executor_ptr executor);
        ~stream() override;

        stream(const stream&) = delete;
        auto operator=(const stream&) -> stream& = delete;

        auto handshake() -> CORO_TASK(bool);
        auto client_handshake() -> CORO_TASK(bool);

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> CORO_TASK(::streaming::receive_result) override;

        auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) override;
        [[nodiscard]] auto is_closed() const -> bool override { return closed_.load(std::memory_order_acquire); }
        void request_close() noexcept override;
        auto set_closed() -> CORO_TASK(void) override;

        [[nodiscard]] auto get_peer_info() const -> peer_info override { return underlying_->get_peer_info(); }

    private:
        const std::shared_ptr<::streaming::stream> underlying_;
        const std::shared_ptr<context> tls_ctx_{};
        const std::shared_ptr<client_context> tls_client_ctx_{};
        const rpc::executor_ptr executor_;
        SSL* ssl_{nullptr};
        BIO* rbio_{nullptr};
        BIO* wbio_{nullptr};
        std::atomic<bool> closed_{false};
        bool handshake_complete_{false};
        bool client_handshake_configured_{false};
#ifdef CANOPY_BUILD_COROUTINE
        rpc::coro::mutex tls_mtx_;
#else
        std::mutex tls_mtx_;
#endif

        auto feed_rbio(std::chrono::milliseconds timeout) -> CORO_TASK(rpc::io_status);
        auto drain_wbio() -> CORO_TASK(bool);
    };
} // namespace streaming::openssl_tls
