/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory_components.h>

#include <memory>
#include <string>
#include <utility>

#include <streaming/spsc_queue/factory.h>
#include <spsc_queue_stream/spsc_queue_stream_config_schema.h>

namespace rpc::connection_factory::detail
{
    namespace
    {
        class spsc_queue_stream_component_factory final : public stream_component_factory
        {
        public:
            bool supports_connect_base() const override { return true; }
            bool supports_accept_single_base() const override { return true; }

            auto connect_base(
                const json::v1::object& settings,
                std::shared_ptr<rpc::service> service,
                const context& factory_context) const -> CORO_TASK(stream_result) override
            {
                auto spsc_settings = materialise_settings<rpc::spsc_queue_stream::stream_settings>(settings);
                if (spsc_settings.error_code != rpc::error::OK())
                    CO_RETURN stream_result{spsc_settings.error_code, {}};
                auto queues = factory_context.get_dependency<rpc::spsc_queue::queue_pair>(
                    spsc_settings.settings.queue_pair ? spsc_settings.settings.queue_pair.value() : std::string{});
                if (!queues)
                    CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};
                CO_RETURN CO_AWAIT rpc::spsc_queue::connect_stream(*queues, std::move(service));
            }

            auto accept_single_base(
                const json::v1::object& settings,
                std::shared_ptr<rpc::service> service,
                const context& factory_context) const -> CORO_TASK(stream_result) override
            {
                auto spsc_settings = materialise_settings<rpc::spsc_queue_stream::stream_settings>(settings);
                if (spsc_settings.error_code != rpc::error::OK())
                    CO_RETURN stream_result{spsc_settings.error_code, {}};
                auto queues = factory_context.get_dependency<rpc::spsc_queue::queue_pair>(
                    spsc_settings.settings.queue_pair ? spsc_settings.settings.queue_pair.value() : std::string{});
                if (!queues)
                    CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};
                CO_RETURN CO_AWAIT rpc::spsc_queue::accept_stream(*queues, std::move(service));
            }
        };
    } // namespace

    void register_spsc_queue_stream_components(stream_component_map& components)
    {
        auto spsc = std::make_shared<spsc_queue_stream_component_factory>();
        components.emplace("spsc", spsc);
        components.emplace("spsc_queue", std::move(spsc));
    }
} // namespace rpc::connection_factory::detail
