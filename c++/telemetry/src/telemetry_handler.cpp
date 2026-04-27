/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#include <unordered_map>

#include <rpc/rpc.h>
#include <rpc/telemetry/i_telemetry_service.h>
#include <rpc/telemetry/telemetry_handler.h>

#ifndef CANOPY_BUILD_COROUTINE
namespace
{
    rpc::zone make_zone(uint64_t subnet)
    {
        auto address = rpc::DEFAULT_PREFIX;
        std::ignore = address.set_subnet(subnet);
        return rpc::zone{address};
    }

    rpc::remote_object make_remote_object(
        uint64_t destination_zone_id,
        uint64_t object_id)
    {
        auto remote_object_id = make_zone(destination_zone_id).with_object({object_id});
        RPC_ASSERT(remote_object_id.has_value());
        return *remote_object_id;
    }
}

// an ocall for logging the test
extern "C"
{
    void on_service_creation_host(
        const std::string& name,
        uint64_t zone_id,
        uint64_t parent_zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_service_creation({name, make_zone(zone_id), make_zone(parent_zone_id)});
    }
    void on_service_deletion_host(uint64_t zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_service_deletion({make_zone(zone_id)});
    }
    void on_service_try_cast_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t caller_zone_id,
        uint64_t object_id,
        uint64_t interface_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
        {
            auto remote_object_id = make_remote_object(destination_zone_id, object_id);
            telemetry_service->on_service_try_cast(
                {make_zone(zone_id), remote_object_id, make_zone(caller_zone_id), {interface_id}});
        }
    }
    void on_service_add_ref_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t object_id,
        uint64_t caller_zone_id,
        uint64_t requesting_zone_id,
        uint64_t options)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
        {
            auto remote_object_id = make_remote_object(destination_zone_id, object_id);
            telemetry_service->on_service_add_ref(
                {make_zone(zone_id),
                    remote_object_id,
                    make_zone(caller_zone_id),
                    make_zone(requesting_zone_id),
                    (rpc::add_ref_options)options});
        }
    }
    void on_service_release_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t object_id,
        uint64_t caller_zone_id,
        uint64_t options)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
        {
            auto remote_object_id = make_remote_object(destination_zone_id, object_id);
            telemetry_service->on_service_release(
                {make_zone(zone_id), remote_object_id, make_zone(caller_zone_id), (rpc::release_options)options});
        }
    }

    void on_service_proxy_creation_host(
        const std::string& service_name,
        const std::string& service_proxy_name,
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t caller_zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_service_proxy_creation(
                {service_name, service_proxy_name, make_zone(zone_id), make_zone(destination_zone_id), make_zone(caller_zone_id)});
    }

    void on_cloned_service_proxy_creation_host(
        const std::string& service_name,
        const std::string& service_proxy_name,
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t caller_zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_cloned_service_proxy_creation(
                {service_name, service_proxy_name, make_zone(zone_id), make_zone(destination_zone_id), make_zone(caller_zone_id)});
    }
    void on_service_proxy_deletion_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t caller_zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_service_proxy_deletion(
                {make_zone(zone_id), make_zone(destination_zone_id), make_zone(caller_zone_id)});
    }
    void on_service_proxy_try_cast_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t caller_zone_id,
        uint64_t object_id,
        uint64_t interface_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
        {
            auto remote_object_id = make_remote_object(destination_zone_id, object_id);
            telemetry_service->on_service_proxy_try_cast(
                {make_zone(zone_id), remote_object_id, make_zone(caller_zone_id), {interface_id}});
        }
    }
    void on_service_proxy_add_ref_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t caller_zone_id,
        uint64_t object_id,
        uint64_t requesting_zone_id,
        uint64_t options)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
        {
            auto remote_object_id = make_remote_object(destination_zone_id, object_id);
            telemetry_service->on_service_proxy_add_ref(
                {make_zone(zone_id),
                    remote_object_id,
                    make_zone(caller_zone_id),
                    make_zone(requesting_zone_id),
                    (rpc::add_ref_options)options});
        }
    }
    void on_service_proxy_release_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t caller_zone_id,
        uint64_t object_id,
        uint64_t options)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
        {
            auto remote_object_id = make_remote_object(destination_zone_id, object_id);
            telemetry_service->on_service_proxy_release(
                {make_zone(zone_id), remote_object_id, make_zone(caller_zone_id), (rpc::release_options)options});
        }
    }

    void on_service_proxy_add_external_ref_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t caller_zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_service_proxy_add_external_ref(
                {make_zone(zone_id), make_zone(destination_zone_id), make_zone(caller_zone_id)});
    }

    void on_service_proxy_release_external_ref_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t caller_zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_service_proxy_release_external_ref(
                {make_zone(zone_id), make_zone(destination_zone_id), make_zone(caller_zone_id)});
    }

    void on_impl_creation_host(
        const std::string& name,
        uint64_t address,
        uint64_t zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_impl_creation({name, address, make_zone(zone_id)});
    }
    void on_impl_deletion_host(
        uint64_t address,
        uint64_t zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_impl_deletion({address, make_zone(zone_id)});
    }

    void on_stub_creation_host(
        uint64_t zone_id,
        uint64_t object_id,
        uint64_t address)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_stub_creation({make_zone(zone_id), {object_id}, address});
    }
    void on_stub_deletion_host(
        uint64_t zone_id,
        uint64_t object_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_stub_deletion({make_zone(zone_id), {object_id}});
    }
    void on_stub_send_host(
        uint64_t zone_id,
        uint64_t object_id,
        uint64_t interface_id,
        uint64_t method_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_stub_send({make_zone(zone_id), {object_id}, {interface_id}, {method_id}});
    }
    void on_stub_add_ref_host(
        uint64_t zone_id,
        uint64_t object_id,
        uint64_t interface_id,
        uint64_t count,
        uint64_t caller_zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_stub_add_ref(
                {make_zone(zone_id), {object_id}, {interface_id}, count, make_zone(caller_zone_id)});
    }
    void on_stub_release_host(
        uint64_t zone_id,
        uint64_t object_id,
        uint64_t interface_id,
        uint64_t count,
        uint64_t caller_zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_stub_release(
                {make_zone(zone_id), {object_id}, {interface_id}, count, make_zone(caller_zone_id)});
    }

    void on_object_proxy_creation_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t object_id,
        int add_ref_done)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_object_proxy_creation(
                {make_zone(zone_id), make_zone(destination_zone_id), {object_id}, !!add_ref_done});
    }

    void on_object_proxy_deletion_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t object_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_object_proxy_deletion({make_zone(zone_id), make_zone(destination_zone_id), {object_id}});
    }

    void on_interface_proxy_creation_host(
        const std::string& name,
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t object_id,
        uint64_t interface_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_interface_proxy_creation(
                {name, make_zone(zone_id), make_zone(destination_zone_id), {object_id}, {interface_id}});
    }
    void on_interface_proxy_deletion_host(
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t object_id,
        uint64_t interface_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_interface_proxy_deletion(
                {make_zone(zone_id), make_zone(destination_zone_id), {object_id}, {interface_id}});
    }
    void on_interface_proxy_send_host(
        const std::string& method_name,
        uint64_t zone_id,
        uint64_t destination_zone_id,
        uint64_t object_id,
        uint64_t interface_id,
        uint64_t method_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_interface_proxy_send(
                {method_name, make_zone(zone_id), make_zone(destination_zone_id), {object_id}, {interface_id}, {method_id}});
    }

    // New transport events
    void on_transport_creation_host(
        const std::string& name,
        uint64_t zone_id,
        uint64_t adjacent_zone_id,
        uint64_t status)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_transport_creation(
                {name, make_zone(zone_id), make_zone(adjacent_zone_id), (rpc::transport_status)status});
    }
    void on_transport_deletion_host(
        uint64_t zone_id,
        uint64_t adjacent_zone_id)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_transport_deletion({make_zone(zone_id), make_zone(adjacent_zone_id)});
    }
    void on_transport_status_change_host(
        const std::string& name,
        uint64_t zone_id,
        uint64_t adjacent_zone_id,
        uint64_t old_status,
        uint64_t new_status)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_transport_status_change(
                {name,
                    make_zone(zone_id),
                    make_zone(adjacent_zone_id),
                    (rpc::transport_status)old_status,
                    (rpc::transport_status)new_status});
    }
    void on_transport_add_destination_host(
        uint64_t zone_id,
        uint64_t adjacent_zone_id,
        uint64_t destination,
        uint64_t caller)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_transport_add_destination(
                {make_zone(zone_id), make_zone(adjacent_zone_id), make_zone(destination), make_zone(caller)});
    }
    void on_transport_remove_destination_host(
        uint64_t zone_id,
        uint64_t adjacent_zone_id,
        uint64_t destination,
        uint64_t caller)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_transport_remove_destination(
                {make_zone(zone_id), make_zone(adjacent_zone_id), make_zone(destination), make_zone(caller)});
    }
    void on_transport_accept_host(
        uint64_t zone_id,
        uint64_t adjacent_zone_id,
        int result)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_transport_accept({make_zone(zone_id), make_zone(adjacent_zone_id), result});
    }

    // New pass-through events
    void on_pass_through_creation_host(
        uint64_t zone_id,
        uint64_t forward_destination,
        uint64_t reverse_destination,
        uint64_t shared_count,
        uint64_t optimistic_count)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_pass_through_creation(
                {make_zone(zone_id), make_zone(forward_destination), make_zone(reverse_destination), shared_count, optimistic_count});
    }
    void on_pass_through_deletion_host(
        uint64_t zone_id,
        uint64_t forward_destination,
        uint64_t reverse_destination)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_pass_through_deletion(
                {make_zone(zone_id), make_zone(forward_destination), make_zone(reverse_destination)});
    }
    void on_pass_through_add_ref_host(
        uint64_t zone_id,
        uint64_t forward_destination,
        uint64_t reverse_destination,
        uint64_t options,
        int64_t shared_delta,
        int64_t optimistic_delta)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_pass_through_add_ref(
                {make_zone(zone_id),
                    make_zone(forward_destination),
                    make_zone(reverse_destination),
                    (rpc::add_ref_options)options,
                    shared_delta,
                    optimistic_delta});
    }
    void on_pass_through_release_host(
        uint64_t zone_id,
        uint64_t forward_destination,
        uint64_t reverse_destination,
        int64_t shared_delta,
        int64_t optimistic_delta)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_pass_through_release(
                {make_zone(zone_id), make_zone(forward_destination), make_zone(reverse_destination), shared_delta, optimistic_delta});
    }
    void on_pass_through_status_change_host(
        uint64_t zone_id,
        uint64_t forward_destination,
        uint64_t reverse_destination,
        uint64_t forward_status,
        uint64_t reverse_status)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->on_pass_through_status_change(
                {make_zone(zone_id),
                    make_zone(forward_destination),
                    make_zone(reverse_destination),
                    (rpc::transport_status)forward_status,
                    (rpc::transport_status)reverse_status});
    }

    void message_host(
        uint64_t level,
        const std::string& name)
    {
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service())
            telemetry_service->message({level, name});
    }
}
#endif
