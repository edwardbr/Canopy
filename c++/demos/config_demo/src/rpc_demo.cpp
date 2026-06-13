/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "config_demo_app.h"

#include <calculator_impl.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <connection_factory/connection_factory.h>
#include <rpc/rpc.h>

namespace config_demo::v1
{
    namespace
    {
        struct paired_state
        {
            rpc::event server_ready;
            rpc::event client_finished;
            std::atomic<demo_error> error_code{demo_error{rpc::error::OK()}};
        };

        void record_error(
            paired_state& state,
            demo_error error_code)
        {
            if (error_code == rpc::error::OK())
                error_code = demo_error::INVALID_RESULT;

            auto expected = demo_error{rpc::error::OK()};
            state.error_code.compare_exchange_strong(
                expected, error_code, std::memory_order_acq_rel, std::memory_order_acquire);
        }

        [[nodiscard]] auto service_name(const rpc::connection_factory::named_connection_settings& connection) -> std::string
        {
            return connection.service_name ? connection.service_name.value() : connection.name;
        }

        [[nodiscard]] auto transport_type(const rpc::connection_factory::connection_settings& settings) -> std::string
        {
            if (!settings.transport || settings.transport->type.empty())
                return "stream_rpc";
            return settings.transport->type;
        }

        CORO_TASK(demo_error)
        run_calculator_calls(const rpc::shared_ptr<i_calculator>& remote)
        {
            if (!remote)
                CO_RETURN demo_error::INVALID_RESULT;

            int result = 0;
            auto error = CO_AWAIT remote->add(100, 200, result);
            RPC_INFO("Calculator: add(100, 200) = {} (error={})", result, error.value());
            if (error != rpc::error::OK())
                CO_RETURN error;
            if (result != 300)
            {
                RPC_ERROR("Calculator: add expected 300, got {}", result);
                CO_RETURN demo_error::CALCULATOR_MISMATCH;
            }

            error = CO_AWAIT remote->multiply(7, 8, result);
            RPC_INFO("Calculator: multiply(7, 8) = {} (error={})", result, error.value());
            if (error != rpc::error::OK())
                CO_RETURN error;
            if (result != 56)
            {
                RPC_ERROR("Calculator: multiply expected 56, got {}", result);
                CO_RETURN demo_error::CALCULATOR_MISMATCH;
            }

            error = CO_AWAIT remote->subtract(500, 200, result);
            RPC_INFO("Calculator: subtract(500, 200) = {} (error={})", result, error.value());
            if (error != rpc::error::OK())
                CO_RETURN error;
            if (result != 300)
            {
                RPC_ERROR("Calculator: subtract expected 300, got {}", result);
                CO_RETURN demo_error::CALCULATOR_MISMATCH;
            }

            error = CO_AWAIT remote->divide(144, 12, result);
            RPC_INFO("Calculator: divide(144, 12) = {} (error={})", result, error.value());
            if (error != rpc::error::OK())
                CO_RETURN error;
            if (result != 12)
            {
                RPC_ERROR("Calculator: divide expected 12, got {}", result);
                CO_RETURN demo_error::CALCULATOR_MISMATCH;
            }

            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(void)
        run_paired_server(
            const rpc::connection_factory::application_runtime& runtime,
            const rpc::connection_factory::named_connection_settings& server,
            std::shared_ptr<coro::scheduler> scheduler,
            paired_state& state)
        {
            auto shutdown_event = std::make_shared<rpc::event>();
            auto service = rpc::root_service::create(
                service_name(server), rpc::create_local_zone(server.zone_subnet), std::move(scheduler));
            service->set_default_encoding(rpc::encoding::yas_binary);
            service->set_shutdown_event(shutdown_event);

            auto accept_result = CO_AWAIT rpc::connection_factory::accept_rpc<i_calculator, i_calculator>(
                [](const rpc::shared_ptr<i_calculator>&,
                    const std::shared_ptr<rpc::service>& service) -> CORO_TASK(rpc::service_connect_result<i_calculator>)
                {
                    CO_RETURN rpc::service_connect_result<i_calculator>{
                        rpc::error::OK(), rpc::shared_ptr<i_calculator>(new calculator_impl(service))};
                },
                server.connection,
                service,
                runtime.create_context());

            if (accept_result.error_code != rpc::error::OK() || (!accept_result.listener && !accept_result.connection))
            {
                RPC_ERROR("Server: accept_rpc failed, error={}", accept_result.error_code);
                record_error(state, accept_result.error_code);
                state.server_ready.set();
                state.client_finished.set();
                CO_RETURN;
            }

            RPC_INFO("Server: accept path ready");
            service.reset();
            state.server_ready.set();
            co_await state.client_finished.wait();

            if (accept_result.listener)
                CO_AWAIT accept_result.listener->stop();
            accept_result.listener.reset();
            accept_result.connection.reset();
            co_await shutdown_event->wait();
            RPC_INFO("Server: shutdown complete");
        }

        CORO_TASK(void)
        run_paired_client(
            const rpc::connection_factory::application_runtime& runtime,
            const rpc::connection_factory::named_connection_settings& client,
            std::shared_ptr<coro::scheduler> scheduler,
            paired_state& state)
        {
            co_await state.server_ready.wait();
            if (state.error_code.load(std::memory_order_acquire) != rpc::error::OK())
            {
                state.client_finished.set();
                CO_RETURN;
            }

            auto service = rpc::root_service::create(
                service_name(client), rpc::create_local_zone(client.zone_subnet), std::move(scheduler));
            service->set_default_encoding(rpc::encoding::yas_binary);

            auto connect_result = CO_AWAIT rpc::connection_factory::connect_rpc<i_calculator, i_calculator>(
                rpc::shared_ptr<i_calculator>(), client.connection, service, runtime.create_context());
            if (connect_result.error_code != rpc::error::OK() || !connect_result.output_interface)
            {
                RPC_ERROR("Client: connect_rpc failed, error={}", connect_result.error_code);
                record_error(state, connect_result.error_code);
                state.client_finished.set();
                CO_RETURN;
            }

            const demo_error call_error = CO_AWAIT run_calculator_calls(connect_result.output_interface);
            if (call_error != rpc::error::OK())
                record_error(state, call_error);

            connect_result.output_interface.reset();
            service.reset();
            state.client_finished.set();
            RPC_INFO("Client: shutdown complete");
        }

        [[nodiscard]] auto run_paired_stream_rpc(
            const rpc::connection_factory::application_runtime& runtime,
            const execution_settings& execution,
            const rpc::connection_factory::named_connection_settings& server,
            const rpc::connection_factory::named_connection_settings& client,
            const std::shared_ptr<coro::scheduler>& server_scheduler,
            const std::shared_ptr<coro::scheduler>& client_scheduler) -> int
        {
            for (uint64_t iteration = 0; iteration < execution.iterations; ++iteration)
            {
                RPC_INFO("Config demo paired stream RPC iteration {}", iteration + 1);
                paired_state state;
                coro::sync_wait(
                    coro::when_all(
                        run_paired_server(runtime, server, server_scheduler, state),
                        run_paired_client(runtime, client, client_scheduler, state)));
                const demo_error error_code = state.error_code.load(std::memory_order_acquire);
                if (error_code != rpc::error::OK())
                    return error_code;
            }

            return rpc::error::OK();
        }

        CORO_TASK(demo_error)
        run_local_child_once(
            const rpc::connection_factory::named_connection_settings& client,
            std::shared_ptr<coro::scheduler> scheduler)
        {
#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
            const auto configured_transport = transport_type(client.connection);
            if (configured_transport != "local")
            {
                RPC_ERROR("local child mode requires client.transport.type local, got {}", configured_transport);
                CO_RETURN demo_error::INVALID_CONFIGURATION;
            }

            auto service = rpc::root_service::create(
                service_name(client), rpc::create_local_zone(client.zone_subnet), std::move(scheduler));
            service->set_default_encoding(rpc::encoding::yas_binary);

            auto connect_result = CO_AWAIT rpc::connection_factory::connect_local_child_rpc<i_calculator, i_calculator>(
                rpc::shared_ptr<i_calculator>(),
                [](const rpc::shared_ptr<i_calculator>&, const std::shared_ptr<rpc::service>& child_service)
                    -> CORO_TASK(rpc::service_connect_result<i_calculator>)
                {
                    CO_RETURN rpc::service_connect_result<i_calculator>{
                        rpc::error::OK(), rpc::shared_ptr<i_calculator>(new calculator_impl(child_service))};
                },
                client.connection,
                service);

            if (connect_result.error_code != rpc::error::OK() || !connect_result.output_interface)
            {
                RPC_ERROR("local child: connect failed, error={}", connect_result.error_code);
                CO_RETURN connect_result.error_code != rpc::error::OK() ? connect_result.error_code
                                                                        : demo_error::INVALID_RESULT.value();
            }

            const demo_error error_code = CO_AWAIT run_calculator_calls(connect_result.output_interface);
            connect_result.output_interface.reset();
            service.reset();
            CO_RETURN error_code;
#else
            (void)client;
            (void)scheduler;
            RPC_ERROR("local child mode is unavailable because local transport support is not built");
            CO_RETURN demo_error::UNSUPPORTED_CONFIGURATION;
#endif
        }

        [[nodiscard]] auto run_local_child(
            const execution_settings& execution,
            const rpc::connection_factory::named_connection_settings& client,
            const std::shared_ptr<coro::scheduler>& scheduler) -> int
        {
            for (uint64_t iteration = 0; iteration < execution.iterations; ++iteration)
            {
                RPC_INFO("Config demo local child iteration {}", iteration + 1);
                const demo_error error_code = coro::sync_wait(run_local_child_once(client, scheduler));
                if (error_code != rpc::error::OK())
                    return error_code;
            }
            return rpc::error::OK();
        }

        CORO_TASK(demo_error)
        run_external_native_once(
            const rpc::connection_factory::application_runtime& runtime,
            const rpc::connection_factory::named_connection_settings& client,
            std::shared_ptr<coro::scheduler> scheduler)
        {
            const auto configured_transport = transport_type(client.connection);
            if (configured_transport == "stream_rpc")
            {
                RPC_ERROR("external native mode is for native transports; add an acceptor for stream_rpc");
                CO_RETURN demo_error::INVALID_CONFIGURATION;
            }
            if (configured_transport == "local")
            {
                RPC_ERROR("external native mode cannot supply the local child factory; use local child");
                CO_RETURN demo_error::INVALID_CONFIGURATION;
            }

            auto service = rpc::root_service::create(
                service_name(client), rpc::create_local_zone(client.zone_subnet), std::move(scheduler));
            service->set_default_encoding(rpc::encoding::yas_binary);

            auto connect_result = CO_AWAIT rpc::connection_factory::connect_rpc<i_calculator, i_calculator>(
                rpc::shared_ptr<i_calculator>(), client.connection, service, runtime.create_context());
            if (connect_result.error_code != rpc::error::OK() || !connect_result.output_interface)
            {
                RPC_ERROR("external native: connect failed, error={}", connect_result.error_code);
                CO_RETURN connect_result.error_code != rpc::error::OK() ? connect_result.error_code
                                                                        : demo_error::INVALID_RESULT.value();
            }

            const demo_error error_code = CO_AWAIT run_calculator_calls(connect_result.output_interface);
            connect_result.output_interface.reset();
            service.reset();
            CO_RETURN error_code;
        }

        [[nodiscard]] auto run_external_native(
            const rpc::connection_factory::application_runtime& runtime,
            const execution_settings& execution,
            const rpc::connection_factory::named_connection_settings& client,
            const std::shared_ptr<coro::scheduler>& scheduler) -> int
        {
            for (uint64_t iteration = 0; iteration < execution.iterations; ++iteration)
            {
                RPC_INFO("Config demo external native iteration {}", iteration + 1);
                const demo_error error_code = coro::sync_wait(run_external_native_once(runtime, client, scheduler));
                if (error_code != rpc::error::OK())
                    return error_code;
            }
            return rpc::error::OK();
        }

        [[nodiscard]] auto find_named_connection(
            const rpc::connection_factory::application_runtime& runtime,
            const std::string& name) -> const rpc::connection_factory::named_connection_settings*
        {
            const auto* connection = runtime.find_connection(name);
            if (!connection)
                RPC_ERROR("no connection named '{}'", name);
            return connection;
        }

        [[nodiscard]] auto validate_connector(const rpc::connection_factory::named_connection_settings& connection)
            -> demo_error
        {
            if (connection.role != rpc::connection_factory::connection_role::connector)
            {
                RPC_ERROR("connection '{}' must have role connector", connection.name);
                return demo_error::INVALID_CONFIGURATION;
            }
            return rpc::error::OK();
        }

        [[nodiscard]] auto validate_acceptor(const rpc::connection_factory::named_connection_settings& connection)
            -> demo_error
        {
            if (connection.role != rpc::connection_factory::connection_role::acceptor)
            {
                RPC_ERROR("connection '{}' must have role acceptor", connection.name);
                return demo_error::INVALID_CONFIGURATION;
            }
            return rpc::error::OK();
        }
    } // namespace

    auto run_configured_demo(
        const rpc::connection_factory::application_runtime& runtime,
        const execution_settings& execution,
        const std::shared_ptr<coro::scheduler>& scheduler_1,
        const std::shared_ptr<coro::scheduler>& scheduler_2) -> int
    {
        const auto* client = find_named_connection(runtime, execution.client_connection);
        if (!client)
            return demo_error::INVALID_CONFIGURATION;

        demo_error error_code = validate_connector(*client);
        if (error_code != rpc::error::OK())
            return error_code;

        const auto* server
            = execution.server_connection.empty() ? nullptr : runtime.find_connection(execution.server_connection);
        if (!execution.server_connection.empty() && !server)
        {
            RPC_ERROR("no connection named '{}'", execution.server_connection);
            return demo_error::INVALID_CONFIGURATION;
        }

        const auto client_transport = transport_type(client->connection);
        if (server)
        {
            error_code = validate_acceptor(*server);
            if (error_code != rpc::error::OK())
                return error_code;

            if (transport_type(server->connection) != "stream_rpc" || client_transport != "stream_rpc")
            {
                RPC_ERROR("paired acceptor/connector demo requires stream_rpc on both connections");
                return demo_error::INVALID_CONFIGURATION;
            }
            return run_paired_stream_rpc(runtime, execution, *server, *client, scheduler_1, scheduler_2);
        }

        if (client_transport == "local")
            return run_local_child(execution, *client, scheduler_1);
        if (client_transport == "stream_rpc")
        {
            RPC_ERROR("stream_rpc connector requires a named acceptor connection");
            return demo_error::INVALID_CONFIGURATION;
        }
        return run_external_native(runtime, execution, *client, scheduler_1);
    }
} // namespace config_demo::v1
