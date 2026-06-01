/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory_components.h>

#include <memory>
#include <utility>

#include <streaming/tcp_coroutine/factory.h>
#include <tcp_coroutine_stream/tcp_coroutine_stream_config_schema.h>

namespace rpc::connection_factory::detail
{
    namespace
    {
        class tcp_coroutine_stream_component_factory final : public stream_component_factory
        {
        public:
            bool supports_connect_base() const override { return true; }
            bool supports_accept_base() const override { return true; }

            auto connect_base(
                const json::v1::object& settings,
                std::shared_ptr<rpc::service> service,
                const context&) const -> CORO_TASK(stream_result) override
            {
                auto io_options = materialise_settings<rpc::tcp_coroutine_stream::endpoint>(settings);
                if (io_options.error_code != rpc::error::OK())
                    CO_RETURN stream_result{io_options.error_code, {}};

                CO_RETURN CO_AWAIT rpc::tcp_coroutine::connect_stream(io_options.settings, std::move(service));
            }

            auto accept_base(
                const json::v1::object& settings,
                std::shared_ptr<rpc::service> service,
                const context&) const -> CORO_TASK(stream_acceptor_result) override
            {
                auto io_options = materialise_settings<rpc::tcp_coroutine_stream::endpoint>(settings);
                if (io_options.error_code != rpc::error::OK())
                    CO_RETURN stream_acceptor_result{io_options.error_code, {}, {}, 0};

                auto factory_settings = rpc::connection_factory::make_stream_rpc_settings();
                if (!service)
                {
                    auto runtime = rpc::tcp_coroutine::make_runtime(io_options.settings.controller);
                    if (runtime.error_code != rpc::error::OK())
                        CO_RETURN stream_acceptor_result{runtime.error_code, {}, {}, 0};

                    auto acceptor = std::make_shared<::streaming::coroutine::tcp::acceptor>(
                        runtime.controller, io_options.settings.stream);
                    auto listen_result = CO_AWAIT rpc::tcp_coroutine::listen_endpoint(acceptor, io_options.settings);
                    if (listen_result.error_code != rpc::error::OK())
                        CO_RETURN stream_acceptor_result{listen_result.error_code, {}, {}, 0};
                    if (listen_result.port == 0)
                        CO_RETURN stream_acceptor_result{rpc::error::TRANSPORT_ERROR(), {}, {}, 0};

                    CO_RETURN stream_acceptor_result{
                        rpc::error::OK(), std::move(acceptor), std::move(runtime.scheduler_owner), listen_result.port};
                }

                auto acceptor = CO_AWAIT rpc::tcp_coroutine::listen_acceptor(
                    io_options.settings, factory_settings, std::move(service));
                if (acceptor.error_code != rpc::error::OK())
                    CO_RETURN stream_acceptor_result{acceptor.error_code, {}, {}, 0};
                CO_RETURN stream_acceptor_result{
                    rpc::error::OK(), std::move(acceptor.acceptor), std::move(acceptor.owner), acceptor.port};
            }
        };
    } // namespace

    void register_tcp_coroutine_stream_components(stream_component_map& components)
    {
        components.emplace("tcp_coroutine", std::make_shared<tcp_coroutine_stream_component_factory>());
    }
} // namespace rpc::connection_factory::detail
