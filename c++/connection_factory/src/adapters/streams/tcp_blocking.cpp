/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory_components.h>

#include <memory>
#include <utility>

#include <streaming/tcp_blocking/factory.h>
#include <tcp_blocking_stream/tcp_blocking_stream_config_schema.h>

namespace rpc::connection_factory::detail
{
    namespace
    {
        class tcp_blocking_stream_component_factory final : public stream_component_factory
        {
        public:
            bool supports_connect_base() const override { return true; }
            bool supports_accept_base() const override { return true; }

            auto connect_base(
                const json::v1::object& settings,
                std::shared_ptr<rpc::service> service,
                const context&) const -> CORO_TASK(stream_result) override
            {
                auto endpoint = materialise_settings<rpc::tcp_blocking_stream::endpoint>(settings);
                if (endpoint.error_code != rpc::error::OK())
                    CO_RETURN stream_result{endpoint.error_code, {}};
                CO_RETURN CO_AWAIT rpc::tcp_blocking::connect_stream(endpoint.settings, std::move(service));
            }

            auto accept_base(
                const json::v1::object& settings,
                std::shared_ptr<rpc::service>,
                const context&) const -> CORO_TASK(stream_acceptor_result) override
            {
                auto endpoint = materialise_settings<rpc::tcp_blocking_stream::endpoint>(settings);
                if (endpoint.error_code != rpc::error::OK())
                    CO_RETURN stream_acceptor_result{endpoint.error_code, {}, {}, 0};
                CO_RETURN stream_acceptor_result{
                    rpc::error::OK(), rpc::tcp_blocking::make_acceptor(endpoint.settings), {}, endpoint.settings.port};
            }
        };
    } // namespace

    void register_tcp_blocking_stream_components(stream_component_map& components)
    {
        components.emplace("tcp_blocking", std::make_shared<tcp_blocking_stream_component_factory>());
    }
} // namespace rpc::connection_factory::detail
