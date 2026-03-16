/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Stream Composition Demo — TCP → SPSC → TLS
 *
 *   Demonstrates that streaming::stream classes are fully composable.
 *   Each layer wraps the one below via the common stream interface:
 *
 *       [TCP socket]
 *           ↕  tcp_stream
 *       [SPSC buffering layer]
 *           ↕  spsc_wrapping_stream  (internal SPSC queues; no external pump)
 *       [TLS encryption]
 *           ↕  tls_stream
 *       [streaming_transport / RPC layer]
 *           ↕
 *       [i_echo interface]
 *
 *   The transport drives all I/O.  When it calls receive() the call propagates
 *   down through TLS → SPSC → TCP naturally.  No relay pump coroutines are
 *   needed because the transport guarantees exactly one active task per
 *   direction (receive_consumer_loop / send_producer_loop).
 *
 *   Build:
 *       cmake --preset Coroutine_Debug
 *       cmake --build build_coroutine_debug --target tcp_spsc_tls_demo
 *       ./build_coroutine_debug/output/debug/demos/stream_composition/tcp_spsc_tls_demo
 */

#include <echo_impl.h>
#include <rpc/rpc.h>

#include <streaming/listener.h>
#include <streaming/spsc_wrapping/stream.h>
#include <streaming/tcp/acceptor.h>
#include <streaming/tcp/stream.h>
#include <streaming/tls/stream.h>
#include <transports/streaming/transport.h>

#include <canopy/network_config/network_args.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>

// ---------------------------------------------------------------------------
// Test certificate generation
// ---------------------------------------------------------------------------

// Writes a self-signed RSA-2048 certificate and private key to the given
// paths using the OpenSSL API.  Used only for demo / testing purposes.
static bool generate_demo_cert(const std::string& cert_path, const std::string& key_path)
{
    if (std::filesystem::exists(cert_path) && std::filesystem::exists(key_path))
        return true; // already generated

    EVP_PKEY* pkey = nullptr;
    {
        EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (!kctx)
            return false;
        EVP_PKEY_keygen_init(kctx);
        EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
        EVP_PKEY_keygen(kctx, &pkey);
        EVP_PKEY_CTX_free(kctx);
    }
    if (!pkey)
        return false;

    X509* x509 = X509_new();
    if (!x509)
    {
        EVP_PKEY_free(pkey);
        return false;
    }

    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 10L * 365 * 24 * 60 * 60);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("stream_composition_demo"), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    FILE* cf = fopen(cert_path.c_str(), "wb");
    if (!cf)
    {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return false;
    }
    PEM_write_X509(cf, x509);
    fclose(cf);

    FILE* kf = fopen(key_path.c_str(), "wb");
    if (!kf)
    {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return false;
    }
    PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    fclose(kf);

    X509_free(x509);
    EVP_PKEY_free(pkey);
    RPC_INFO("Generated demo TLS certificate: {}", cert_path);
    return true;
}

// ---------------------------------------------------------------------------
// Demo tasks
// ---------------------------------------------------------------------------

namespace stream_composition
{
#ifdef CANOPY_BUILD_COROUTINE

    CORO_TASK(void)
    run_server(std::shared_ptr<coro::scheduler> scheduler,
        rpc::event& server_ready,
        const rpc::event& client_finished,
        const canopy::network_config::network_config& cfg,
        rpc::zone server_zone,
        const std::string& cert_path,
        const std::string& key_path,
        std::atomic<bool>& iteration_ok)
    {
        auto shutdown_event = std::make_shared<rpc::event>();
        auto service = std::make_shared<rpc::root_service>("echo_server", server_zone, scheduler);
        service->set_shutdown_event(shutdown_event);
        service->set_default_encoding(rpc::encoding::yas_binary);

        auto tls_ctx = std::make_shared<streaming::tls::context>(cert_path, key_path);
        if (!tls_ctx->is_valid())
        {
            RPC_ERROR("Server: TLS context init failed");
            iteration_ok.store(false);
            CO_RETURN;
        }

        // on_new_connection: called for each accepted TCP stream.
        // Builds the full stream chain and passes it to the transport.
        // No relay pumps — the transport drives all I/O through the chain.
        const auto domain = cfg.host_family == canopy::network_config::ip_address_family::ipv6
                                ? coro::net::domain_t::ipv6
                                : coro::net::domain_t::ipv4;
        const coro::net::socket_address endpoint{
            coro::net::ip_address::from_string(cfg.get_host_string(), domain), cfg.port};

        // Build the listener inside a scope so on_connection (which captures service)
        // is destroyed before the first co_await — otherwise the lambda would live in
        // the coroutine frame for the lifetime of run_server, keeping service alive and
        // preventing shutdown_event from firing.
        // stream_transformer: TCP → SPSC buffering → TLS handshake
        // Returns nullopt if the TLS handshake fails, rejecting the connection.
        auto tls_transformer = [tls_ctx, scheduler](std::shared_ptr<streaming::stream> tcp_stm)
            -> CORO_TASK(std::optional<std::shared_ptr<streaming::stream>>)
        {
            auto spsc_stm = streaming::spsc_wrapping::stream::create(tcp_stm, scheduler);
            auto tls_stm = std::make_shared<streaming::tls::stream>(spsc_stm, tls_ctx);
            if (!CO_AWAIT tls_stm->handshake())
                CO_RETURN std::nullopt;
            CO_RETURN tls_stm;
        };

        auto lst = std::make_shared<streaming::listener>("server_transport",
            std::make_shared<streaming::tcp::acceptor>(endpoint),
            rpc::stream_transport::make_connection_callback<i_echo, i_echo>(
                [](const rpc::shared_ptr<i_echo>&, rpc::shared_ptr<i_echo>& local, const std::shared_ptr<rpc::service>&)
                    -> CORO_TASK(int)
                {
                    local = rpc::shared_ptr<i_echo>(new echo_impl());
                    CO_RETURN rpc::error::OK();
                }),
            std::move(tls_transformer));

        if (!lst->start_listening(service))
        {
            RPC_ERROR("Server: failed to start listening");
            iteration_ok.store(false);
            CO_RETURN;
        }

        RPC_INFO("Server: listening on {}:{}", cfg.get_host_string(), cfg.port);
        service.reset();
        server_ready.set();

        co_await client_finished.wait();
        RPC_INFO("[run_server] client_finished — calling stop_listening");

        co_await lst->stop_listening();
        RPC_INFO("[run_server] stop_listening returned");
        lst.reset();
        RPC_INFO("[run_server] lst reset — awaiting shutdown_event");

        co_await shutdown_event->wait();
        RPC_INFO("Server: shutdown complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    run_client(std::shared_ptr<coro::scheduler> scheduler,
        const rpc::event& server_ready,
        rpc::event& client_finished,
        const canopy::network_config::network_config& cfg,
        rpc::zone client_zone,
        std::atomic<bool>& iteration_ok)
    {
        co_await server_ready.wait();

        auto client_service = std::make_shared<rpc::root_service>("echo_client", client_zone, scheduler);
        client_service->set_default_encoding(rpc::encoding::yas_binary);

        RPC_INFO("Client: connecting to {}:{}", cfg.get_host_string(), cfg.port);

        // 1. Establish TCP connection.
        const auto domain = cfg.host_family == canopy::network_config::ip_address_family::ipv6
                                ? coro::net::domain_t::ipv6
                                : coro::net::domain_t::ipv4;
        coro::net::tcp::client tcp_client(scheduler,
            coro::net::socket_address{coro::net::ip_address::from_string(cfg.get_host_string(), domain), cfg.port});

        auto conn_status = CO_AWAIT tcp_client.connect(std::chrono::milliseconds{5000});
        if (conn_status != coro::net::connect_status::connected)
        {
            RPC_ERROR("Client: TCP connect failed (status={})", static_cast<int>(conn_status));
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }
        RPC_INFO("Client: TCP connected");

        auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(tcp_client), scheduler);

        // 2. Wrap TCP in the SPSC buffering layer.
        auto spsc_stm = streaming::spsc_wrapping::stream::create(tcp_stm, scheduler);

        // 3. Wrap the SPSC stream in TLS (client side — SSL_VERIFY_NONE for demo).
        auto tls_client_ctx = std::make_shared<streaming::tls::client_context>(/*verify_peer=*/false);
        if (!tls_client_ctx->is_valid())
        {
            RPC_ERROR("Client: TLS client context init failed");
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }
        auto tls_stm = std::make_shared<streaming::tls::stream>(spsc_stm, tls_client_ctx);

        // 4. TLS client handshake.
        if (!CO_AWAIT tls_stm->client_handshake())
        {
            RPC_ERROR("Client: TLS handshake failed");
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }
        RPC_INFO("Client: TLS handshake complete");

        // 5. Create streaming_transport (client side — no connection_handler).
        auto client_transport = rpc::stream_transport::transport::make_client("client_transport", client_service, tls_stm);

        // 6. Connect to the remote zone and obtain the i_echo proxy.
        rpc::shared_ptr<i_echo> local_echo;
        rpc::shared_ptr<i_echo> remote_echo;

        auto error = CO_AWAIT client_service->connect_to_zone("echo_server", client_transport, local_echo, remote_echo);

        if (error != rpc::error::OK())
        {
            RPC_ERROR("Client: connect_to_zone failed: {}", static_cast<int>(error));
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }
        RPC_INFO("Client: RPC connection established over TCP → SPSC → TLS");

        // 7. Make echo calls.
        const std::vector<std::string> messages = {
            "Hello from stream_composition demo!",
            "TCP -> SPSC -> TLS composition works!",
            "Streams are fully composable.",
        };

        for (const auto& msg : messages)
        {
            std::string response;
            error = CO_AWAIT remote_echo->echo(msg, response);
            if (error != rpc::error::OK())
            {
                RPC_ERROR("Client: echo() failed: {}", static_cast<int>(error));
                iteration_ok.store(false);
                break;
            }
            RPC_INFO("Client: echo('{}') -> '{}'", msg, response);
        }

        remote_echo.reset();
        client_finished.set();
        CO_RETURN;
    }

#endif // CANOPY_BUILD_COROUTINE
} // namespace stream_composition

// ---------------------------------------------------------------------------
// Logging hook
// ---------------------------------------------------------------------------

extern "C"
{
    void rpc_log(int level, const char* str, size_t sz)
    {
        std::string message(str, sz);
        const char* prefix = nullptr;
        switch (level)
        {
        case 0:
            prefix = "[TRACE]";
            break;
        case 1:
            prefix = "[DEBUG]";
            break;
        case 2:
            prefix = "[INFO] ";
            break;
        case 3:
            prefix = "[WARN] ";
            break;
        case 4:
            prefix = "[ERROR]";
            break;
        default:
            prefix = "[LOG]  ";
            break;
        }
        printf("%s %s\n", prefix, message.c_str());
        fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    RPC_INFO("Stream Composition Demo — TCP → SPSC → TLS");
    RPC_INFO("============================================");

#ifndef CANOPY_BUILD_COROUTINE
    RPC_ERROR("This demo requires coroutines. Build with: cmake --preset Coroutine_Debug");
    return 1;
#else
    args::ArgumentParser parser("stream_composition demo: i_echo over TCP → SPSC → TLS.");
    args::HelpFlag help(parser, "help", "Display this help and exit", {'h', "help"});
    auto net = canopy::network_config::add_network_args(parser);

    try
    {
        parser.ParseCLI(argc, argv);
    }
    catch (const args::Help&)
    {
        std::cout << parser;
        return 0;
    }
    catch (const args::ParseError& e)
    {
        std::cerr << e.what() << "\n" << parser;
        return 1;
    }

    canopy::network_config::network_config cfg;
    try
    {
        cfg = net.get_config();
    }
    catch (const std::invalid_argument& e)
    {
        RPC_ERROR("Configuration error: {}", e.what());
        return 1;
    }

    if (cfg.port == 0)
        cfg.port = 19999;

    cfg.log_values();

    // Generate (or reuse) a self-signed TLS certificate for the demo server.
    std::string cert_dir = std::string(DEMO_CERT_DIR);
    std::string cert_path = cert_dir + "/cert.pem";
    std::string key_path = cert_dir + "/key.pem";

    if (!generate_demo_cert(cert_path, key_path))
    {
        RPC_ERROR("Failed to generate demo TLS certificate in: {}", cert_dir);
        return 1;
    }

    auto allocator = canopy::network_config::make_allocator(cfg);

    rpc::zone_address server_addr;
    rpc::zone_address client_addr;
    allocator.allocate_zone(server_addr);
    allocator.allocate_zone(client_addr);

    auto scheduler_server = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{.thread_count = 1},
            .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));

    auto scheduler_client = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{.thread_count = 1},
            .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));

    constexpr int iterations = 5;
    std::atomic<int> failures{0};

    for (int i = 0; i < iterations; ++i)
    {
        RPC_INFO("\n=== Iteration {}/{} ===", i + 1, iterations);

        rpc::event server_ready;
        rpc::event client_finished;
        std::atomic<bool> iteration_ok{true};

        coro::sync_wait(coro::when_all(
            stream_composition::run_server(
                scheduler_server, server_ready, client_finished, cfg, rpc::zone{server_addr}, cert_path, key_path, iteration_ok),
            stream_composition::run_client(
                scheduler_client, server_ready, client_finished, cfg, rpc::zone{client_addr}, iteration_ok)));

        if (!iteration_ok.load())
        {
            RPC_ERROR("Iteration {} failed", i + 1);
            ++failures;
        }
        else
        {
            RPC_INFO("Iteration {} passed", i + 1);
        }
    }

    RPC_INFO("\n============================================");
    RPC_INFO("Stream Composition Demo complete: {}/{} iterations passed", iterations - failures.load(), iterations);

    scheduler_server->shutdown();
    scheduler_client->shutdown();

    return failures == 0 ? 0 : 1;
#endif
}
