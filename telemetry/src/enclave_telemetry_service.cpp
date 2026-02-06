/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#include <rpc/telemetry/enclave_telemetry_service.h>
#include <trusted/rpc_telemetry_t.h>

namespace rpc
{
    // Global telemetry service definition for enclave builds
#ifdef CANOPY_USE_TELEMETRY
    std::shared_ptr<i_telemetry_service> telemetry_service_ = nullptr;
#endif

    enclave_telemetry_service::enclave_telemetry_service() { }

    void enclave_telemetry_service::on_service_creation(
        const std::string& name, rpc::zone zone_id, rpc::destination_zone parent_zone_id) const
    {
        on_service_creation_host(name, zone_id.get_val(), parent_zone_id.get_val());
    }

    void enclave_telemetry_service::on_service_deletion(rpc::zone zone_id) const
    {
        on_service_deletion_host(zone_id.get_val());
    }
    void enclave_telemetry_service::on_service_try_cast(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        on_service_try_cast_host(zone_id.get_val(),
            destination_zone_id.get_val(),
            caller_zone_id.get_val(),
            object_id.get_val(),
            interface_id.get_val());
    }

    void enclave_telemetry_service::on_service_add_ref(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options) const
    {
        on_service_add_ref_host(zone_id.get_val(),
            destination_zone_id.get_val(),
            object_id.get_val(),
            caller_zone_id.get_val(),
            known_direction_zone_id.get_val(),
            (uint64_t)options);
    }

    void enclave_telemetry_service::on_service_release(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options) const
    {
        on_service_release_host(zone_id.get_val(),
            destination_zone_id.get_val(),
            object_id.get_val(),
            caller_zone_id.get_val(),
            static_cast<uint64_t>(options));
    }
    void enclave_telemetry_service::on_service_proxy_creation(const std::string& service_name,
        const std::string& service_proxy_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        on_service_proxy_creation_host(
            service_name, service_proxy_name, zone_id.get_val(), destination_zone_id.get_val(), caller_zone_id.get_val());
    }
    void enclave_telemetry_service::on_cloned_service_proxy_creation(const std::string& service_name,
        const std::string& service_proxy_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        on_cloned_service_proxy_creation_host(
            service_name, service_proxy_name, zone_id.get_val(), destination_zone_id.get_val(), caller_zone_id.get_val());
    }
    void enclave_telemetry_service::on_service_proxy_deletion(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
    {
        on_service_proxy_deletion_host(zone_id.get_val(), destination_zone_id.get_val(), caller_zone_id.get_val());
    }
    void enclave_telemetry_service::on_service_proxy_try_cast(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        on_service_proxy_try_cast_host(zone_id.get_val(),
            destination_zone_id.get_val(),
            caller_zone_id.get_val(),
            object_id.get_val(),
            interface_id.get_val());
    }
    void enclave_telemetry_service::on_service_proxy_add_ref(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options) const
    {
        on_service_proxy_add_ref_host(zone_id.get_val(),
            destination_zone_id.get_val(),
            caller_zone_id.get_val(),
            object_id.get_val(),
            known_direction_zone_id.get_val(),
            (uint64_t)options);
    }
    void enclave_telemetry_service::on_service_proxy_release(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::release_options options) const
    {
        on_service_proxy_release_host(zone_id.get_val(),
            destination_zone_id.get_val(),
            caller_zone_id.get_val(),
            object_id.get_val(),
            static_cast<uint64_t>(options));
    }

    void enclave_telemetry_service::on_impl_creation(const std::string& name, uint64_t address, rpc::zone zone_id) const
    {
        on_impl_creation_host(name, address, zone_id.get_val());
    }
    void enclave_telemetry_service::on_impl_deletion(uint64_t address, rpc::zone zone_id) const
    {
        on_impl_deletion_host(address, zone_id.get_val());
    }

    void enclave_telemetry_service::on_stub_creation(rpc::zone zone_id, rpc::object object_id, uint64_t address) const
    {
        on_stub_creation_host(zone_id.get_val(), object_id.get_val(), address);
    }
    void enclave_telemetry_service::on_stub_deletion(rpc::zone zone_id, rpc::object object_id) const
    {
        on_stub_deletion_host(zone_id.get_val(), object_id.get_val());
    }
    void enclave_telemetry_service::on_stub_send(
        rpc::zone zone_id, rpc::object object_id, rpc::interface_ordinal interface_id, rpc::method method_id) const
    {
        on_stub_send_host(zone_id.get_val(), object_id.get_val(), interface_id.get_val(), method_id.get_val());
    }
    void enclave_telemetry_service::on_stub_add_ref(rpc::zone zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        uint64_t count,
        rpc::caller_zone caller_zone_id) const
    {
        on_stub_add_ref_host(
            zone_id.get_val(), object_id.get_val(), interface_id.get_val(), count, caller_zone_id.get_val());
    }
    void enclave_telemetry_service::on_stub_release(rpc::zone zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        uint64_t count,
        rpc::caller_zone caller_zone_id) const
    {
        on_stub_release_host(
            zone_id.get_val(), object_id.get_val(), interface_id.get_val(), count, caller_zone_id.get_val());
    }

    void enclave_telemetry_service::on_object_proxy_creation(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::object object_id, bool add_ref_done) const
    {
        on_object_proxy_creation_host(zone_id.get_val(), destination_zone_id.get_val(), object_id.get_val(), add_ref_done);
    }
    void enclave_telemetry_service::on_object_proxy_deletion(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::object object_id) const
    {
        on_object_proxy_deletion_host(zone_id.get_val(), destination_zone_id.get_val(), object_id.get_val());
    }

    void enclave_telemetry_service::on_interface_proxy_creation(const std::string& name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        on_interface_proxy_creation_host(
            name, zone_id.get_val(), destination_zone_id.get_val(), object_id.get_val(), interface_id.get_val());
    }
    void enclave_telemetry_service::on_interface_proxy_deletion(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        on_interface_proxy_deletion_host(
            zone_id.get_val(), destination_zone_id.get_val(), object_id.get_val(), interface_id.get_val());
    }
    void enclave_telemetry_service::on_interface_proxy_send(const std::string& method_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        on_interface_proxy_send_host(method_name,
            zone_id.get_val(),
            destination_zone_id.get_val(),
            object_id.get_val(),
            interface_id.get_val(),
            method_id.get_val());
    }

    void enclave_telemetry_service::on_service_proxy_add_external_ref(
        rpc::zone operating_zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
    {
        on_service_proxy_add_external_ref_host(
            operating_zone_id.get_val(), destination_zone_id.get_val(), caller_zone_id.get_val());
    }

    void enclave_telemetry_service::on_service_proxy_release_external_ref(
        rpc::zone operating_zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
    {
        on_service_proxy_release_external_ref_host(
            operating_zone_id.get_val(), destination_zone_id.get_val(), caller_zone_id.get_val());
    }

    void enclave_telemetry_service::message(rpc::i_telemetry_service::level_enum level, const std::string& message) const
    {
        message_host(level, message);
    }

    void enclave_telemetry_service::on_transport_creation(
        const std::string& name, rpc::zone zone_id, rpc::zone adjacent_zone_id, rpc::transport_status status) const
    {
        on_transport_creation_host(name, zone_id.get_val(), adjacent_zone_id.get_val(), static_cast<uint32_t>(status));
    }

    void enclave_telemetry_service::on_transport_deletion(rpc::zone zone_id, rpc::zone adjacent_zone_id) const
    {
        on_transport_deletion_host(zone_id.get_val(), adjacent_zone_id.get_val());
    }

    void enclave_telemetry_service::on_transport_status_change(const std::string& name,
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::transport_status old_status,
        rpc::transport_status new_status) const
    {
        on_transport_status_change_host(name,
            zone_id.get_val(),
            adjacent_zone_id.get_val(),
            static_cast<uint32_t>(old_status),
            static_cast<uint32_t>(new_status));
    }

    void enclave_telemetry_service::on_transport_add_destination(
        rpc::zone zone_id, rpc::zone adjacent_zone_id, rpc::destination_zone destination, rpc::caller_zone caller) const
    {
        on_transport_add_destination_host(
            zone_id.get_val(), adjacent_zone_id.get_val(), destination.get_val(), caller.get_val());
    }

    void enclave_telemetry_service::on_transport_remove_destination(
        rpc::zone zone_id, rpc::zone adjacent_zone_id, rpc::destination_zone destination, rpc::caller_zone caller) const
    {
        on_transport_remove_destination_host(
            zone_id.get_val(), adjacent_zone_id.get_val(), destination.get_val(), caller.get_val());
    }

    void enclave_telemetry_service::on_transport_accept(rpc::zone zone_id, rpc::zone adjacent_zone_id, int result) const
    {
        on_transport_accept_host(zone_id.get_val(), adjacent_zone_id.get_val(), result);
    }

    void enclave_telemetry_service::on_transport_outbound_send(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;
        std::ignore = method_id;
    }

    void enclave_telemetry_service::on_transport_outbound_post(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;
        std::ignore = method_id;
    }

    void enclave_telemetry_service::on_transport_outbound_try_cast(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;
    }

    void enclave_telemetry_service::on_transport_outbound_add_ref(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = known_direction_zone_id;
        std::ignore = options;
    }

    void enclave_telemetry_service::on_transport_outbound_release(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::release_options options) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = options;
    }

    void enclave_telemetry_service::on_transport_outbound_object_released(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
    }

    void enclave_telemetry_service::on_transport_outbound_transport_down(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
    }

    void enclave_telemetry_service::on_transport_inbound_send(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;
        std::ignore = method_id;
    }

    void enclave_telemetry_service::on_transport_inbound_post(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;
        std::ignore = method_id;
    }

    void enclave_telemetry_service::on_transport_inbound_try_cast(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;
    }

    void enclave_telemetry_service::on_transport_inbound_add_ref(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = known_direction_zone_id;
        std::ignore = options;
    }

    void enclave_telemetry_service::on_transport_inbound_release(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::release_options options) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
        std::ignore = options;
    }

    void enclave_telemetry_service::on_transport_inbound_object_released(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        std::ignore = object_id;
    }

    void enclave_telemetry_service::on_transport_inbound_transport_down(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
    }

    void enclave_telemetry_service::on_pass_through_creation(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        uint64_t shared_count,
        uint64_t optimistic_count) const
    {
        on_pass_through_creation_host(
            zone_id.get_val(), forward_destination.get_val(), reverse_destination.get_val(), shared_count, optimistic_count);
    }

    void enclave_telemetry_service::on_pass_through_deletion(
        rpc::zone zone_id, rpc::destination_zone forward_destination, rpc::destination_zone reverse_destination) const
    {
        on_pass_through_deletion_host(zone_id.get_val(), forward_destination.get_val(), reverse_destination.get_val());
    }

    void enclave_telemetry_service::on_pass_through_add_ref(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        rpc::add_ref_options options,
        int64_t shared_delta,
        int64_t optimistic_delta) const
    {
        on_pass_through_add_ref_host(zone_id.get_val(),
            forward_destination.get_val(),
            reverse_destination.get_val(),
            static_cast<uint64_t>(options),
            shared_delta,
            optimistic_delta);
    }

    void enclave_telemetry_service::on_pass_through_release(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        int64_t shared_delta,
        int64_t optimistic_delta) const
    {
        on_pass_through_release_host(
            zone_id.get_val(), forward_destination.get_val(), reverse_destination.get_val(), shared_delta, optimistic_delta);
    }

    void enclave_telemetry_service::on_pass_through_status_change(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        rpc::transport_status forward_status,
        rpc::transport_status reverse_status) const
    {
        on_pass_through_status_change_host(zone_id.get_val(),
            forward_destination.get_val(),
            reverse_destination.get_val(),
            static_cast<uint32_t>(forward_status),
            static_cast<uint32_t>(reverse_status));
    }
}
