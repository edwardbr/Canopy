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
 *       cmake --preset Debug_Coroutine
 *       cmake --build build_debug_coroutine --target tcp_spsc_tls_demo
 *       ./build_debug_coroutine/output/debug/demos/stream_composition/tcp_spsc_tls_demo
 *
 *   Options:
 *     --va-name   <name>       Virtual address name (e.g. "demo")
 *     --va-type   <type>       Zone address type: local | ipv4 | ipv6 | ipv6_tun
 *     --va-prefix <prefix>     Routing prefix (auto-detected if omitted)
 *     --listen    [name:]addr:port   Server bind address (default 0.0.0.0:19999)
 *     --connect   [name:]addr:port   Client connect address (default 127.0.0.1:19999)
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
#include <string_view>

// ---------------------------------------------------------------------------
// Test certificate generation
// ---------------------------------------------------------------------------

// Writes a self-signed RSA-2048 certificate and private key to the given
// paths using the OpenSSL API.  Used only for demo / testing purposes.
static bool generate_demo_cert(
    const std::string& cert_path,
    const std::string& key_path)
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
    run_server(
        std::shared_ptr<coro::scheduler> scheduler,
        rpc::event& server_ready,
        const rpc::event& client_finished,
        const canopy::network_config::tcp_endpoint& listen_ep,
        rpc::zone server_zone,
        const std::string& cert_path,
        const std::string& key_path,
        std::atomic<bool>& iteration_ok)
    {
        auto shutdown_event = std::make_shared<rpc::event>();
        auto service = rpc::root_service::create("echo_server", server_zone, scheduler);
        service->set_shutdown_event(shutdown_event);
        service->set_default_encoding(rpc::encoding::yas_binary);

        auto tls_ctx = std::make_shared<streaming::tls::context>(cert_path, key_path);
        if (!tls_ctx->is_valid())
        {
            RPC_ERROR("Server: TLS context init failed");
            iteration_ok.store(false);
            CO_RETURN;
        }

        const auto domain = listen_ep.family == canopy::network_config::ip_address_family::ipv6
                                ? coro::net::domain_t::ipv6
                                : coro::net::domain_t::ipv4;
        const coro::net::socket_address endpoint{
            coro::net::ip_address::from_string(listen_ep.to_string(), domain), listen_ep.port};

        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        auto tls_transformer = [tls_ctx, scheduler](std::shared_ptr<streaming::stream> tcp_stm)
            -> CORO_TASK(std::optional<std::shared_ptr<streaming::stream>>)
        {
            auto spsc_stm = streaming::spsc_wrapping::stream::create(tcp_stm, scheduler);
            auto tls_stm = std::make_shared<streaming::tls::stream>(spsc_stm, tls_ctx);
            if (!CO_AWAIT tls_stm->handshake())
                CO_RETURN std::nullopt;
            CO_RETURN tls_stm;
        };

        auto lst = std::make_shared<streaming::listener>(
            "server_transport",
            std::make_shared<streaming::tcp::acceptor>(endpoint),
            rpc::stream_transport::make_connection_callback<i_echo, i_echo>(
                [](const rpc::shared_ptr<i_echo>&,
                    const std::shared_ptr<rpc::service>&) -> CORO_TASK(rpc::service_connect_result<i_echo>)
                {
                    CO_RETURN rpc::service_connect_result<i_echo>{
                        rpc::error::OK(), rpc::shared_ptr<i_echo>(new echo_impl())};
                }),
            std::move(tls_transformer));

        if (!lst->start_listening(service))
        {
            RPC_ERROR("Server: failed to start listening");
            iteration_ok.store(false);
            CO_RETURN;
        }

        RPC_INFO("Server: listening on {}:{}", listen_ep.to_string(), listen_ep.port);
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
    run_client(
        std::shared_ptr<coro::scheduler> scheduler,
        const rpc::event& server_ready,
        rpc::event& client_finished,
        const canopy::network_config::tcp_endpoint& connect_ep,
        rpc::zone client_zone,
        std::atomic<bool>& iteration_ok)
    {
        co_await server_ready.wait();

        auto client_service = rpc::root_service::create("echo_client", client_zone, scheduler);
        client_service->set_default_encoding(rpc::encoding::yas_binary);

        RPC_INFO("Client: connecting to {}:{}", connect_ep.to_string(), connect_ep.port);

        const auto domain = connect_ep.family == canopy::network_config::ip_address_family::ipv6
                                ? coro::net::domain_t::ipv6
                                : coro::net::domain_t::ipv4;
        coro::net::tcp::client tcp_client(
            scheduler,
            coro::net::socket_address{coro::net::ip_address::from_string(connect_ep.to_string(), domain), connect_ep.port});

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
        auto spsc_stm = streaming::spsc_wrapping::stream::create(tcp_stm, scheduler);

        auto tls_client_ctx = std::make_shared<streaming::tls::client_context>(/*verify_peer=*/false);
        if (!tls_client_ctx->is_valid())
        {
            RPC_ERROR("Client: TLS client context init failed");
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }
        auto tls_stm = std::make_shared<streaming::tls::stream>(spsc_stm, tls_client_ctx);

        if (!CO_AWAIT tls_stm->client_handshake())
        {
            RPC_ERROR("Client: TLS handshake failed");
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }
        RPC_INFO("Client: TLS handshake complete");

        auto client_transport = rpc::stream_transport::make_client("client_transport", client_service, tls_stm);

        rpc::shared_ptr<i_echo> local_echo;
        rpc::shared_ptr<i_echo> remote_echo;

        auto connect_result
            = CO_AWAIT client_service->connect_to_zone<i_echo, i_echo>("echo_server", client_transport, local_echo);
        remote_echo = connect_result.output_interface;
        auto error = connect_result.error_code;

        if (error != rpc::error::OK())
        {
            RPC_ERROR("Client: connect_to_zone failed: {}", static_cast<int>(error));
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }
        RPC_INFO("Client: RPC connection established over TCP → SPSC → TLS");

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
namespace
{
    struct augmented_cli
    {
        int argc = 0;
        std::vector<std::string> storage;
        std::vector<char*> argv;
    };

    bool has_cli_option(
        int argc,
        char* argv[],
        std::string_view option)
    {
        const std::string with_equals = std::string(option) + "=";
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == option || arg.rfind(with_equals, 0) == 0)
                return true;
        }

        return false;
    }

    augmented_cli add_default_network_args(
        int argc,
        char* argv[])
    {
        augmented_cli result;
        result.storage.reserve(16);
        result.argv.reserve(argc + 16);

        for (int i = 0; i < argc; ++i)
            result.argv.push_back(argv[i]);

        const bool has_any_va
            = has_cli_option(argc, argv, "--va-name") || has_cli_option(argc, argv, "--va-type")
              || has_cli_option(argc, argv, "--va-prefix") || has_cli_option(argc, argv, "--va-subnet-bits")
              || has_cli_option(argc, argv, "--va-object-id-bits") || has_cli_option(argc, argv, "--va-subnet");
        const bool has_listen = has_cli_option(argc, argv, "--listen");
        const bool has_connect = has_cli_option(argc, argv, "--connect");

        auto append = [&result](std::initializer_list<const char*> args)
        {
            for (const char* arg : args)
            {
                result.storage.emplace_back(arg);
                result.argv.push_back(result.storage.back().data());
            }
        };

        if (!has_any_va)
        {
            append(
                {"--va-name=server",
                    "--va-type=ipv4",
                    "--va-prefix=127.0.0.1",
                    "--va-subnet-bits=32",
                    "--va-object-id-bits=32",
                    "--va-subnet=1",
                    "--va-name=client",
                    "--va-type=ipv4",
                    "--va-prefix=127.0.0.1",
                    "--va-subnet-bits=32",
                    "--va-object-id-bits=32",
                    "--va-subnet=100"});
        }

        if (!has_listen)
            append({"--listen=server:127.0.0.1:8080"});

        if (!has_connect)
            append({"--connect=client:127.0.0.1:8080"});

        result.argc = static_cast<int>(result.argv.size());
        return result;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(
    int argc,
    char* argv[])
{
    RPC_INFO("Stream Composition Demo — TCP → SPSC → TLS");
    RPC_INFO("============================================");

#ifndef CANOPY_BUILD_COROUTINE
    RPC_ERROR("This demo requires coroutines. Build with: cmake --preset Debug_Coroutine");
    return 1;
#else
    args::ArgumentParser parser("stream_composition demo: i_echo over TCP → SPSC → TLS.");
    args::HelpFlag help(parser, "help", "Display this help and exit", {'h', "help"});
    auto net = canopy::network_config::add_network_args(parser);
    auto cli = add_default_network_args(argc, argv);

    try
    {
        parser.ParseCLI(cli.argc, cli.argv.data());
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

    cfg.log_values();

    // Resolve listen endpoint.  Default: bind on 0.0.0.0:19999.
    canopy::network_config::tcp_endpoint listen_ep;
    if (const auto* p = cfg.first_listen())
        listen_ep = *p;
    else
        listen_ep.port = 19999; // addr = {} = 0.0.0.0, family = ipv4

    // Resolve connect endpoint.  Default: 127.0.0.1 on the listen port.
    canopy::network_config::tcp_endpoint connect_ep;
    if (const auto* p = cfg.first_connect())
    {
        connect_ep = *p;
    }
    else
    {
        connect_ep = listen_ep;
        const bool is_any = std::all_of(connect_ep.addr.begin(), connect_ep.addr.end(), [](uint8_t b) { return b == 0; });
        if (is_any)
            canopy::network_config::ipv4_to_ip_address("127.0.0.1", connect_ep.addr);
    }

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

        coro::sync_wait(
            coro::when_all(
                stream_composition::run_server(
                    scheduler_server, server_ready, client_finished, listen_ep, rpc::zone{server_addr}, cert_path, key_path, iteration_ok),
                stream_composition::run_client(
                    scheduler_client, server_ready, client_finished, connect_ep, rpc::zone{client_addr}, iteration_ok)));

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
