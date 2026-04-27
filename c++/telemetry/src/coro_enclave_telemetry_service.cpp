/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <rpc/telemetry/telemetry_service_factory.h>

#include <mutex>
#include <iterator>
#include <utility>
#include <vector>

namespace rpc::telemetry
{
    class coro_enclave_telemetry_service : public i_telemetry_service
    {
        mutable std::mutex mutex_;
        mutable std::vector<rpc::telemetry_event> pending_events_;

        template<typename Event> rpc::telemetry_event make_event(const Event& event) const
        {
            return rpc::telemetry_event{
                {}, rpc::id<Event>::get(rpc::get_version()), rpc::to_yas_binary<std::vector<char>>(event)};
        }

        template<typename Event> void post_event(Event event) const
        {
            auto report_event = make_event(event);
            std::lock_guard lock(mutex_);
            pending_events_.push_back(std::move(report_event));
        }

    public:
        coro_enclave_telemetry_service() = default;

        void pop_events(std::vector<rpc::telemetry_event>& events) const
        {
            std::lock_guard lock(mutex_);
            events.insert(
                events.end(),
                std::make_move_iterator(pending_events_.begin()),
                std::make_move_iterator(pending_events_.end()));
            pending_events_.clear();
        }

#define RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(method_name, event_type)                                                    \
    void method_name(const rpc::telemetry::event_type& event) const override                                           \
    {                                                                                                                  \
        return post_event(rpc::telemetry::event_type(event));                                                          \
    }

        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_creation,
            service_creation_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_deletion,
            service_deletion_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_send,
            service_send_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_post,
            service_post_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_try_cast,
            service_try_cast_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_add_ref,
            service_add_ref_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_release,
            service_release_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_object_released,
            service_object_released_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_transport_down,
            service_transport_down_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_proxy_creation,
            service_proxy_creation_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_cloned_service_proxy_creation,
            cloned_service_proxy_creation_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_proxy_deletion,
            service_proxy_deletion_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_proxy_send,
            service_proxy_send_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_proxy_post,
            service_proxy_post_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_proxy_try_cast,
            service_proxy_try_cast_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_proxy_add_ref,
            service_proxy_add_ref_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_proxy_release,
            service_proxy_release_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_proxy_object_released,
            service_proxy_object_released_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_proxy_transport_down,
            service_proxy_transport_down_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_proxy_add_external_ref,
            service_proxy_external_ref_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_service_proxy_release_external_ref,
            service_proxy_external_ref_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_creation,
            transport_creation_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_deletion,
            transport_deletion_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_status_change,
            transport_status_change_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_add_destination,
            transport_destination_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_remove_destination,
            transport_destination_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_accept,
            transport_accept_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_outbound_send,
            transport_send_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_outbound_post,
            transport_post_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_outbound_try_cast,
            transport_try_cast_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_outbound_add_ref,
            transport_add_ref_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_outbound_release,
            transport_release_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_outbound_object_released,
            transport_object_released_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_outbound_transport_down,
            transport_transport_down_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_inbound_send,
            transport_send_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_inbound_post,
            transport_post_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_inbound_try_cast,
            transport_try_cast_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_inbound_add_ref,
            transport_add_ref_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_inbound_release,
            transport_release_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_inbound_object_released,
            transport_object_released_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_transport_inbound_transport_down,
            transport_transport_down_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_impl_creation,
            impl_creation_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_impl_deletion,
            impl_deletion_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_stub_creation,
            stub_creation_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_stub_deletion,
            stub_deletion_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_stub_send,
            stub_send_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_stub_add_ref,
            stub_add_ref_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_stub_release,
            stub_release_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_object_proxy_creation,
            object_proxy_creation_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_object_proxy_deletion,
            object_proxy_deletion_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_interface_proxy_creation,
            interface_proxy_creation_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_interface_proxy_deletion,
            interface_proxy_deletion_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_interface_proxy_send,
            interface_proxy_send_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_pass_through_creation,
            pass_through_creation_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_pass_through_deletion,
            pass_through_deletion_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_pass_through_add_ref,
            pass_through_add_ref_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_pass_through_release,
            pass_through_release_event)
        RPC_CORO_ENCLAVE_TELEMETRY_FORWARD(
            on_pass_through_status_change,
            pass_through_status_change_event)
#undef RPC_CORO_ENCLAVE_TELEMETRY_FORWARD

        void message(const rpc::log_record& event) const override
        {
            return post_event(rpc::log_record(event));
        }
    };

    bool create_coro_enclave_telemetry_service(std::shared_ptr<i_telemetry_service>& service)
    {
        service = std::make_shared<coro_enclave_telemetry_service>();
        return true;
    }

    bool pop_coro_enclave_telemetry_events(
        const std::shared_ptr<i_telemetry_service>& service,
        std::vector<rpc::telemetry_event>& events)
    {
        auto coro_service = std::dynamic_pointer_cast<coro_enclave_telemetry_service>(service);
        if (!coro_service)
            return false;
        coro_service->pop_events(events);
        return true;
    }
}
