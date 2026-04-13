/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#include <rpc/telemetry/enclave_telemetry_service.h>

namespace rpc
{
    namespace
    {
        template<typename... Args> void ignore_event(const Args&...) { }
    }

    // Global telemetry service definition for enclave builds
#ifdef CANOPY_USE_TELEMETRY
    std::shared_ptr<i_telemetry_service> telemetry_service_ = nullptr;
#endif

    enclave_telemetry_service::enclave_telemetry_service() { }

    void enclave_telemetry_service::on_service_creation(
        const std::string& name,
        rpc::zone zone_id,
        rpc::destination_zone parent_zone_id) const
    {
        ignore_event(name, zone_id, parent_zone_id);
    }

    void enclave_telemetry_service::on_service_deletion(rpc::zone zone_id) const
    {
        ignore_event(zone_id);
    }

    void enclave_telemetry_service::on_service_send(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id, method_id);
    }

    void enclave_telemetry_service::on_service_post(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id, method_id);
    }
    void enclave_telemetry_service::on_service_try_cast(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id);
    }

    void enclave_telemetry_service::on_service_add_ref(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::requesting_zone requesting_zone_id,
        rpc::add_ref_options options) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id, requesting_zone_id, options);
    }

    void enclave_telemetry_service::on_service_release(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id, options);
    }

    void enclave_telemetry_service::on_service_object_released(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id);
    }

    void enclave_telemetry_service::on_service_transport_down(
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        ignore_event(zone_id, destination_zone_id, caller_zone_id);
    }
    void enclave_telemetry_service::on_service_proxy_creation(
        const std::string& service_name,
        const std::string& service_proxy_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        ignore_event(service_name, service_proxy_name, zone_id, destination_zone_id, caller_zone_id);
    }
    void enclave_telemetry_service::on_cloned_service_proxy_creation(
        const std::string& service_name,
        const std::string& service_proxy_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        ignore_event(service_name, service_proxy_name, zone_id, destination_zone_id, caller_zone_id);
    }
    void enclave_telemetry_service::on_service_proxy_deletion(
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        ignore_event(zone_id, destination_zone_id, caller_zone_id);
    }
    void enclave_telemetry_service::on_service_proxy_send(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id, method_id);
    }
    void enclave_telemetry_service::on_service_proxy_post(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id, method_id);
    }
    void enclave_telemetry_service::on_service_proxy_try_cast(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id);
    }
    void enclave_telemetry_service::on_service_proxy_add_ref(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::requesting_zone requesting_zone_id,
        rpc::add_ref_options options) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id, requesting_zone_id, options);
    }
    void enclave_telemetry_service::on_service_proxy_release(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id, options);
    }

    void enclave_telemetry_service::on_service_proxy_object_released(
        rpc::zone zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id) const
    {
        ignore_event(zone_id, remote_object_id, caller_zone_id);
    }

    void enclave_telemetry_service::on_service_proxy_transport_down(
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        ignore_event(zone_id, destination_zone_id, caller_zone_id);
    }

    void enclave_telemetry_service::on_impl_creation(
        const std::string& name,
        uint64_t address,
        rpc::zone zone_id) const
    {
        ignore_event(name, address, zone_id);
    }
    void enclave_telemetry_service::on_impl_deletion(
        uint64_t address,
        rpc::zone zone_id) const
    {
        ignore_event(address, zone_id);
    }

    void enclave_telemetry_service::on_stub_creation(
        rpc::zone zone_id,
        rpc::object object_id,
        uint64_t address) const
    {
        ignore_event(zone_id, object_id, address);
    }
    void enclave_telemetry_service::on_stub_deletion(
        rpc::zone zone_id,
        rpc::object object_id) const
    {
        ignore_event(zone_id, object_id);
    }
    void enclave_telemetry_service::on_stub_send(
        rpc::zone zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        ignore_event(zone_id, object_id, interface_id, method_id);
    }
    void enclave_telemetry_service::on_stub_add_ref(
        rpc::zone zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        uint64_t count,
        rpc::caller_zone caller_zone_id) const
    {
        ignore_event(zone_id, object_id, interface_id, count, caller_zone_id);
    }
    void enclave_telemetry_service::on_stub_release(
        rpc::zone zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        uint64_t count,
        rpc::caller_zone caller_zone_id) const
    {
        ignore_event(zone_id, object_id, interface_id, count, caller_zone_id);
    }

    void enclave_telemetry_service::on_object_proxy_creation(
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        bool add_ref_done) const
    {
        ignore_event(zone_id, destination_zone_id, object_id, add_ref_done);
    }
    void enclave_telemetry_service::on_object_proxy_deletion(
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id) const
    {
        ignore_event(zone_id, destination_zone_id, object_id);
    }

    void enclave_telemetry_service::on_interface_proxy_creation(
        const std::string& name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        ignore_event(name, zone_id, destination_zone_id, object_id, interface_id);
    }
    void enclave_telemetry_service::on_interface_proxy_deletion(
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        ignore_event(zone_id, destination_zone_id, object_id, interface_id);
    }
    void enclave_telemetry_service::on_interface_proxy_send(
        const std::string& method_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        ignore_event(method_name, zone_id, destination_zone_id, object_id, interface_id, method_id);
    }

    void enclave_telemetry_service::on_service_proxy_add_external_ref(
        rpc::zone operating_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        ignore_event(operating_zone_id, destination_zone_id, caller_zone_id);
    }

    void enclave_telemetry_service::on_service_proxy_release_external_ref(
        rpc::zone operating_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        ignore_event(operating_zone_id, destination_zone_id, caller_zone_id);
    }

    void enclave_telemetry_service::message(
        rpc::i_telemetry_service::level_enum level,
        const std::string& message) const
    {
        ignore_event(level, message);
    }

    void enclave_telemetry_service::on_transport_creation(
        const std::string& name,
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::transport_status status) const
    {
        ignore_event(name, zone_id, adjacent_zone_id, status);
    }

    void enclave_telemetry_service::on_transport_deletion(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id) const
    {
        ignore_event(zone_id, adjacent_zone_id);
    }

    void enclave_telemetry_service::on_transport_status_change(
        const std::string& name,
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::transport_status old_status,
        rpc::transport_status new_status) const
    {
        ignore_event(name, zone_id, adjacent_zone_id, old_status, new_status);
    }

    void enclave_telemetry_service::on_transport_add_destination(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination,
        rpc::caller_zone caller) const
    {
        ignore_event(zone_id, adjacent_zone_id, destination, caller);
    }

    void enclave_telemetry_service::on_transport_remove_destination(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination,
        rpc::caller_zone caller) const
    {
        ignore_event(zone_id, adjacent_zone_id, destination, caller);
    }

    void enclave_telemetry_service::on_transport_accept(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        int result) const
    {
        ignore_event(zone_id, adjacent_zone_id, result);
    }

    void enclave_telemetry_service::on_transport_outbound_send(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
        std::ignore = method_id;
    }

    void enclave_telemetry_service::on_transport_outbound_post(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
        std::ignore = method_id;
    }

    void enclave_telemetry_service::on_transport_outbound_try_cast(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
    }

    void enclave_telemetry_service::on_transport_outbound_add_ref(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::requesting_zone requesting_zone_id,
        rpc::add_ref_options options) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = requesting_zone_id;
        std::ignore = options;
    }

    void enclave_telemetry_service::on_transport_outbound_release(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = options;
    }

    void enclave_telemetry_service::on_transport_outbound_object_released(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
    }

    void enclave_telemetry_service::on_transport_outbound_transport_down(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
    }

    void enclave_telemetry_service::on_transport_inbound_send(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
        std::ignore = method_id;
    }

    void enclave_telemetry_service::on_transport_inbound_post(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
        std::ignore = method_id;
    }

    void enclave_telemetry_service::on_transport_inbound_try_cast(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::interface_ordinal interface_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
    }

    void enclave_telemetry_service::on_transport_inbound_add_ref(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::requesting_zone requesting_zone_id,
        rpc::add_ref_options options) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = requesting_zone_id;
        std::ignore = options;
    }

    void enclave_telemetry_service::on_transport_inbound_release(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = options;
    }

    void enclave_telemetry_service::on_transport_inbound_object_released(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::remote_object remote_object_id,
        rpc::caller_zone caller_zone_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
    }

    void enclave_telemetry_service::on_transport_inbound_transport_down(
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
    }

    void enclave_telemetry_service::on_pass_through_creation(
        rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        uint64_t shared_count,
        uint64_t optimistic_count) const
    {
        ignore_event(zone_id, forward_destination, reverse_destination, shared_count, optimistic_count);
    }

    void enclave_telemetry_service::on_pass_through_deletion(
        rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination) const
    {
        ignore_event(zone_id, forward_destination, reverse_destination);
    }

    void enclave_telemetry_service::on_pass_through_add_ref(
        rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        rpc::add_ref_options options,
        int64_t shared_delta,
        int64_t optimistic_delta) const
    {
        ignore_event(zone_id, forward_destination, reverse_destination, options, shared_delta, optimistic_delta);
    }

    void enclave_telemetry_service::on_pass_through_release(
        rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        int64_t shared_delta,
        int64_t optimistic_delta) const
    {
        ignore_event(zone_id, forward_destination, reverse_destination, shared_delta, optimistic_delta);
    }

    void enclave_telemetry_service::on_pass_through_status_change(
        rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        rpc::transport_status forward_status,
        rpc::transport_status reverse_status) const
    {
        ignore_event(zone_id, forward_destination, reverse_destination, forward_status, reverse_status);
    }
}
