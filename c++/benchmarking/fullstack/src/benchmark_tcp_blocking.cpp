/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#include "benchmark_data_processor.h"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <thread>

#ifdef CANOPY_BUILD_COROUTINE
#  include <coro/net/tcp/server.hpp>
#endif

#include <streaming/tcp_blocking/stream.h>
#include <transports/streaming/transport.h>

#ifdef CANOPY_FULLSTACK_BENCHMARK_HAS_TLS
#  include <streaming/secure_stream.h>
#endif

#ifdef CANOPY_BUILD_WEBSOCKET
#  include <streaming/websocket/stream.h>
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace comprehensive::v1
{
    namespace
    {
        struct benchmark_stream_pair
        {
            std::shared_ptr<streaming::stream> server;
            std::shared_ptr<streaming::stream> client;
#ifdef CANOPY_BUILD_COROUTINE
            std::shared_ptr<coro::scheduler> server_scheduler;
            std::shared_ptr<coro::scheduler> client_scheduler;
#endif

            void shutdown()
            {
#ifdef CANOPY_BUILD_COROUTINE
                server.reset();
                client.reset();
                server_scheduler.reset();
                client_scheduler.reset();
#endif
            }
        };

#ifdef CANOPY_FULLSTACK_BENCHMARK_HAS_TLS
        struct benchmark_tls_cert_pair
        {
            std::string cert_path;
            std::string key_path;
            bool valid = false;

            benchmark_tls_cert_pair()
            {
                const auto cert_dir = std::filesystem::path(CANOPY_FULLSTACK_BENCHMARK_CERT_DIR);
                cert_path = (cert_dir / "server.crt").string();
                key_path = (cert_dir / "server.key").string();
                valid = std::filesystem::exists(cert_path) && std::filesystem::exists(key_path);
            }
        };
#endif

        rpc::zone_address make_client_zone_address()
        {
            return *rpc::zone_address::create(
                rpc::zone_address_args(
                    rpc::default_values::version_3,
                    rpc::address_type::local,
                    0,
                    {},
                    rpc::default_values::default_subnet_size_bits,
                    2,
                    rpc::default_values::default_object_id_size_bits,
                    1,
                    {}));
        }

#ifndef CANOPY_BUILD_COROUTINE
        void close_fd(int& fd) noexcept
        {
            if (fd >= 0)
            {
                ::close(fd);
                fd = -1;
            }
        }

        int make_loopback_listener(uint16_t port) noexcept
        {
            int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listen_fd < 0)
                return -1;

            int reuse = 1;
            ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);

            if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0
                || ::listen(listen_fd, 16) != 0)
            {
                close_fd(listen_fd);
                return -1;
            }

            return listen_fd;
        }

        int connect_loopback(uint16_t port) noexcept
        {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                return -1;

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
            {
                close_fd(fd);
                return -1;
            }

            return fd;
        }
#endif

        bool make_tcp_stream_pair(
            benchmark_stream_pair& pair,
            uint16_t port)
        {
#ifdef CANOPY_BUILD_COROUTINE
            const coro::net::socket_address endpoint{"127.0.0.1", port};
            pair.server_scheduler = make_benchmark_scheduler();
            pair.client_scheduler = make_benchmark_scheduler();

            rpc::event server_ready;
            std::shared_ptr<streaming::stream> server_stream;
            std::shared_ptr<streaming::stream> client_stream;
            coro::sync_wait(
                coro::when_all(
                    [&]() -> coro::task<void>
                    {
                        coro::net::tcp::server server(pair.server_scheduler, endpoint);
                        server_ready.set();
                        auto accepted = co_await server.accept(std::chrono::milliseconds(5000));
                        if (!accepted)
                        {
                            std::cerr << "tcp_pair: accept failed\n";
                            co_return;
                        }
                        server_stream = std::make_shared<streaming::blocking::tcp::stream>(
                            std::move(*accepted), pair.server_scheduler);
                    }(),
                    [&]() -> coro::task<void>
                    {
                        co_await server_ready.wait();
                        coro::net::tcp::client client(pair.client_scheduler, endpoint);
                        const auto connection_status = co_await client.connect(std::chrono::milliseconds(5000));
                        if (connection_status != coro::net::connect_status::connected)
                        {
                            std::cerr << "tcp_pair: connect failed with status " << static_cast<int>(connection_status)
                                      << '\n';
                            co_return;
                        }
                        client_stream = std::make_shared<streaming::blocking::tcp::stream>(
                            std::move(client), pair.client_scheduler);
                    }()));

            pair.server = std::move(server_stream);
            pair.client = std::move(client_stream);
            if (pair.server && pair.client)
                return true;

            pair.shutdown();
            return false;
#else
            int listen_fd = make_loopback_listener(port);
            if (listen_fd < 0)
                return false;

            int client_fd = connect_loopback(port);
            if (client_fd < 0)
            {
                close_fd(listen_fd);
                return false;
            }

            sockaddr_in peer_addr{};
            socklen_t peer_addr_len = sizeof(peer_addr);
            int server_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_addr_len);
            close_fd(listen_fd);
            if (server_fd < 0)
            {
                close_fd(client_fd);
                return false;
            }

            pair.server = std::make_shared<streaming::blocking::tcp::stream>(streaming::blocking::tcp::socket(server_fd));
            pair.client = std::make_shared<streaming::blocking::tcp::stream>(streaming::blocking::tcp::socket(client_fd));
            return true;
#endif
        }

#ifdef CANOPY_FULLSTACK_BENCHMARK_HAS_TLS
        bool wrap_tls(benchmark_stream_pair& pair)
        {
            benchmark_tls_cert_pair cert;
            if (!cert.valid)
                return false;

            auto server_context = std::make_shared<streaming::secure::context>(cert.cert_path, cert.key_path);
            auto client_context = std::make_shared<streaming::secure::client_context>(false);
            if (!server_context->is_valid() || !client_context->is_valid())
                return false;

            std::shared_ptr<streaming::stream> server_tls;
            std::shared_ptr<streaming::stream> client_tls;
#  ifdef CANOPY_BUILD_COROUTINE
            coro::sync_wait(
                coro::when_all(
                    pair.server_scheduler->schedule(
                        [&]() -> coro::task<void>
                        {
                            auto stream = std::make_shared<streaming::secure::stream>(pair.server, server_context);
                            if (co_await stream->handshake())
                                server_tls = stream;
                        }()),
                    pair.client_scheduler->schedule(
                        [&]() -> coro::task<void>
                        {
                            auto stream = std::make_shared<streaming::secure::stream>(pair.client, client_context);
                            if (co_await stream->client_handshake())
                                client_tls = stream;
                        }())));
#  else
            std::thread server_thread(
                [&]()
                {
                    auto stream = std::make_shared<streaming::secure::stream>(pair.server, server_context);
                    if (stream->handshake())
                        server_tls = stream;
                });
            std::thread client_thread(
                [&]()
                {
                    auto stream = std::make_shared<streaming::secure::stream>(pair.client, client_context);
                    if (stream->client_handshake())
                        client_tls = stream;
                });
            server_thread.join();
            client_thread.join();
#  endif

            if (!server_tls || !client_tls)
                return false;

            pair.server = std::move(server_tls);
            pair.client = std::move(client_tls);
            return true;
        }
#endif

#ifdef CANOPY_BUILD_WEBSOCKET
        bool wrap_websocket(benchmark_stream_pair& pair)
        {
            pair.server = std::make_shared<streaming::websocket::stream>(pair.server);
            pair.client
                = std::make_shared<streaming::websocket::stream>(pair.client, streaming::websocket::stream_role::client);
            return true;
        }
#endif

        CORO_TASK(void)
        stream_server_task(
#ifdef CANOPY_BUILD_COROUTINE
            std::shared_ptr<coro::scheduler> scheduler,
#else
            rpc::executor_ptr executor,
#endif
            std::shared_ptr<streaming::stream> stream,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            rpc::encoding enc)
        {
#ifdef CANOPY_BUILD_COROUTINE
            auto service = rpc::root_service::create("stream_server", rpc::DEFAULT_PREFIX, scheduler);
#else
            auto service = rpc::root_service::create("stream_server", rpc::DEFAULT_PREFIX, executor);
#endif
            service->set_default_encoding(enc);

            auto server_transport = CO_AWAIT service->make_acceptor<i_data_processor, i_data_processor>(
                "server_transport",
                rpc::stream_transport::transport_factory(std::move(stream)),
                [](const rpc::shared_ptr<i_data_processor>&,
                    const std::shared_ptr<rpc::service>&) -> CORO_TASK(rpc::service_connect_result<i_data_processor>)
                {
                    auto local = make_benchmark_data_processor();
                    CO_RETURN rpc::service_connect_result<i_data_processor>{rpc::error::OK(), std::move(local)};
                });

            server_ready.set();
            const auto accept_error = CO_AWAIT server_transport->accept();
            if (accept_error != rpc::error::OK())
                std::cerr << "stream_server: accept failed with error " << accept_error << '\n';

            CO_AWAIT client_finished.wait();
#ifdef CANOPY_BUILD_COROUTINE
            const auto disconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
            while (server_transport->get_status() < rpc::transport_status::DISCONNECTED
                   && std::chrono::steady_clock::now() < disconnect_deadline)
            {
                CO_AWAIT scheduler->schedule_after(std::chrono::milliseconds(1));
            }
#endif
            server_transport.reset();
            service.reset();
            CO_RETURN;
        }

        CORO_TASK(void)
        stream_client_task(
#ifdef CANOPY_BUILD_COROUTINE
            std::shared_ptr<coro::scheduler> scheduler,
#else
            rpc::executor_ptr executor,
#endif
            std::shared_ptr<streaming::stream> stream,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            rpc::encoding enc,
            size_t blob_size,
            benchmark_result& result)
        {
            CO_AWAIT server_ready.wait();

#ifdef CANOPY_BUILD_COROUTINE
            auto client_service = rpc::root_service::create("stream_client", make_client_zone_address(), scheduler);
#else
            auto client_service = rpc::root_service::create("stream_client", make_client_zone_address(), executor);
#endif
            client_service->set_default_encoding(enc);

            auto client_transport
                = rpc::stream_transport::make_client("client_transport", client_service, std::move(stream));

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;

            auto connect_result = CO_AWAIT client_service->connect_to_zone<i_data_processor, i_data_processor>(
                "stream_server", client_transport, not_used);
            remote_processor = std::move(connect_result.output_interface);
            const auto error = connect_result.error_code;
            not_used = nullptr;

            if (error != rpc::error::OK())
            {
                std::cerr << "stream_client: connect_to_zone failed with error " << error << '\n';
                result.error = error;
                client_finished.set();
                client_transport.reset();
                client_service.reset();
                CO_RETURN;
            }

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_ns;
            result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_ns, tcp_warmup_calls);
            if (result.error == rpc::error::OK())
                result.stats = compute_stats(durations_ns);

            remote_processor.reset();
            client_finished.set();
            client_transport.reset();
            client_service.reset();
            CO_RETURN;
        }

        template<class PairBuilder>
        benchmark_result run_streaming_transport_benchmark(
            rpc::encoding enc,
            size_t blob_size,
            uint16_t port,
            PairBuilder build_pair)
        {
            benchmark_result result{};
            benchmark_stream_pair pair;
            if (!make_tcp_stream_pair(pair, port) || !build_pair(pair))
            {
                result.error = rpc::error::ZONE_NOT_FOUND();
                pair.shutdown();
                return result;
            }

            rpc::event server_ready;
            rpc::event client_finished;

#ifdef CANOPY_BUILD_COROUTINE
            auto weak_server_scheduler = std::weak_ptr<coro::scheduler>(pair.server_scheduler);
            auto weak_client_scheduler = std::weak_ptr<coro::scheduler>(pair.client_scheduler);
            coro::sync_wait(
                coro::when_all(
                    stream_server_task(pair.server_scheduler, pair.server, server_ready, client_finished, enc),
                    stream_client_task(
                        pair.client_scheduler, pair.client, server_ready, client_finished, enc, blob_size, result)));
            pair.shutdown();
            wait_for_scheduler_cleanup(weak_server_scheduler);
            wait_for_scheduler_cleanup(weak_client_scheduler);
#else
            auto server_executor = std::make_shared<rpc::executor>();
            auto client_executor = std::make_shared<rpc::executor>();
            std::thread server_thread(
                [&]() { stream_server_task(server_executor, pair.server, server_ready, client_finished, enc); });
            std::thread client_thread(
                [&]()
                {
                    stream_client_task(client_executor, pair.client, server_ready, client_finished, enc, blob_size, result);
                });
            client_thread.join();
            server_thread.join();
            server_executor->shutdown();
            client_executor->shutdown();
            pair.shutdown();
#endif
            return result;
        }
    } // namespace

    benchmark_result run_tcp_blocking_benchmark(
        rpc::encoding enc,
        size_t blob_size,
        uint16_t port)
    {
        return run_streaming_transport_benchmark(enc, blob_size, port, [](benchmark_stream_pair&) { return true; });
    }

    benchmark_result run_tls_tcp_blocking_benchmark(
        rpc::encoding enc,
        size_t blob_size,
        uint16_t port)
    {
#ifdef CANOPY_FULLSTACK_BENCHMARK_HAS_TLS
        return run_streaming_transport_benchmark(
            enc, blob_size, port, [](benchmark_stream_pair& pair) { return wrap_tls(pair); });
#else
        (void)enc;
        (void)blob_size;
        (void)port;
        return benchmark_result{rpc::error::NOT_IMPLEMENTED(), {}};
#endif
    }

    benchmark_result run_websocket_tcp_blocking_benchmark(
        rpc::encoding enc,
        size_t blob_size,
        uint16_t port)
    {
#ifdef CANOPY_BUILD_WEBSOCKET
        return run_streaming_transport_benchmark(
            enc, blob_size, port, [](benchmark_stream_pair& pair) { return wrap_websocket(pair); });
#else
        (void)enc;
        (void)blob_size;
        (void)port;
        return benchmark_result{rpc::error::NOT_IMPLEMENTED(), {}};
#endif
    }

    benchmark_result run_tls_websocket_tcp_blocking_benchmark(
        rpc::encoding enc,
        size_t blob_size,
        uint16_t port)
    {
#if defined(CANOPY_FULLSTACK_BENCHMARK_HAS_TLS) && defined(CANOPY_BUILD_WEBSOCKET)
        return run_streaming_transport_benchmark(
            enc, blob_size, port, [](benchmark_stream_pair& pair) { return wrap_tls(pair) && wrap_websocket(pair); });
#else
        (void)enc;
        (void)blob_size;
        (void)port;
        return benchmark_result{rpc::error::NOT_IMPLEMENTED(), {}};
#endif
    }
}
