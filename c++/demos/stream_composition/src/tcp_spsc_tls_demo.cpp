/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Stream Composition Demo — TCP → SPSC buffered stream → TLS
 *
 *   Demonstrates that streaming::stream classes are fully composable.
 *   Each layer wraps the one below via the common stream interface:
 *
 *       [TCP socket]
 *           ↕  tcp_stream
 *       [SPSC buffering layer]
 *           ↕  spsc_buffered_stream  (internal SPSC queues; no external pump)
 *       [TLS encryption]
 *           ↕  secure_stream          (OpenSSL or mbedTLS, selected by CMake)
 *       [streaming_transport / RPC layer]
 *           ↕
 *       [i_echo interface]
 *
 *   The transport still sees a normal streaming::stream. When it calls send()
 *   or receive(), TLS talks to spsc_buffered_stream, and that adapter moves
 *   bytes through its private SPSC queues to or from TCP. The queues are not a
 *   configured endpoint rendezvous; they are only a local buffering boundary in
 *   this stream stack.
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
#include <connection_factory/detail/stream_rpc.h>

#include <streaming/listener.h>
#include <streaming/spsc_buffered_stream/stream.h>
#include <streaming/tcp_coroutine/acceptor.h>
#include <streaming/tcp_coroutine/connector.h>
#include <streaming/tcp_coroutine/stream.h>
#include <streaming/secure_stream.h>
#include <transports/streaming/transport.h>

#include <io_uring/host_io_uring.h>

#include <canopy/network_config/cli_args.h>
#include <canopy/network_config/endpoint.h>
#include <canopy/network_config/zone.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>

// ---------------------------------------------------------------------------
// Test certificate preparation
// ---------------------------------------------------------------------------

#ifndef DEMO_CERT_FIXTURE_DIR
#  error "DEMO_CERT_FIXTURE_DIR must point at the stream-composition-local TLS certificate fixtures"
#endif

// Copies the demo-local fixture certificate into the binary tree. The secure stream backend remains selected by CMake;
// this helper just avoids requiring OpenSSL in this demo executable solely to generate test credentials.
static bool prepare_demo_cert(
    const std::string& cert_path,
    const std::string& key_path)
{
    if (std::filesystem::exists(cert_path) && std::filesystem::exists(key_path))
        return true;

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(cert_path).parent_path(), error);
    if (error)
        return false;

    const auto fixture_dir = std::filesystem::path(DEMO_CERT_FIXTURE_DIR);
    std::filesystem::copy_file(
        fixture_dir / "server.crt", cert_path, std::filesystem::copy_options::overwrite_existing, error);
    if (error)
        return false;

    std::filesystem::copy_file(
        fixture_dir / "server.key", key_path, std::filesystem::copy_options::overwrite_existing, error);
    if (error)
        return false;

    RPC_INFO("Prepared demo TLS certificate: {}", cert_path);
    return true;
}

// ---------------------------------------------------------------------------
// Demo tasks
// ---------------------------------------------------------------------------

namespace stream_composition
{
#ifdef CANOPY_BUILD_COROUTINE
    namespace
    {
        auto make_tcp_coroutine_controller(
            std::shared_ptr<coro::scheduler> scheduler,
            const char* role) -> std::shared_ptr<rpc::io_uring::controller>
        {
            rpc::io_uring::linux_io_uring_handle::options handle_options;
            handle_options.queue_depth = 256;
            handle_options.buffer_count = 128;
            handle_options.buffer_size = 64U * 1024U;
            handle_options.fixed_file_count = 256;
            handle_options.register_fixed_files = true;

            std::shared_ptr<rpc::io_uring::linux_io_uring_handle> handle;
            const auto error = rpc::io_uring::linux_io_uring_handle::create(handle, handle_options, scheduler);
            if (error != rpc::error::OK())
            {
                RPC_ERROR("{}: failed to create TCP coroutine io_uring handle error={}", role, error);
                return {};
            }

            return std::make_shared<rpc::io_uring::controller>(
                std::move(handle), scheduler.get(), rpc::io_uring::default_controller_options());
        }

        auto ipv4_address(const canopy::network_config::tcp_endpoint& endpoint) -> std::array<
            uint8_t,
            4>
        {
            return {endpoint.addr[0], endpoint.addr[1], endpoint.addr[2], endpoint.addr[3]};
        }

        auto ipv6_address(const canopy::network_config::tcp_endpoint& endpoint) -> std::array<
            uint8_t,
            16>
        {
            std::array<uint8_t, 16> result{};
            std::copy(endpoint.addr.begin(), endpoint.addr.end(), result.begin());
            return result;
        }
    } // namespace

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

        auto tls_ctx = std::make_shared<streaming::secure::context>(cert_path, key_path);
        if (!tls_ctx->is_valid())
        {
            RPC_ERROR("Server: TLS context init failed");
            iteration_ok.store(false);
            CO_RETURN;
        }

        auto controller = make_tcp_coroutine_controller(scheduler, "Server");
        if (!controller)
        {
            iteration_ok.store(false);
            CO_RETURN;
        }

        auto acceptor = std::make_shared<streaming::coroutine::tcp::acceptor>(controller);
        int listen_error = rpc::error::OK();
        if (listen_ep.family == canopy::network_config::ip_address_family::ipv6)
            listen_error = CO_AWAIT acceptor->listen_ipv6(ipv6_address(listen_ep), listen_ep.port);
        else
            listen_error = CO_AWAIT acceptor->listen_ipv4(ipv4_address(listen_ep), listen_ep.port);

        if (listen_error != rpc::error::OK())
        {
            RPC_ERROR("Server: TCP coroutine listen failed: {}", listen_error);
            iteration_ok.store(false);
            CO_RETURN;
        }

        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        auto tls_transformer = [tls_ctx, scheduler](std::shared_ptr<streaming::stream> tcp_stm)
            -> CORO_TASK(std::optional<std::shared_ptr<streaming::stream>>)
        {
            auto spsc_stm = streaming::spsc_buffered_stream::stream::create(tcp_stm, scheduler);
            auto tls_stm = std::make_shared<streaming::secure::stream>(spsc_stm, tls_ctx, scheduler);
            if (!CO_AWAIT tls_stm->handshake())
                CO_RETURN std::nullopt;
            CO_RETURN tls_stm;
        };

        rpc::connection_factory::stream_rpc_connection_settings options;
        options.listener.name = "server_transport";
        options.transport.name = "server_transport";
        options.transport.encoding = rpc::encoding::yas_binary;
        auto accept_result = CO_AWAIT rpc::connection_factory::accept_rpc_listener<i_echo, i_echo>(
            std::move(acceptor),
            [](const rpc::shared_ptr<i_echo>&,
                const std::shared_ptr<rpc::service>&) -> CORO_TASK(rpc::service_connect_result<i_echo>)
            {
                CO_RETURN rpc::service_connect_result<i_echo>{rpc::error::OK(), rpc::shared_ptr<i_echo>(new echo_impl())};
            },
            options,
            service,
            {},
            listen_ep.port,
            {},
            std::move(tls_transformer));

        if (accept_result.error_code != rpc::error::OK() || !accept_result.handle)
        {
            RPC_ERROR("Server: failed to start listening");
            iteration_ok.store(false);
            CO_RETURN;
        }
        auto listener = std::move(accept_result.handle);

        RPC_INFO("Server: listening on {}:{}", listen_ep.to_string(), listen_ep.port);
        service.reset();
        server_ready.set();

        co_await client_finished.wait();
        RPC_INFO("[run_server] client_finished — calling stop_listening");

        co_await listener->stop();
        RPC_INFO("[run_server] stop_listening returned");
        listener.reset();
        RPC_INFO("[run_server] listener reset — awaiting shutdown_event");

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

        auto controller = make_tcp_coroutine_controller(scheduler, "Client");
        if (!controller)
        {
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }

        auto tcp_result = CO_AWAIT streaming::coroutine::tcp::connect_loopback(
            controller,
            connect_ep.port,
            streaming::coroutine::tcp::default_stream_options(),
            std::chrono::milliseconds{5000},
            scheduler);
        if (tcp_result.error_code != rpc::error::OK() || !tcp_result.connection)
        {
            RPC_ERROR("Client: TCP coroutine connect failed: {}", tcp_result.error_code);
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }
        RPC_INFO("Client: TCP connected");

        auto spsc_stm = streaming::spsc_buffered_stream::stream::create(std::move(tcp_result.connection), scheduler);

        auto tls_client_ctx = std::make_shared<streaming::secure::client_context>(/*verify_peer=*/false);
        if (!tls_client_ctx->is_valid())
        {
            RPC_ERROR("Client: TLS client context init failed");
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }
        auto tls_stm = std::make_shared<streaming::secure::stream>(spsc_stm, tls_client_ctx, scheduler);

        if (!CO_AWAIT tls_stm->client_handshake())
        {
            RPC_ERROR("Client: TLS handshake failed");
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }
        RPC_INFO("Client: TLS handshake complete");

        rpc::shared_ptr<i_echo> local_echo;
        rpc::shared_ptr<i_echo> remote_echo;

        rpc::connection_factory::stream_rpc_connection_settings options;
        options.transport.name = "client_transport";
        options.transport.service_proxy_name = "echo_server";
        options.transport.encoding = rpc::encoding::yas_binary;
        auto connect_result = CO_AWAIT rpc::connection_factory::connect_rpc_stream<i_echo, i_echo>(
            local_echo, tls_stm, options, client_service);
        remote_echo = connect_result.output_interface;
        auto error = connect_result.error_code;

        if (error != rpc::error::OK())
        {
            RPC_ERROR("Client: connect_to_zone failed: {}", static_cast<int>(error));
            iteration_ok.store(false);
            client_finished.set();
            CO_RETURN;
        }
        RPC_INFO("Client: RPC connection established over TCP → SPSC buffered stream → TLS");

        const std::vector<std::string> messages = {
            "Hello from stream_composition demo!",
            "TCP -> SPSC buffered stream -> TLS composition works!",
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
    RPC_INFO("Stream Composition Demo — TCP → SPSC buffered stream → TLS");
    RPC_INFO("============================================");

#ifndef CANOPY_BUILD_COROUTINE
    RPC_ERROR("This demo requires coroutines. Build with: cmake --preset Debug_Coroutine");
    return 1;
#else
    args::ArgumentParser parser("stream_composition demo: i_echo over TCP → SPSC buffered stream → TLS.");
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

    // Prepare (or reuse) a self-signed TLS certificate for the demo server.
    std::string cert_dir = std::string(DEMO_CERT_DIR);
    std::string cert_path = cert_dir + "/cert.pem";
    std::string key_path = cert_dir + "/key.pem";

    if (!prepare_demo_cert(cert_path, key_path))
    {
        RPC_ERROR("Failed to prepare demo TLS certificate in: {}", cert_dir);
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
