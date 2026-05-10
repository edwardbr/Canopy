/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "end_2_end_setup.h"

#include <io_uring/tcp.h>
#include <streaming/io_uring/stream.h>
#include <transports/streaming/transport.h>

#include <atomic>
#include <chrono>
#include <memory>

namespace io_uring_test_enclave
{
    namespace
    {
        // Endpoint implementations add these constants into their replies.
        // The final response value proves both call directions were exercised:
        // client -> server and server -> client callback.
        constexpr uint32_t client_peer_bias = 0x1000U;
        constexpr uint32_t server_peer_bias = 0x2000U;

        // Build deterministic but distinct zones for the in-enclave client and
        // server root services. They share one scheduler and one io_uring
        // controller, but the RPC stacks should otherwise be independent.
        rpc::zone make_peer_zone(uint64_t subnet_offset)
        {
            auto zone = rpc::DEFAULT_PREFIX;
            [[maybe_unused]] const auto subnet_set = zone.set_subnet(zone.get_subnet() + subnet_offset);
            return rpc::zone{zone};
        }

        // Tests provide a small endpoint factory: given the service and the
        // optional remote peer endpoint, return the local i_peer2peer object.
        // The streaming transport needs a service_connect_result, so this keeps
        // that wrapping noise away from the concrete test endpoint code.
        CORO_TASK(rpc::service_connect_result<io_uring_test::i_peer2peer>)
        make_endpoint_result(
            peer2peer_endpoint_factory factory,
            std::shared_ptr<rpc::service> service,
            rpc::shared_ptr<io_uring_test::i_peer2peer> remote)
        {
            auto endpoint = factory(service, remote);
            const auto err = endpoint ? rpc::error::OK() : rpc::error::INCOMPATIBLE_SERVICE();
            CO_RETURN rpc::service_connect_result<io_uring_test::i_peer2peer>{err, endpoint};
        }

        // Adapter used by create(). During connect_to_zone the client
        // supplies its local endpoint as the server's remote_client. The server
        // endpoint factory can capture that remote endpoint for callbacks.
        struct server_connection_factory
        {
            peer2peer_endpoint_factory factory{nullptr};

            CORO_TASK(rpc::service_connect_result<io_uring_test::i_peer2peer>)
            operator()(
                rpc::shared_ptr<io_uring_test::i_peer2peer> remote_client,
                std::shared_ptr<rpc::service> service) const
            {
                CO_RETURN CO_AWAIT make_endpoint_result(factory, service, remote_client);
            }
        };

        // Shared state between the detached server and client coroutines.
        // Atomics are used because the SGX coroutine scheduler may run these
        // coroutines on different worker threads.
        struct peer2peer_run_state
        {
            std::atomic<uint16_t> port{0};
            std::atomic<int> server_error{rpc::error::OK()};
            std::atomic<int> client_error{rpc::error::OK()};

            // Cross-side cancellation handle. If the client fails before a
            // connection is accepted, the wrapper closes this acceptor to
            // unblock the server coroutine.
            std::shared_ptr<rpc::io_uring::acceptor> acceptor;

            // Completion events for the detached wrappers. The parent waits on
            // these before reading final errors.
            std::shared_ptr<rpc::event> server_done;
            std::shared_ptr<rpc::event> client_done;
        };

        // Publish the server's listen result before releasing the client. On
        // success this also publishes the selected loopback port.
        void signal_server_ready(
            const std::shared_ptr<peer2peer_run_state>& state,
            rpc::event& server_ready,
            int error_code,
            uint16_t port = 0)
        {
            if (state)
            {
                state->server_error.store(error_code, std::memory_order_release);
                if (port != 0)
                {
                    state->port.store(port, std::memory_order_release);
                }
            }

            server_ready.set();
        }

        // Service shutdown can be asynchronous because transports and service
        // proxies release one another through the scheduler. Waiting on the
        // service's shutdown event avoids assuming that reset() means fully
        // drained.
        CORO_TASK(int)
        wait_for_shutdown_event(
            const rpc::coro::scheduler_ptr& scheduler,
            const std::shared_ptr<rpc::event>& shutdown_event)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
            while (shutdown_event && !shutdown_event->is_set())
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    CO_RETURN rpc::error::CALL_TIMEOUT();
                }

                CO_AWAIT scheduler->schedule();
            }

            CO_RETURN rpc::error::OK();
        }

        // Awaitable cleanup bundle for the server side. Destructors cannot
        // co_await, so close() centralizes the order needed for graceful test
        // shutdown.
        struct server_resources
        {
            std::shared_ptr<rpc::root_service> service;
            std::shared_ptr<rpc::event> shutdown_event;
            std::shared_ptr<rpc::io_uring::acceptor> acceptor;
            std::shared_ptr<rpc::stream_transport::transport> transport;

            CORO_TASK(int)
            close(
                const rpc::coro::scheduler_ptr& scheduler,
                const std::shared_ptr<peer2peer_run_state>& state,
                int preferred_error)
            {
                // Drop the transport first. It owns the stream and participates
                // in service-proxy lifetime cleanup.
                transport.reset();

                // The acceptor owns the listening direct descriptor. Closing it
                // may submit async cleanup through the shared io_uring controller.
                if (acceptor)
                {
                    CO_AWAIT acceptor->close();
                }
                acceptor.reset();

                // Clear the shared cancellation handle once the descriptor is
                // closed, so later failure handling cannot close it again.
                if (state)
                {
                    state->acceptor.reset();
                }

                // Releasing the service starts the service/transport shutdown
                // sequence. The event below confirms that it has drained.
                service.reset();

                const auto shutdown_error = CO_AWAIT wait_for_shutdown_event(scheduler, shutdown_event);
                CO_RETURN preferred_error == rpc::error::OK() ? shutdown_error : preferred_error;
            }
        };

        // Awaitable cleanup bundle for the client side. It owns the client root
        // service, the stream transport, and both RPC pointers used by the test.
        struct client_resources
        {
            std::shared_ptr<rpc::root_service> service;
            std::shared_ptr<rpc::event> shutdown_event;
            std::shared_ptr<rpc::stream_transport::transport> transport;
            rpc::shared_ptr<io_uring_test::i_peer2peer> local_endpoint;
            rpc::shared_ptr<io_uring_test::i_peer2peer> remote_server;

            CORO_TASK(int)
            close(
                const rpc::coro::scheduler_ptr& scheduler,
                rpc::event& client_finished,
                int preferred_error)
            {
                // Release RPC pointers before dropping the transport/service so
                // any remote release traffic can still flow over a live stack.
                remote_server = nullptr;
                local_endpoint = nullptr;
                transport.reset();

                // Tell the server that the client will not issue more calls.
                // The server waits for this before closing its transport.
                client_finished.set();
                service.reset();

                const auto shutdown_error = CO_AWAIT wait_for_shutdown_event(scheduler, shutdown_event);
                CO_RETURN preferred_error == rpc::error::OK() ? shutdown_error : preferred_error;
            }
        };

        // Cross-side unblock used by the client wrapper when the client fails
        // before the server has accepted a connection. The client flow itself
        // does not own server cleanup policy.
        CORO_TASK(void)
        close_server_acceptor(const std::shared_ptr<peer2peer_run_state>& state)
        {
            auto acceptor = state ? state->acceptor : nullptr;
            if (acceptor)
            {
                CO_AWAIT acceptor->close();
            }

            if (state)
            {
                state->acceptor.reset();
            }

            CO_RETURN;
        }

        // Server half of the harness. It creates a root service, listens on an
        // io_uring TCP acceptor, accepts one connection, and turns that accepted
        // stream into a streaming RPC server transport.
        CORO_TASK(int)
        run_tcp_server(
            const std::shared_ptr<rpc::io_uring::controller>& controller,
            rpc::coro::scheduler_ptr scheduler,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            rpc::zone server_zone,
            const std::shared_ptr<peer2peer_run_state>& state,
            peer2peer_endpoint_factory server_factory)
        {
            if (!controller || !scheduler || !state || !server_factory)
            {
                signal_server_ready(state, server_ready, rpc::error::INVALID_DATA());
                CO_RETURN rpc::error::INVALID_DATA();
            }

            server_resources server;

            // This root service is independent from the client root service
            // created below. Sharing the scheduler/controller is deliberate; it
            // proves multiple zones in one enclave can use the same io_uring.
            server.service = rpc::root_service::create("io_uring_peer_server", server_zone, scheduler);
            server.shutdown_event = std::make_shared<rpc::event>(false);
            server.shutdown_event->set_scheduler(scheduler.get());
            server.service->set_shutdown_event(server.shutdown_event);
            server.acceptor = std::make_shared<rpc::io_uring::acceptor>(controller);

            // Try a small test-only range rather than one fixed port. This keeps
            // the test robust when an old failed run or another local process
            // has briefly occupied the first candidate port.
            uint16_t port = 0;
            int listen_error = rpc::error::TRANSPORT_ERROR();
            for (uint16_t candidate_port = 25344; candidate_port < 25380; ++candidate_port)
            {
                listen_error = CO_AWAIT server.acceptor->listen_loopback(candidate_port);
                if (listen_error == rpc::error::OK())
                {
                    port = candidate_port;
                    break;
                }
            }

            if (port == 0)
            {
                signal_server_ready(state, server_ready, listen_error);
                CO_RETURN CO_AWAIT server.close(scheduler, state, listen_error);
            }

            state->acceptor = server.acceptor;
            signal_server_ready(state, server_ready, rpc::error::OK(), port);

            // This smoke harness accepts exactly one connection. Future tests
            // can reuse end_2_end_setup and vary endpoint behavior without
            // copying the transport bootstrap code.
            auto accept_result
                = streaming::io_uring::make_stream_result(CO_AWAIT server.acceptor->accept_with_result(), port);
            if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
            {
                const auto error_code = accept_result.error_code != rpc::error::OK() ? accept_result.error_code
                                                                                     : rpc::error::PROTOCOL_ERROR();
                CO_RETURN CO_AWAIT server.close(scheduler, state, error_code);
            }

            // Build the server-side transport. The connection factory receives
            // the client endpoint from the connect_to_zone handshake and creates
            // the server's local endpoint.
            server.transport = rpc::stream_transport::create<io_uring_test::i_peer2peer, io_uring_test::i_peer2peer>(
                "io_uring_peer_server_transport",
                server.service,
                accept_result.connection,
                server_connection_factory{server_factory});

            // This file avoids std::move for readability. Clear the copied
            // result-held stream explicitly so it cannot keep the transport or
            // service graph alive during shutdown.
            accept_result.connection.reset();

            if (!server.transport)
            {
                CO_RETURN CO_AWAIT server.close(scheduler, state, rpc::error::OUT_OF_MEMORY());
            }

            // Keep the server transport alive while the client performs the
            // actual test calls. The client decides when the exchange is done.
            CO_AWAIT client_finished.wait();
            CO_RETURN CO_AWAIT server.close(scheduler, state, rpc::error::OK());
        }

        // Client half of the harness. It waits for the server port, connects an
        // io_uring TCP descriptor, adapts it into a generic stream, starts a
        // streaming RPC client transport, and runs the peer_ping verification loop.
        CORO_TASK(int)
        run_tcp_client(
            const std::shared_ptr<rpc::io_uring::controller>& controller,
            rpc::coro::scheduler_ptr scheduler,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            rpc::zone client_zone,
            const std::shared_ptr<peer2peer_run_state>& state,
            uint32_t iterations,
            peer2peer_endpoint_factory client_factory)
        {
            if (!controller || !scheduler || !state || !client_factory)
            {
                client_finished.set();
                CO_RETURN rpc::error::INVALID_DATA();
            }

            // Do not attempt the TCP connect until the server has either
            // successfully listened or published a listen failure.
            CO_AWAIT server_ready.wait();

            auto err = state->server_error.load(std::memory_order_acquire);
            if (err != rpc::error::OK())
            {
                client_finished.set();
                CO_RETURN err;
            }

            const auto port = state->port.load(std::memory_order_acquire);
            if (port == 0)
            {
                client_finished.set();
                CO_RETURN rpc::error::INVALID_DATA();
            }

            client_resources client;

            // Second independent root service in the same enclave. It uses the
            // same scheduler and io_uring controller as the server, but owns its
            // own zone and transport graph.
            client.service = rpc::root_service::create("io_uring_peer_client", client_zone, scheduler);
            client.shutdown_event = std::make_shared<rpc::event>(false);
            client.shutdown_event->set_scheduler(scheduler.get());
            client.service->set_shutdown_event(client.shutdown_event);

            // The connector returns a direct descriptor. The streaming adapter
            // below gives the RPC transport a generic byte stream without
            // putting TCP knowledge into the streaming layer.
            rpc::io_uring::connector connector(controller);
            auto connect_result
                = streaming::io_uring::make_stream_result(CO_AWAIT connector.connect_loopback_with_result(port), port);
            if (connect_result.error_code != rpc::error::OK() || !connect_result.connection)
            {
                err = connect_result.error_code != rpc::error::OK() ? connect_result.error_code
                                                                    : rpc::error::PROTOCOL_ERROR();
                CO_RETURN CO_AWAIT client.close(scheduler, client_finished, err);
            }

            client.transport = rpc::stream_transport::make_client(
                "io_uring_peer_client_transport", client.service, connect_result.connection);

            // As above, clear the copied result-held stream explicitly because
            // we are not using std::move in this file.
            connect_result.connection.reset();
            if (!client.transport)
            {
                CO_RETURN CO_AWAIT client.close(scheduler, client_finished, rpc::error::OUT_OF_MEMORY());
            }

            // The client endpoint is the object offered to the server during the
            // connection handshake, so it has no remote peer yet.
            client.local_endpoint = client_factory(client.service, {});
            if (!client.local_endpoint)
            {
                CO_RETURN CO_AWAIT client.close(scheduler, client_finished, rpc::error::INCOMPATIBLE_SERVICE());
            }

            auto zone_result
                = CO_AWAIT client.service->connect_to_zone<io_uring_test::i_peer2peer, io_uring_test::i_peer2peer>(
                    "io_uring_peer_server", client.transport, client.local_endpoint);
            if (zone_result.error_code != rpc::error::OK() || !zone_result.output_interface)
            {
                err = zone_result.error_code != rpc::error::OK() ? zone_result.error_code : rpc::error::PROTOCOL_ERROR();
                CO_RETURN CO_AWAIT client.close(scheduler, client_finished, err);
            }

            // connect_to_zone returns the server endpoint. From here onward the
            // test is exercising normal generated RPC over the io_uring stream.
            client.remote_server = zone_result.output_interface;

            // Clear the copied result pointer before shutdown. Otherwise this
            // local result object can extend the proxy lifetime and delay the
            // service shutdown event.
            zone_result.output_interface = nullptr;
            for (uint32_t iteration = 0; iteration < iterations; ++iteration)
            {
                const auto value = 100U + iteration;
                uint32_t response = 0;
                err = CO_AWAIT client.remote_server->peer_ping(value, response);
                if (err != rpc::error::OK())
                {
                    break;
                }

                // The expected value proves both directions: the direct
                // client->server call contributes server_peer_bias and the
                // server->client callback contributes client_peer_bias.
                const auto expected = (value + server_peer_bias) + (value + 1U + client_peer_bias);
                if (response != expected)
                {
                    RPC_WARNING(
                        "peer-to-peer RPC response mismatch iteration={} value={} response={} expected={}",
                        iteration,
                        value,
                        response,
                        expected);
                    err = rpc::error::INVALID_DATA();
                    break;
                }
            }

            CO_RETURN CO_AWAIT client.close(scheduler, client_finished, err);
        }

        // Detached scheduler tasks do not return a value to the spawner. This
        // wrapper stores the server result and signals completion through
        // peer2peer_run_state.
        CORO_TASK(void)
        run_tcp_server_and_signal(
            const std::shared_ptr<rpc::io_uring::controller>& controller,
            rpc::coro::scheduler_ptr scheduler,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            rpc::zone server_zone,
            const std::shared_ptr<peer2peer_run_state>& state,
            peer2peer_endpoint_factory server_factory)
        {
            auto err = CO_AWAIT run_tcp_server(
                controller, scheduler, server_ready, client_finished, server_zone, state, server_factory);
            state->server_error.store(err, std::memory_order_release);
            state->server_done->set();
            CO_RETURN;
        }

        // Client wrapper mirrors the server wrapper, with one extra
        // responsibility: if the client fails before the server accepts, close
        // the acceptor to unblock the server coroutine.
        CORO_TASK(void)
        run_tcp_client_and_signal(
            const std::shared_ptr<rpc::io_uring::controller>& controller,
            rpc::coro::scheduler_ptr scheduler,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            rpc::zone client_zone,
            const std::shared_ptr<peer2peer_run_state>& state,
            uint32_t iterations,
            peer2peer_endpoint_factory client_factory)
        {
            auto err = CO_AWAIT run_tcp_client(
                controller, scheduler, server_ready, client_finished, client_zone, state, iterations, client_factory);
            if (err != rpc::error::OK())
            {
                CO_AWAIT close_server_acceptor(state);
            }
            state->client_error.store(err, std::memory_order_release);
            state->client_done->set();
            CO_RETURN;
        }
    } // namespace

    // Shared end-to-end harness for peer-to-peer tests. It owns the common
    // lifecycle: create two independent root services, connect them over an
    // io_uring TCP stream, run the client test loop, and wait for both sides to
    // tear down cleanly. Tests only provide the concrete i_peer2peer endpoint
    // factories.
    CORO_TASK(int)
    end_2_end_setup(
        const std::shared_ptr<rpc::io_uring::controller>& controller,
        const std::shared_ptr<rpc::service>& child_service,
        uint32_t iterations,
        peer2peer_endpoint_factory client_factory,
        peer2peer_endpoint_factory server_factory)
    {
        if (!child_service || !child_service->get_scheduler() || !controller || !client_factory || !server_factory)
        {
            CO_RETURN rpc::error::INVALID_DATA();
        }
        if (iterations == 0 || iterations > 1024)
        {
            CO_RETURN rpc::error::INVALID_DATA();
        }

        // This test validates the peer-to-peer streaming RPC path rather than
        // comparing wait strategies. Keep it on the proactor path used by the
        // current peer smoke test.
        controller->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

        rpc::event server_ready;
        rpc::event client_finished;
        auto scheduler = child_service->get_scheduler();

        // Events resume through the SGX scheduler instead of directly on the
        // setter's stack. That matches the rest of the enclave coroutine model.
        server_ready.set_scheduler(scheduler.get());
        client_finished.set_scheduler(scheduler.get());

        auto state = std::make_shared<peer2peer_run_state>();
        state->server_done = std::make_shared<rpc::event>(false);
        state->client_done = std::make_shared<rpc::event>(false);
        state->server_done->set_scheduler(scheduler.get());
        state->client_done->set_scheduler(scheduler.get());

        // Start the listener first. It signals server_ready only after bind and
        // listen have succeeded, so the client can safely read the selected port.
        if (!child_service->spawn(run_tcp_server_and_signal(
                controller, scheduler, server_ready, client_finished, make_peer_zone(4096), state, server_factory)))
        {
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Start the client independently so run_tcp_client and run_tcp_server
        // remain readable as isolated endpoint flows. This mirrors the
        // comprehensive TCP demo without relying on libcoro when_all in SGX.
        if (!child_service->spawn(run_tcp_client_and_signal(
                controller, scheduler, server_ready, client_finished, make_peer_zone(4097), state, iterations, client_factory)))
        {
            // If the client task cannot be spawned, the server may already be
            // blocked in accept. Wait until the acceptor is published, close it,
            // and then wait for the server wrapper to finish.
            CO_AWAIT server_ready.wait();
            CO_AWAIT close_server_acceptor(state);
            client_finished.set();
            CO_AWAIT state->server_done->wait();
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        // Both detached wrappers must have returned before inspecting final
        // errors. This also guarantees that the cleanup each side awaited has
        // completed.
        CO_AWAIT state->client_done->wait();
        CO_AWAIT state->server_done->wait();

        const auto client_error = state->client_error.load(std::memory_order_acquire);
        const auto server_error = state->server_error.load(std::memory_order_acquire);
        if (client_error != rpc::error::OK())
        {
            CO_RETURN client_error;
        }
        if (server_error != rpc::error::OK())
        {
            CO_RETURN server_error;
        }

        CO_RETURN rpc::error::OK();
    }
} // namespace io_uring_test_enclave
