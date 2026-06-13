/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "config_demo_app.h"

#include <calculator_impl.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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
            std::atomic<bool> ok{true};
        };

        [[nodiscard]] auto make_zone(uint64_t subnet) -> rpc::zone
        {
            auto zone_id = rpc::DEFAULT_PREFIX;
            std::ignore = zone_id.set_subnet(subnet);
            return rpc::zone{zone_id};
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

        [[nodiscard]] auto make_factory_context(
            const rpc::connection_factory::application_runtime& runtime,
            const rpc::connection_factory::named_connection_settings& connection,
            std::shared_ptr<coro::scheduler> scheduler) -> rpc::connection_factory::context
        {
            auto context = runtime.context_for(connection, std::move(scheduler));
            if (context.error_code != rpc::error::OK())
            {
                throw std::runtime_error("failed to create context for " + connection.name + ": " + context.message);
            }
            return std::move(context.context);
        }

        [[nodiscard]] auto call_ok(
            std::string_view name,
            int a,
            int b,
            int expected,
            int result,
            int error) -> bool
        {
            RPC_INFO("Calculator: {}({}, {}) = {} (error={})", name, a, b, result, error);
            if (error != rpc::error::OK() || result != expected)
            {
                RPC_ERROR("Calculator: {} expected {}, got {}, error={}", name, expected, result, error);
                return false;
            }
            return true;
        }

        CORO_TASK(bool)
        run_calculator_calls(const rpc::shared_ptr<i_calculator>& remote)
        {
            if (!remote)
                CO_RETURN false;

            int result = 0;
            auto error = CO_AWAIT remote->add(100, 200, result);
            if (!call_ok("add", 100, 200, 300, result, error))
                CO_RETURN false;

            error = CO_AWAIT remote->multiply(7, 8, result);
            if (!call_ok("multiply", 7, 8, 56, result, error))
                CO_RETURN false;

            error = CO_AWAIT remote->subtract(500, 200, result);
            if (!call_ok("subtract", 500, 200, 300, result, error))
                CO_RETURN false;

            error = CO_AWAIT remote->divide(144, 12, result);
            if (!call_ok("divide", 144, 12, 12, result, error))
                CO_RETURN false;

            CO_RETURN true;
        }

        CORO_TASK(void)
        run_paired_server(
            const rpc::connection_factory::application_runtime& runtime,
            const rpc::connection_factory::named_connection_settings& server,
            std::shared_ptr<coro::scheduler> scheduler,
            paired_state& state)
        {
            try
            {
                auto context = make_factory_context(runtime, server, scheduler);
                auto shutdown_event = std::make_shared<rpc::event>();
                const auto root_service_name = service_name(server);
                auto service = rpc::root_service::create(
                    root_service_name.c_str(), make_zone(server.zone_subnet), std::move(scheduler));
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
                    context);

                if (accept_result.error_code != rpc::error::OK() || (!accept_result.listener && !accept_result.connection))
                {
                    RPC_ERROR("Server: accept_rpc failed, error={}", accept_result.error_code);
                    state.ok.store(false, std::memory_order_release);
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
            catch (const std::exception& error)
            {
                RPC_ERROR("Server: {}", error.what());
                state.ok.store(false, std::memory_order_release);
                state.server_ready.set();
                state.client_finished.set();
            }
        }

        CORO_TASK(void)
        run_paired_client(
            const rpc::connection_factory::application_runtime& runtime,
            const rpc::connection_factory::named_connection_settings& client,
            std::shared_ptr<coro::scheduler> scheduler,
            paired_state& state)
        {
            try
            {
                co_await state.server_ready.wait();
                if (!state.ok.load(std::memory_order_acquire))
                {
                    state.client_finished.set();
                    CO_RETURN;
                }

                auto context = make_factory_context(runtime, client, scheduler);
                const auto root_service_name = service_name(client);
                auto service = rpc::root_service::create(
                    root_service_name.c_str(), make_zone(client.zone_subnet), std::move(scheduler));
                service->set_default_encoding(rpc::encoding::yas_binary);

                auto connect_result = CO_AWAIT rpc::connection_factory::connect_rpc<i_calculator, i_calculator>(
                    rpc::shared_ptr<i_calculator>(), client.connection, service, context);
                if (connect_result.error_code != rpc::error::OK() || !connect_result.output_interface)
                {
                    RPC_ERROR("Client: connect_rpc failed, error={}", connect_result.error_code);
                    state.ok.store(false, std::memory_order_release);
                    state.client_finished.set();
                    CO_RETURN;
                }

                if (!CO_AWAIT run_calculator_calls(connect_result.output_interface))
                    state.ok.store(false, std::memory_order_release);

                connect_result.output_interface.reset();
                service.reset();
                state.client_finished.set();
                RPC_INFO("Client: shutdown complete");
            }
            catch (const std::exception& error)
            {
                RPC_ERROR("Client: {}", error.what());
                state.ok.store(false, std::memory_order_release);
                state.client_finished.set();
            }
        }

        [[nodiscard]] auto run_paired_stream_rpc(
            const rpc::connection_factory::application_runtime& runtime,
            const demo_settings& settings,
            const rpc::connection_factory::named_connection_settings& server,
            const rpc::connection_factory::named_connection_settings& client,
            const std::shared_ptr<coro::scheduler>& server_scheduler,
            const std::shared_ptr<coro::scheduler>& client_scheduler) -> bool
        {
            for (uint64_t iteration = 0; iteration < settings.iterations; ++iteration)
            {
                RPC_INFO("Config demo paired stream RPC iteration {}", iteration + 1);
                paired_state state;
                coro::sync_wait(
                    coro::when_all(
                        run_paired_server(runtime, server, server_scheduler, state),
                        run_paired_client(runtime, client, client_scheduler, state)));
                if (!state.ok.load(std::memory_order_acquire))
                    return false;
            }

            return true;
        }

        CORO_TASK(bool)
        run_local_child_once(
            const rpc::connection_factory::named_connection_settings& client,
            std::shared_ptr<coro::scheduler> scheduler)
        {
#ifdef CANOPY_CONNECTION_FACTORY_HAS_LOCAL
            const auto configured_transport = transport_type(client.connection);
            if (configured_transport != "local")
            {
                RPC_ERROR("local child mode requires client.transport.type local, got {}", configured_transport);
                CO_RETURN false;
            }

            const auto root_service_name = service_name(client);
            auto service = rpc::root_service::create(
                root_service_name.c_str(), make_zone(client.zone_subnet), std::move(scheduler));
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
                CO_RETURN false;
            }

            const bool ok = CO_AWAIT run_calculator_calls(connect_result.output_interface);
            connect_result.output_interface.reset();
            service.reset();
            CO_RETURN ok;
#else
            (void)client;
            (void)scheduler;
            RPC_ERROR("local child mode is unavailable because local transport support is not built");
            CO_RETURN false;
#endif
        }

        [[nodiscard]] auto run_local_child(
            const demo_settings& settings,
            const rpc::connection_factory::named_connection_settings& client,
            const std::shared_ptr<coro::scheduler>& scheduler) -> bool
        {
            for (uint64_t iteration = 0; iteration < settings.iterations; ++iteration)
            {
                RPC_INFO("Config demo local child iteration {}", iteration + 1);
                if (!coro::sync_wait(run_local_child_once(client, scheduler)))
                    return false;
            }
            return true;
        }

        CORO_TASK(bool)
        run_external_native_once(
            const rpc::connection_factory::application_runtime& runtime,
            const rpc::connection_factory::named_connection_settings& client,
            std::shared_ptr<coro::scheduler> scheduler)
        {
            const auto configured_transport = transport_type(client.connection);
            if (configured_transport == "stream_rpc")
            {
                RPC_ERROR("external native mode is for native transports; add an acceptor for stream_rpc");
                CO_RETURN false;
            }
            if (configured_transport == "local")
            {
                RPC_ERROR("external native mode cannot supply the local child factory; use local child");
                CO_RETURN false;
            }

            auto context = make_factory_context(runtime, client, scheduler);
            const auto root_service_name = service_name(client);
            auto service = rpc::root_service::create(
                root_service_name.c_str(), make_zone(client.zone_subnet), std::move(scheduler));
            service->set_default_encoding(rpc::encoding::yas_binary);

            auto connect_result = CO_AWAIT rpc::connection_factory::connect_rpc<i_calculator, i_calculator>(
                rpc::shared_ptr<i_calculator>(), client.connection, service, context);
            if (connect_result.error_code != rpc::error::OK() || !connect_result.output_interface)
            {
                RPC_ERROR("external native: connect failed, error={}", connect_result.error_code);
                CO_RETURN false;
            }

            const bool ok = CO_AWAIT run_calculator_calls(connect_result.output_interface);
            connect_result.output_interface.reset();
            service.reset();
            CO_RETURN ok;
        }

        [[nodiscard]] auto run_external_native(
            const rpc::connection_factory::application_runtime& runtime,
            const demo_settings& settings,
            const rpc::connection_factory::named_connection_settings& client,
            const std::shared_ptr<coro::scheduler>& scheduler) -> bool
        {
            for (uint64_t iteration = 0; iteration < settings.iterations; ++iteration)
            {
                RPC_INFO("Config demo external native iteration {}", iteration + 1);
                if (!coro::sync_wait(run_external_native_once(runtime, client, scheduler)))
                    return false;
            }
            return true;
        }

        [[nodiscard]] auto named_connection_or_throw(
            const rpc::connection_factory::application_runtime& runtime,
            const std::string& name) -> const rpc::connection_factory::named_connection_settings&
        {
            const auto* connection = runtime.find_connection(name);
            if (!connection)
                throw std::runtime_error("no connection named '" + name + "'");
            return *connection;
        }

        void validate_connector(const rpc::connection_factory::named_connection_settings& connection)
        {
            if (connection.role != rpc::connection_factory::connection_role::connector)
                throw std::runtime_error("connection '" + connection.name + "' must have role connector");
        }

        void validate_acceptor(const rpc::connection_factory::named_connection_settings& connection)
        {
            if (connection.role != rpc::connection_factory::connection_role::acceptor)
                throw std::runtime_error("connection '" + connection.name + "' must have role acceptor");
        }
    } // namespace

    auto run_configured_demo(
        const rpc::connection_factory::application_runtime& runtime,
        const demo_settings& settings,
        const std::shared_ptr<coro::scheduler>& scheduler_1,
        const std::shared_ptr<coro::scheduler>& scheduler_2) -> bool
    {
        const auto& client = named_connection_or_throw(runtime, settings.client_connection);
        validate_connector(client);

        const auto* server
            = settings.server_connection.empty() ? nullptr : runtime.find_connection(settings.server_connection);
        const auto client_transport = transport_type(client.connection);
        if (server)
        {
            validate_acceptor(*server);
            if (transport_type(server->connection) != "stream_rpc" || client_transport != "stream_rpc")
                throw std::runtime_error("paired acceptor/connector demo requires stream_rpc on both connections");
            return run_paired_stream_rpc(runtime, settings, *server, client, scheduler_1, scheduler_2);
        }

        if (client_transport == "local")
            return run_local_child(settings, client, scheduler_1);
        if (client_transport == "stream_rpc")
            throw std::runtime_error("stream_rpc connector requires a named acceptor connection");
        return run_external_native(runtime, settings, client, scheduler_1);
    }
} // namespace config_demo::v1
