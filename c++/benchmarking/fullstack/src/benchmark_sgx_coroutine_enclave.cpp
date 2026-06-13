/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_data_processor.h"

#include <comprehensive/comprehensive.h>
#include <io_uring/tcp.h>
#include <rpc/rpc.h>
#include <streaming/tcp_coroutine/stream.h>
#include <transports/sgx_coroutine/enclave/runtime.h>
#include <transports/streaming/transport.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace comprehensive::v1
{
    namespace
    {
        uint64_t now_ticks() noexcept
        {
            return rpc::sgx_coroutine_transport::enclave::read_runtime_tick_counter();
        }

        uint64_t ticks_to_nanoseconds(uint64_t ticks) noexcept
        {
            return rpc::sgx_coroutine_transport::enclave::runtime_ticks_to_nanoseconds(ticks);
        }

        std::vector<uint8_t> make_payload(uint64_t size)
        {
            std::vector<uint8_t> data(static_cast<size_t>(size));
            for (size_t index = 0; index < data.size(); ++index)
            {
                data[index] = static_cast<uint8_t>(index % 251U);
            }
            return data;
        }

        rpc::zone make_benchmark_zone(uint64_t subnet_offset)
        {
            auto zone = rpc::DEFAULT_PREFIX;
            [[maybe_unused]] const auto subnet_set = zone.set_subnet(zone.get_subnet() + subnet_offset);
            return rpc::zone{zone};
        }

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

        CORO_TASK(int)
        wait_for_transport_disconnect(
            const rpc::coro::scheduler_ptr& scheduler,
            const std::shared_ptr<rpc::stream_transport::transport>& transport)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
            while (transport && transport->get_status() < rpc::transport_status::DISCONNECTED)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    CO_RETURN rpc::error::CALL_TIMEOUT();
                }

                CO_AWAIT scheduler->schedule();
            }

            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(comprehensive_error)
        run_benchmark_calls_in_enclave(
            rpc::shared_ptr<i_data_processor> remote,
            const std::vector<uint8_t>& payload,
            uint32_t iterations,
            uint32_t warmup_iterations,
            std::vector<uint64_t>& durations_ns)
        {
            if (!remote || iterations == 0)
            {
                CO_RETURN comprehensive_error::INVALID_ARGUMENT;
            }

            durations_ns.clear();
            durations_ns.reserve(iterations);

            std::vector<uint8_t> response;
            for (uint32_t index = 0; index < warmup_iterations; ++index)
            {
                const auto error = CO_AWAIT remote->echo_binary(payload, response);
                if (error != rpc::error::OK())
                {
                    CO_RETURN error;
                }
            }

            for (uint32_t index = 0; index < iterations; ++index)
            {
                const auto start = now_ticks();
                const auto error = CO_AWAIT remote->echo_binary(payload, response);
                const auto end = now_ticks();

                if (error != rpc::error::OK())
                {
                    CO_RETURN error;
                }
                if (response.size() != payload.size())
                {
                    CO_RETURN comprehensive_error::INVALID_BENCHMARK_RESULT;
                }

                durations_ns.push_back(ticks_to_nanoseconds(end - start));
            }

            CO_RETURN rpc::error::OK();
        }

        struct benchmark_run_state
        {
            std::atomic<uint16_t> port{0};
            std::atomic<int> server_error{rpc::error::OK()};
            std::atomic<int> client_error{rpc::error::OK()};
            std::shared_ptr<rpc::io_uring::acceptor> acceptor;
            std::shared_ptr<rpc::event> server_done;
            std::shared_ptr<rpc::event> client_done;
        };

        struct server_session
        {
            std::shared_ptr<benchmark_run_state> state;
            std::shared_ptr<rpc::event> server_ready;
            std::shared_ptr<rpc::event> client_finished;
        };

        void signal_server_ready(
            const std::shared_ptr<benchmark_run_state>& state,
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

        CORO_TASK(void)
        close_server_acceptor(const std::shared_ptr<benchmark_run_state>& state)
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

        struct server_resources
        {
            std::shared_ptr<rpc::root_service> service;
            std::shared_ptr<rpc::event> shutdown_event;
            std::shared_ptr<rpc::io_uring::acceptor> acceptor;
            std::shared_ptr<rpc::stream_transport::transport> transport;

            CORO_TASK(int)
            close(
                const rpc::coro::scheduler_ptr& scheduler,
                const std::shared_ptr<benchmark_run_state>& state,
                int preferred_error)
            {
                transport.reset();

                if (acceptor)
                {
                    CO_AWAIT acceptor->close();
                }
                acceptor.reset();

                if (state)
                {
                    state->acceptor.reset();
                }

                service.reset();

                const auto shutdown_error = CO_AWAIT wait_for_shutdown_event(scheduler, shutdown_event);
                CO_RETURN preferred_error == rpc::error::OK() ? shutdown_error : preferred_error;
            }
        };

        struct client_resources
        {
            std::shared_ptr<rpc::root_service> service;
            std::shared_ptr<rpc::event> shutdown_event;
            std::shared_ptr<rpc::stream_transport::transport> transport;
            rpc::shared_ptr<i_data_processor> remote_processor;

            CORO_TASK(int)
            close(
                const rpc::coro::scheduler_ptr& scheduler,
                rpc::event& client_finished,
                int preferred_error)
            {
                const bool wait_for_clean_disconnect = preferred_error == rpc::error::OK() && remote_processor && transport;

                remote_processor = nullptr;

                if (wait_for_clean_disconnect)
                {
                    const auto disconnect_error = CO_AWAIT wait_for_transport_disconnect(scheduler, transport);
                    client_finished.set();
                    transport.reset();
                    service.reset();

                    const auto shutdown_error = CO_AWAIT wait_for_shutdown_event(scheduler, shutdown_event);
                    CO_RETURN disconnect_error == rpc::error::OK() ? shutdown_error : disconnect_error;
                }

                transport.reset();
                client_finished.set();
                service.reset();

                const auto shutdown_error = CO_AWAIT wait_for_shutdown_event(scheduler, shutdown_event);
                CO_RETURN preferred_error == rpc::error::OK() ? shutdown_error : preferred_error;
            }
        };

        CORO_TASK(int)
        run_server(
            const std::shared_ptr<rpc::io_uring::controller>& controller,
            rpc::coro::scheduler_ptr scheduler,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            rpc::encoding enc,
            rpc::io_uring::wait_strategy measured_wait_strategy,
            const std::shared_ptr<benchmark_run_state>& state)
        {
            if (!controller || !scheduler || !state)
            {
                signal_server_ready(state, server_ready, rpc::error::INVALID_DATA());
                CO_RETURN rpc::error::INVALID_DATA();
            }

            server_resources server;
            server.service
                = rpc::root_service::create("enclave_io_uring_benchmark_server", make_benchmark_zone(8192), scheduler);
            server.service->set_default_encoding(enc);
            server.shutdown_event = std::make_shared<rpc::event>(false);
            server.shutdown_event->set_scheduler(scheduler.get());
            server.service->set_shutdown_event(server.shutdown_event);
            server.acceptor = std::make_shared<rpc::io_uring::acceptor>(controller);

            uint16_t selected_port = 0;
            int listen_error = rpc::error::TRANSPORT_ERROR();
            for (uint16_t candidate_port = 26000; candidate_port < 27000; ++candidate_port)
            {
                listen_error = CO_AWAIT server.acceptor->listen_loopback(candidate_port);
                if (listen_error == rpc::error::OK())
                {
                    selected_port = candidate_port;
                    break;
                }
            }

            if (selected_port == 0)
            {
                signal_server_ready(state, server_ready, listen_error);
                CO_RETURN CO_AWAIT server.close(scheduler, state, listen_error);
            }

            state->acceptor = server.acceptor;
            signal_server_ready(state, server_ready, rpc::error::OK(), selected_port);

            auto accept_result = streaming::coroutine::tcp::make_stream_result(
                CO_AWAIT server.acceptor->accept_with_result(), selected_port);
            if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
            {
                const auto error_code = accept_result.error_code != rpc::error::OK() ? accept_result.error_code
                                                                                     : rpc::error::PROTOCOL_ERROR();
                CO_RETURN CO_AWAIT server.close(scheduler, state, error_code);
            }

            server.transport = rpc::stream_transport::create<i_data_processor, i_data_processor>(
                "enclave_io_uring_benchmark_server_transport",
                server.service,
                accept_result.connection,
                [](rpc::shared_ptr<i_data_processor>,
                    std::shared_ptr<rpc::service>) -> CORO_TASK(rpc::service_connect_result<i_data_processor>)
                {
                    CO_RETURN rpc::service_connect_result<i_data_processor>{
                        rpc::error::OK(), make_benchmark_data_processor()};
                });
            accept_result.connection.reset();

            if (!server.transport)
            {
                CO_RETURN CO_AWAIT server.close(scheduler, state, rpc::error::OUT_OF_MEMORY());
            }

            controller->set_wait_strategy(measured_wait_strategy);
            CO_AWAIT client_finished.wait();
            CO_RETURN CO_AWAIT server.close(scheduler, state, rpc::error::OK());
        }

        CORO_TASK(int)
        run_client(
            const std::shared_ptr<rpc::io_uring::controller>& controller,
            rpc::coro::scheduler_ptr scheduler,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            rpc::encoding enc,
            uint64_t blob_size,
            uint32_t iterations,
            uint32_t warmup_iterations,
            rpc::io_uring::wait_strategy measured_wait_strategy,
            const std::shared_ptr<benchmark_run_state>& state,
            std::vector<uint64_t>& durations_ns)
        {
            if (!controller || !scheduler || !state)
            {
                client_finished.set();
                CO_RETURN rpc::error::INVALID_DATA();
            }

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
            client.service
                = rpc::root_service::create("enclave_io_uring_benchmark_client", make_benchmark_zone(8193), scheduler);
            client.service->set_default_encoding(enc);
            client.shutdown_event = std::make_shared<rpc::event>(false);
            client.shutdown_event->set_scheduler(scheduler.get());
            client.service->set_shutdown_event(client.shutdown_event);

            rpc::io_uring::connector connector(controller);
            auto connect_result = streaming::coroutine::tcp::make_stream_result(
                CO_AWAIT connector.connect_loopback_with_result(port), port);
            if (connect_result.error_code != rpc::error::OK() || !connect_result.connection)
            {
                err = connect_result.error_code != rpc::error::OK() ? connect_result.error_code
                                                                    : rpc::error::PROTOCOL_ERROR();
                CO_RETURN CO_AWAIT client.close(scheduler, client_finished, err);
            }

            client.transport = rpc::stream_transport::make_client(
                "enclave_io_uring_benchmark_client_transport", client.service, connect_result.connection);
            connect_result.connection.reset();
            if (!client.transport)
            {
                CO_RETURN CO_AWAIT client.close(scheduler, client_finished, rpc::error::OUT_OF_MEMORY());
            }

            rpc::shared_ptr<i_data_processor> not_used;
            auto zone_result = CO_AWAIT client.service->connect_to_zone<i_data_processor, i_data_processor>(
                "enclave_io_uring_benchmark_server", client.transport, not_used);
            client.remote_processor = zone_result.output_interface;
            zone_result.output_interface = nullptr;
            not_used = nullptr;

            if (zone_result.error_code != rpc::error::OK() || !client.remote_processor)
            {
                err = zone_result.error_code != rpc::error::OK() ? zone_result.error_code : rpc::error::PROTOCOL_ERROR();
                CO_RETURN CO_AWAIT client.close(scheduler, client_finished, err);
            }

            controller->set_wait_strategy(measured_wait_strategy);
            const auto payload = make_payload(blob_size);
            err = CO_AWAIT run_benchmark_calls_in_enclave(
                client.remote_processor, payload, iterations, warmup_iterations, durations_ns);

            CO_RETURN CO_AWAIT client.close(scheduler, client_finished, err);
        }

        CORO_TASK(int)
        run_client_to_port(
            const std::shared_ptr<rpc::io_uring::controller>& controller,
            rpc::coro::scheduler_ptr scheduler,
            rpc::encoding enc,
            uint64_t blob_size,
            uint32_t iterations,
            uint32_t warmup_iterations,
            rpc::io_uring::wait_strategy measured_wait_strategy,
            uint16_t port,
            std::vector<uint64_t>& durations_ns)
        {
            if (!controller || !scheduler || port == 0)
            {
                CO_RETURN rpc::error::INVALID_DATA();
            }

            rpc::event client_finished;
            client_finished.set_scheduler(scheduler.get());

            client_resources client;
            client.service
                = rpc::root_service::create("enclave_io_uring_benchmark_client", make_benchmark_zone(8193), scheduler);
            client.service->set_default_encoding(enc);
            client.shutdown_event = std::make_shared<rpc::event>(false);
            client.shutdown_event->set_scheduler(scheduler.get());
            client.service->set_shutdown_event(client.shutdown_event);

            controller->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

            rpc::io_uring::connector connector(controller);
            auto connect_result = streaming::coroutine::tcp::make_stream_result(
                CO_AWAIT connector.connect_loopback_with_result(port), port);
            if (connect_result.error_code != rpc::error::OK() || !connect_result.connection)
            {
                const auto err = connect_result.error_code != rpc::error::OK() ? connect_result.error_code
                                                                               : rpc::error::PROTOCOL_ERROR();
                CO_RETURN CO_AWAIT client.close(scheduler, client_finished, err);
            }

            client.transport = rpc::stream_transport::make_client(
                "enclave_io_uring_benchmark_client_transport", client.service, connect_result.connection);
            connect_result.connection.reset();
            if (!client.transport)
            {
                CO_RETURN CO_AWAIT client.close(scheduler, client_finished, rpc::error::OUT_OF_MEMORY());
            }

            rpc::shared_ptr<i_data_processor> not_used;
            auto zone_result = CO_AWAIT client.service->connect_to_zone<i_data_processor, i_data_processor>(
                "enclave_io_uring_benchmark_server", client.transport, not_used);
            client.remote_processor = zone_result.output_interface;
            zone_result.output_interface = nullptr;
            not_used = nullptr;

            if (zone_result.error_code != rpc::error::OK() || !client.remote_processor)
            {
                const auto err = zone_result.error_code != rpc::error::OK() ? zone_result.error_code
                                                                            : rpc::error::PROTOCOL_ERROR();
                CO_RETURN CO_AWAIT client.close(scheduler, client_finished, err);
            }

            controller->set_wait_strategy(measured_wait_strategy);
            const auto payload = make_payload(blob_size);
            const auto err = CO_AWAIT run_benchmark_calls_in_enclave(
                client.remote_processor, payload, iterations, warmup_iterations, durations_ns);

            CO_RETURN CO_AWAIT client.close(scheduler, client_finished, err);
        }

        CORO_TASK(void)
        run_server_and_signal(
            const std::shared_ptr<rpc::io_uring::controller>& controller,
            rpc::coro::scheduler_ptr scheduler,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            rpc::encoding enc,
            rpc::io_uring::wait_strategy measured_wait_strategy,
            const std::shared_ptr<benchmark_run_state>& state)
        {
            auto err = CO_AWAIT run_server(
                controller, scheduler, server_ready, client_finished, enc, measured_wait_strategy, state);
            state->server_error.store(err, std::memory_order_release);
            state->server_done->set();
            CO_RETURN;
        }

        CORO_TASK(void)
        run_client_and_signal(
            const std::shared_ptr<rpc::io_uring::controller>& controller,
            rpc::coro::scheduler_ptr scheduler,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            rpc::encoding enc,
            uint64_t blob_size,
            uint32_t iterations,
            uint32_t warmup_iterations,
            rpc::io_uring::wait_strategy measured_wait_strategy,
            const std::shared_ptr<benchmark_run_state>& state,
            std::vector<uint64_t>& durations_ns)
        {
            auto err = CO_AWAIT run_client(
                controller,
                scheduler,
                server_ready,
                client_finished,
                enc,
                blob_size,
                iterations,
                warmup_iterations,
                measured_wait_strategy,
                state,
                durations_ns);
            if (err != rpc::error::OK())
            {
                CO_AWAIT close_server_acceptor(state);
            }
            state->client_error.store(err, std::memory_order_release);
            state->client_done->set();
            CO_RETURN;
        }

        class enclave_io_uring_benchmark : public rpc::base<enclave_io_uring_benchmark, i_enclave_io_uring_benchmark>
        {
        public:
            enclave_io_uring_benchmark(
                std::shared_ptr<rpc::io_uring::controller> controller,
                std::shared_ptr<rpc::service> service) noexcept
                : controller_(std::move(controller))
                , service_(std::move(service))
            {
            }

            ~enclave_io_uring_benchmark()
            {
                if (server_session_ && server_session_->client_finished)
                {
                    server_session_->client_finished->set();
                }
            }

            CORO_TASK(comprehensive_error)
            run_io_uring_rpc(
                uint64_t encoding,
                uint64_t blob_size,
                uint32_t iterations,
                uint32_t warmup_iterations,
                bool use_proactor,
                std::vector<uint64_t>& durations_ns) override
            {
                durations_ns.clear();

                if (!controller_ || !service_ || !service_->get_scheduler() || iterations == 0)
                {
                    CO_RETURN comprehensive_error::INVALID_ARGUMENT;
                }

                controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

                rpc::event server_ready;
                rpc::event client_finished;
                auto scheduler = service_->get_scheduler();
                server_ready.set_scheduler(scheduler.get());
                client_finished.set_scheduler(scheduler.get());

                auto state = std::make_shared<benchmark_run_state>();
                state->server_done = std::make_shared<rpc::event>(false);
                state->client_done = std::make_shared<rpc::event>(false);
                state->server_done->set_scheduler(scheduler.get());
                state->client_done->set_scheduler(scheduler.get());

                const auto enc = static_cast<rpc::encoding>(encoding);
                const auto measured_wait_strategy = use_proactor ? rpc::io_uring::wait_strategy::proactor
                                                                 : rpc::io_uring::wait_strategy::cooperative_poll;

                if (!service_->spawn(run_server_and_signal(
                        controller_, scheduler, server_ready, client_finished, enc, measured_wait_strategy, state)))
                {
                    CO_RETURN rpc::error::TRANSPORT_ERROR();
                }

                if (!service_->spawn(run_client_and_signal(
                        controller_,
                        scheduler,
                        server_ready,
                        client_finished,
                        enc,
                        blob_size,
                        iterations,
                        warmup_iterations,
                        measured_wait_strategy,
                        state,
                        durations_ns)))
                {
                    CO_AWAIT server_ready.wait();
                    CO_AWAIT close_server_acceptor(state);
                    client_finished.set();
                    CO_AWAIT state->server_done->wait();
                    CO_RETURN rpc::error::TRANSPORT_ERROR();
                }

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

            CORO_TASK(comprehensive_error)
            start_io_uring_rpc_server(
                uint64_t encoding,
                bool use_proactor,
                uint32_t& port) override
            {
                port = 0;

                if (!controller_ || !service_ || !service_->get_scheduler())
                {
                    CO_RETURN comprehensive_error::SERVICE_UNAVAILABLE;
                }
                if (server_session_ && server_session_->state && server_session_->state->server_done
                    && !server_session_->state->server_done->is_set())
                {
                    CO_RETURN comprehensive_error::SERVER_ALREADY_RUNNING;
                }

                auto scheduler = service_->get_scheduler();
                auto session = std::make_shared<server_session>();
                session->state = std::make_shared<benchmark_run_state>();
                session->server_ready = std::make_shared<rpc::event>(false);
                session->client_finished = std::make_shared<rpc::event>(false);
                session->state->server_done = std::make_shared<rpc::event>(false);
                session->state->client_done = std::make_shared<rpc::event>(false);
                session->server_ready->set_scheduler(scheduler.get());
                session->client_finished->set_scheduler(scheduler.get());
                session->state->server_done->set_scheduler(scheduler.get());
                session->state->client_done->set_scheduler(scheduler.get());

                controller_->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

                const auto enc = static_cast<rpc::encoding>(encoding);
                const auto measured_wait_strategy = use_proactor ? rpc::io_uring::wait_strategy::proactor
                                                                 : rpc::io_uring::wait_strategy::cooperative_poll;

                if (!service_->spawn(run_server_and_signal(
                        controller_,
                        scheduler,
                        *session->server_ready,
                        *session->client_finished,
                        enc,
                        measured_wait_strategy,
                        session->state)))
                {
                    CO_RETURN rpc::error::TRANSPORT_ERROR();
                }

                CO_AWAIT session->server_ready->wait();

                const auto error = session->state->server_error.load(std::memory_order_acquire);
                if (error != rpc::error::OK())
                {
                    session->client_finished->set();
                    CO_AWAIT session->state->server_done->wait();
                    CO_RETURN error;
                }

                const auto selected_port = session->state->port.load(std::memory_order_acquire);
                if (selected_port == 0)
                {
                    session->client_finished->set();
                    CO_AWAIT session->state->server_done->wait();
                    CO_RETURN comprehensive_error::INVALID_BENCHMARK_RESULT;
                }

                port = selected_port;
                server_session_ = std::move(session);
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(comprehensive_error)
            run_io_uring_rpc_client(
                uint64_t encoding,
                uint64_t blob_size,
                uint32_t iterations,
                uint32_t warmup_iterations,
                bool use_proactor,
                uint32_t port,
                std::vector<uint64_t>& durations_ns) override
            {
                durations_ns.clear();

                if (!controller_ || !service_ || !service_->get_scheduler() || iterations == 0 || port == 0 || port > 65535)
                {
                    CO_RETURN comprehensive_error::INVALID_ARGUMENT;
                }

                const auto enc = static_cast<rpc::encoding>(encoding);
                const auto measured_wait_strategy = use_proactor ? rpc::io_uring::wait_strategy::proactor
                                                                 : rpc::io_uring::wait_strategy::cooperative_poll;

                CO_RETURN CO_AWAIT run_client_to_port(
                    controller_,
                    service_->get_scheduler(),
                    enc,
                    blob_size,
                    iterations,
                    warmup_iterations,
                    measured_wait_strategy,
                    static_cast<uint16_t>(port),
                    durations_ns);
            }

            CORO_TASK(comprehensive_error) stop_io_uring_rpc_server() override
            {
                if (!server_session_)
                {
                    CO_RETURN rpc::error::OK();
                }

                auto session = std::move(server_session_);
                CO_AWAIT close_server_acceptor(session->state);
                session->client_finished->set();
                CO_AWAIT session->state->server_done->wait();
                CO_RETURN session->state->server_error.load(std::memory_order_acquire);
            }

        private:
            std::shared_ptr<rpc::io_uring::controller> controller_;
            std::shared_ptr<rpc::service> service_;
            std::shared_ptr<server_session> server_session_;
        };

        struct enclave_entry_point
        {
            enclave_entry_point()
            {
                rpc::sgx_coroutine_transport::enclave::register_connection_factory<rpc::i_noop, i_enclave_io_uring_benchmark>(
                    "benchmark_sgx_coroutine_enclave",
                    [](rpc::shared_ptr<rpc::i_noop>, std::shared_ptr<rpc::service> service)
                        -> CORO_TASK(rpc::service_connect_result<i_enclave_io_uring_benchmark>)
                    {
                        if (!service)
                        {
                            CO_RETURN rpc::service_connect_result<i_enclave_io_uring_benchmark>{
                                rpc::error::INVALID_DATA(), {}};
                        }

                        auto enclave_service = std::dynamic_pointer_cast<rpc::enclave_service>(service);
                        if (!enclave_service)
                        {
                            CO_RETURN rpc::service_connect_result<i_enclave_io_uring_benchmark>{
                                rpc::error::INVALID_CAST(), {}};
                        }

                        auto controller = enclave_service->get_io_uring_controller();
                        if (!controller)
                        {
                            CO_RETURN rpc::service_connect_result<i_enclave_io_uring_benchmark>{
                                rpc::error::INCOMPATIBLE_SERVICE(), {}};
                        }

                        rpc::shared_ptr<i_enclave_io_uring_benchmark> benchmark(
                            new enclave_io_uring_benchmark(std::move(controller), std::move(service)));
                        CO_RETURN rpc::service_connect_result<i_enclave_io_uring_benchmark>{
                            rpc::error::OK(), std::move(benchmark)};
                    });
            }
        };

        enclave_entry_point g_enclave_entry_point;
    } // namespace
} // namespace comprehensive::v1
