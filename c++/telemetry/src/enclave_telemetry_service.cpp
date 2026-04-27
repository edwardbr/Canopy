/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#include <rpc/telemetry/telemetry_service_factory.h>

#include <sgx_error.h>

#include <rpc/telemetry/i_telemetry_service.h>

namespace rpc::telemetry
{
    class enclave_telemetry_service : public i_telemetry_service
    {
        enclave_telemetry_service();

    public:
        static bool create(std::shared_ptr<i_telemetry_service>& out)
        {
            out = std::shared_ptr<enclave_telemetry_service>(new enclave_telemetry_service());
            return true;
        }
        virtual ~enclave_telemetry_service() = default;

        void on_service_creation(const rpc::telemetry::service_creation_event& event) const override;
        void on_service_deletion(const rpc::telemetry::service_deletion_event& event) const override;
        void on_service_send(const rpc::telemetry::service_send_event& event) const override;
        void on_service_post(const rpc::telemetry::service_post_event& event) const override;
        void on_service_try_cast(const rpc::telemetry::service_try_cast_event& event) const override;
        void on_service_add_ref(const rpc::telemetry::service_add_ref_event& event) const override;
        void on_service_release(const rpc::telemetry::service_release_event& event) const override;
        void on_service_object_released(const rpc::telemetry::service_object_released_event& event) const override;
        void on_service_transport_down(const rpc::telemetry::service_transport_down_event& event) const override;
        void on_service_proxy_creation(const rpc::telemetry::service_proxy_creation_event& event) const override;
        void on_cloned_service_proxy_creation(
            const rpc::telemetry::cloned_service_proxy_creation_event& event) const override;
        void on_service_proxy_deletion(const rpc::telemetry::service_proxy_deletion_event& event) const override;
        void on_service_proxy_send(const rpc::telemetry::service_proxy_send_event& event) const override;
        void on_service_proxy_post(const rpc::telemetry::service_proxy_post_event& event) const override;
        void on_service_proxy_try_cast(const rpc::telemetry::service_proxy_try_cast_event& event) const override;
        void on_service_proxy_add_ref(const rpc::telemetry::service_proxy_add_ref_event& event) const override;
        void on_service_proxy_release(const rpc::telemetry::service_proxy_release_event& event) const override;
        void on_service_proxy_object_released(
            const rpc::telemetry::service_proxy_object_released_event& event) const override;
        void on_service_proxy_transport_down(const rpc::telemetry::service_proxy_transport_down_event& event) const override;
        void on_service_proxy_add_external_ref(const rpc::telemetry::service_proxy_external_ref_event& event) const override;
        void on_service_proxy_release_external_ref(
            const rpc::telemetry::service_proxy_external_ref_event& event) const override;
        void on_transport_creation(const rpc::telemetry::transport_creation_event& event) const override;
        void on_transport_deletion(const rpc::telemetry::transport_deletion_event& event) const override;
        void on_transport_status_change(const rpc::telemetry::transport_status_change_event& event) const override;
        void on_transport_add_destination(const rpc::telemetry::transport_destination_event& event) const override;
        void on_transport_remove_destination(const rpc::telemetry::transport_destination_event& event) const override;
        void on_transport_accept(const rpc::telemetry::transport_accept_event& event) const override;
        void on_transport_outbound_send(const rpc::telemetry::transport_send_event& event) const override;
        void on_transport_outbound_post(const rpc::telemetry::transport_post_event& event) const override;
        void on_transport_outbound_try_cast(const rpc::telemetry::transport_try_cast_event& event) const override;
        void on_transport_outbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const override;
        void on_transport_outbound_release(const rpc::telemetry::transport_release_event& event) const override;
        void on_transport_outbound_object_released(
            const rpc::telemetry::transport_object_released_event& event) const override;
        void on_transport_outbound_transport_down(const rpc::telemetry::transport_transport_down_event& event) const override;
        void on_transport_inbound_send(const rpc::telemetry::transport_send_event& event) const override;
        void on_transport_inbound_post(const rpc::telemetry::transport_post_event& event) const override;
        void on_transport_inbound_try_cast(const rpc::telemetry::transport_try_cast_event& event) const override;
        void on_transport_inbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const override;
        void on_transport_inbound_release(const rpc::telemetry::transport_release_event& event) const override;
        void on_transport_inbound_object_released(
            const rpc::telemetry::transport_object_released_event& event) const override;
        void on_transport_inbound_transport_down(const rpc::telemetry::transport_transport_down_event& event) const override;
        void on_impl_creation(const rpc::telemetry::impl_creation_event& event) const override;
        void on_impl_deletion(const rpc::telemetry::impl_deletion_event& event) const override;
        void on_stub_creation(const rpc::telemetry::stub_creation_event& event) const override;
        void on_stub_deletion(const rpc::telemetry::stub_deletion_event& event) const override;
        void on_stub_send(const rpc::telemetry::stub_send_event& event) const override;
        void on_stub_add_ref(const rpc::telemetry::stub_add_ref_event& event) const override;
        void on_stub_release(const rpc::telemetry::stub_release_event& event) const override;
        void on_object_proxy_creation(const rpc::telemetry::object_proxy_creation_event& event) const override;
        void on_object_proxy_deletion(const rpc::telemetry::object_proxy_deletion_event& event) const override;
        void on_interface_proxy_creation(const rpc::telemetry::interface_proxy_creation_event& event) const override;
        void on_interface_proxy_deletion(const rpc::telemetry::interface_proxy_deletion_event& event) const override;
        void on_interface_proxy_send(const rpc::telemetry::interface_proxy_send_event& event) const override;
        void on_pass_through_creation(const rpc::telemetry::pass_through_creation_event& event) const override;
        void on_pass_through_deletion(const rpc::telemetry::pass_through_deletion_event& event) const override;
        void on_pass_through_add_ref(const rpc::telemetry::pass_through_add_ref_event& event) const override;
        void on_pass_through_release(const rpc::telemetry::pass_through_release_event& event) const override;
        void on_pass_through_status_change(const rpc::telemetry::pass_through_status_change_event& event) const override;
        void message(const rpc::log_record& event) const override;
    };
}

namespace rpc::telemetry
{
    namespace
    {
#ifndef CANOPY_BUILD_COROUTINE
        extern "C"
        {
            sgx_status_t on_service_creation_host(
                const char* name,
                uint64_t zone_id,
                uint64_t parent_zone_id);
            sgx_status_t on_service_deletion_host(uint64_t zone_id);
            sgx_status_t on_service_proxy_creation_host(
                const char* service_name,
                const char* service_proxy_name,
                uint64_t zone_id,
                uint64_t destination_zone_id,
                uint64_t caller_zone_id);
            sgx_status_t on_service_proxy_deletion_host(
                uint64_t zone_id,
                uint64_t destination_zone_id,
                uint64_t caller_zone_id);
            sgx_status_t on_service_proxy_add_ref_host(
                uint64_t zone_id,
                uint64_t destination_zone_id,
                uint64_t object_id,
                uint64_t caller_zone_id,
                uint64_t requesting_zone_id,
                uint64_t options);
            sgx_status_t on_impl_creation_host(
                const char* name,
                uint64_t address,
                uint64_t zone_id);
            sgx_status_t on_impl_deletion_host(
                uint64_t address,
                uint64_t zone_id);
            sgx_status_t on_stub_creation_host(
                uint64_t zone_id,
                uint64_t object_id,
                uint64_t address);
            sgx_status_t on_stub_deletion_host(
                uint64_t zone_id,
                uint64_t object_id);
            sgx_status_t on_object_proxy_creation_host(
                uint64_t zone_id,
                uint64_t destination_zone_id,
                uint64_t object_id,
                int add_ref_done);
            sgx_status_t on_object_proxy_deletion_host(
                uint64_t zone_id,
                uint64_t destination_zone_id,
                uint64_t object_id);
            sgx_status_t on_interface_proxy_creation_host(
                const char* name,
                uint64_t zone_id,
                uint64_t destination_zone_id,
                uint64_t object_id,
                uint64_t interface_id);
            sgx_status_t on_interface_proxy_deletion_host(
                uint64_t zone_id,
                uint64_t destination_zone_id,
                uint64_t object_id,
                uint64_t interface_id);
            sgx_status_t on_interface_proxy_send_host(
                const char* method_name,
                uint64_t zone_id,
                uint64_t destination_zone_id,
                uint64_t object_id,
                uint64_t interface_id,
                uint64_t method_id);
            sgx_status_t on_service_add_ref_host(
                uint64_t zone_id,
                uint64_t destination_zone_id,
                uint64_t object_id,
                uint64_t caller_zone_id,
                uint64_t requesting_zone_id,
                uint64_t options);
            sgx_status_t on_transport_creation_host(
                const char* name,
                uint64_t zone_id,
                uint64_t adjacent_zone_id,
                uint32_t status);
            sgx_status_t on_transport_deletion_host(
                uint64_t zone_id,
                uint64_t adjacent_zone_id);
            sgx_status_t on_transport_status_change_host(
                const char* name,
                uint64_t zone_id,
                uint64_t adjacent_zone_id,
                uint32_t old_status,
                uint32_t new_status);
            sgx_status_t on_transport_add_destination_host(
                uint64_t zone_id,
                uint64_t adjacent_zone_id,
                uint64_t destination_zone_id,
                uint64_t caller_zone_id);
            sgx_status_t on_transport_outbound_add_ref_host(
                uint64_t zone_id,
                uint64_t adjacent_zone_id,
                uint64_t destination_zone_id,
                uint64_t object_id,
                uint64_t caller_zone_id,
                uint64_t requesting_zone_id,
                uint64_t options);
            sgx_status_t on_transport_inbound_add_ref_host(
                uint64_t zone_id,
                uint64_t adjacent_zone_id,
                uint64_t destination_zone_id,
                uint64_t object_id,
                uint64_t caller_zone_id,
                uint64_t requesting_zone_id,
                uint64_t options);
            sgx_status_t on_transport_remove_passthrough_host(
                uint64_t zone_id,
                uint64_t adjacent_zone_id,
                uint64_t destination_zone_id,
                uint64_t caller_zone_id);
            sgx_status_t message_host(
                uint64_t level,
                const char* message);
        }
#endif

        template<typename ZoneType> uint64_t zone_val(ZoneType zone_id)
        {
            return zone_id.get_subnet();
        }
        uint64_t object_val(rpc::object object_id)
        {
            return object_id.get_val();
        }
        template<typename OptionsType> uint32_t options_val(OptionsType options)
        {
            return static_cast<uint32_t>(options);
        }
        uint32_t status_val(rpc::transport_status status)
        {
            return static_cast<uint32_t>(status);
        }

        template<typename... Args> void ignore_event(const Args&...) { }
    }
    // Global telemetry service definition for enclave builds
#ifdef CANOPY_USE_TELEMETRY
    std::shared_ptr<i_telemetry_service> telemetry_service_ = nullptr;
#endif

    enclave_telemetry_service::enclave_telemetry_service() { }

    bool create_enclave_telemetry_service(std::shared_ptr<i_telemetry_service>& service)
    {
        return enclave_telemetry_service::create(service);
    }

    void enclave_telemetry_service::on_service_creation(const rpc::telemetry::service_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto parent_zone_id = event.parent_zone_id;
        std::ignore = on_service_creation_host(name.c_str(), zone_val(zone_id), zone_val(parent_zone_id));
        return;
    }

    void enclave_telemetry_service::on_service_deletion(const rpc::telemetry::service_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        std::ignore = on_service_deletion_host(zone_val(zone_id));
        return;
    }

    void enclave_telemetry_service::on_service_send(const rpc::telemetry::service_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id, method_id);
        return;
    }

    void enclave_telemetry_service::on_service_post(const rpc::telemetry::service_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id, method_id);
        return;
    }
    void enclave_telemetry_service::on_service_try_cast(const rpc::telemetry::service_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id);
        return;
    }

    void enclave_telemetry_service::on_service_add_ref(const rpc::telemetry::service_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        std::ignore = on_service_add_ref_host(
            zone_val(zone_id),
            zone_val(remote_object_id.as_zone()),
            object_val(remote_object_id.get_object_id()),
            zone_val(caller_zone_id),
            zone_val(requesting_zone_id),
            options_val(options));
        return;
    }

    void enclave_telemetry_service::on_service_release(const rpc::telemetry::service_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        ignore_event(zone_id, remote_object_id, caller_zone_id, options);
        return;
    }

    void enclave_telemetry_service::on_service_object_released(const rpc::telemetry::service_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        ignore_event(zone_id, remote_object_id, caller_zone_id);
        return;
    }

    void enclave_telemetry_service::on_service_transport_down(const rpc::telemetry::service_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        ignore_event(zone_id, destination_zone_id, caller_zone_id);
        return;
    }
    void enclave_telemetry_service::on_service_proxy_creation(const rpc::telemetry::service_proxy_creation_event& event) const
    {
        const auto& service_name = event.service_name;
        const auto& service_proxy_name = event.service_proxy_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = on_service_proxy_creation_host(
            service_name.c_str(),
            service_proxy_name.c_str(),
            zone_val(zone_id),
            zone_val(destination_zone_id),
            zone_val(caller_zone_id));
        return;
    }
    void enclave_telemetry_service::on_cloned_service_proxy_creation(
        const rpc::telemetry::cloned_service_proxy_creation_event& event) const
    {
        const auto& service_name = event.service_name;
        const auto& service_proxy_name = event.service_proxy_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        ignore_event(service_name, service_proxy_name, zone_id, destination_zone_id, caller_zone_id);
        return;
    }
    void enclave_telemetry_service::on_service_proxy_deletion(const rpc::telemetry::service_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore
            = on_service_proxy_deletion_host(zone_val(zone_id), zone_val(destination_zone_id), zone_val(caller_zone_id));
        return;
    }
    void enclave_telemetry_service::on_service_proxy_send(const rpc::telemetry::service_proxy_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id, method_id);
        return;
    }
    void enclave_telemetry_service::on_service_proxy_post(const rpc::telemetry::service_proxy_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id, method_id);
        return;
    }
    void enclave_telemetry_service::on_service_proxy_try_cast(const rpc::telemetry::service_proxy_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        ignore_event(zone_id, remote_object_id, caller_zone_id, interface_id);
        return;
    }
    void enclave_telemetry_service::on_service_proxy_add_ref(const rpc::telemetry::service_proxy_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        std::ignore = on_service_proxy_add_ref_host(
            zone_val(zone_id),
            zone_val(remote_object_id.as_zone()),
            object_val(remote_object_id.get_object_id()),
            zone_val(caller_zone_id),
            zone_val(requesting_zone_id),
            options_val(options));
        return;
    }
    void enclave_telemetry_service::on_service_proxy_release(const rpc::telemetry::service_proxy_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        ignore_event(zone_id, remote_object_id, caller_zone_id, options);
        return;
    }

    void enclave_telemetry_service::on_service_proxy_object_released(
        const rpc::telemetry::service_proxy_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        ignore_event(zone_id, remote_object_id, caller_zone_id);
        return;
    }

    void enclave_telemetry_service::on_service_proxy_transport_down(
        const rpc::telemetry::service_proxy_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        ignore_event(zone_id, destination_zone_id, caller_zone_id);
        return;
    }

    void enclave_telemetry_service::on_impl_creation(const rpc::telemetry::impl_creation_event& event) const
    {
        const auto& name = event.name;
        auto address = event.address;
        auto zone_id = event.zone_id;
        std::ignore = on_impl_creation_host(name.c_str(), address, zone_val(zone_id));
        return;
    }
    void enclave_telemetry_service::on_impl_deletion(const rpc::telemetry::impl_deletion_event& event) const
    {
        auto address = event.address;
        auto zone_id = event.zone_id;
        std::ignore = on_impl_deletion_host(address, zone_val(zone_id));
        return;
    }

    void enclave_telemetry_service::on_stub_creation(const rpc::telemetry::stub_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        auto address = event.address;
        std::ignore = on_stub_creation_host(zone_val(zone_id), object_val(object_id), address);
        return;
    }
    void enclave_telemetry_service::on_stub_deletion(const rpc::telemetry::stub_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        std::ignore = on_stub_deletion_host(zone_val(zone_id), object_val(object_id));
        return;
    }
    void enclave_telemetry_service::on_stub_send(const rpc::telemetry::stub_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        ignore_event(zone_id, object_id, interface_id, method_id);
        return;
    }
    void enclave_telemetry_service::on_stub_add_ref(const rpc::telemetry::stub_add_ref_event& event) const
    {
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto count = event.count;
        auto caller_zone_id = event.caller_zone_id;
        ignore_event(destination_zone_id, object_id, interface_id, count, caller_zone_id);
        return;
    }
    void enclave_telemetry_service::on_stub_release(const rpc::telemetry::stub_release_event& event) const
    {
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto count = event.count;
        auto caller_zone_id = event.caller_zone_id;
        ignore_event(destination_zone_id, object_id, interface_id, count, caller_zone_id);
        return;
    }

    void enclave_telemetry_service::on_object_proxy_creation(const rpc::telemetry::object_proxy_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto add_ref_done = event.add_ref_done;
        std::ignore = on_object_proxy_creation_host(
            zone_val(zone_id), zone_val(destination_zone_id), object_val(object_id), add_ref_done ? 1 : 0);
        return;
    }
    void enclave_telemetry_service::on_object_proxy_deletion(const rpc::telemetry::object_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        std::ignore
            = on_object_proxy_deletion_host(zone_val(zone_id), zone_val(destination_zone_id), object_val(object_id));
        return;
    }

    void enclave_telemetry_service::on_interface_proxy_creation(
        const rpc::telemetry::interface_proxy_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        std::ignore = on_interface_proxy_creation_host(
            name.c_str(), zone_val(zone_id), zone_val(destination_zone_id), object_val(object_id), interface_id.get_val());
        return;
    }
    void enclave_telemetry_service::on_interface_proxy_deletion(
        const rpc::telemetry::interface_proxy_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        std::ignore = on_interface_proxy_deletion_host(
            zone_val(zone_id), zone_val(destination_zone_id), object_val(object_id), interface_id.get_val());
        return;
    }
    void enclave_telemetry_service::on_interface_proxy_send(const rpc::telemetry::interface_proxy_send_event& event) const
    {
        const auto& method_name = event.method_name;
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto object_id = event.object_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        std::ignore = on_interface_proxy_send_host(
            method_name.c_str(),
            zone_val(zone_id),
            zone_val(destination_zone_id),
            object_val(object_id),
            interface_id.get_val(),
            method_id.get_val());
        return;
    }

    void enclave_telemetry_service::on_service_proxy_add_external_ref(
        const rpc::telemetry::service_proxy_external_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        ignore_event(zone_id, destination_zone_id, caller_zone_id);
        return;
    }

    void enclave_telemetry_service::on_service_proxy_release_external_ref(
        const rpc::telemetry::service_proxy_external_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        ignore_event(zone_id, destination_zone_id, caller_zone_id);
        return;
    }

    void enclave_telemetry_service::message(const rpc::log_record& event) const
    {
        auto level = static_cast<level_enum>(event.level);
        const auto& message = event.message;
        std::ignore = message_host(options_val(level), message.c_str());
        return;
    }

    void enclave_telemetry_service::on_transport_creation(const rpc::telemetry::transport_creation_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto status = event.status;
        std::ignore
            = on_transport_creation_host(name.c_str(), zone_val(zone_id), zone_val(adjacent_zone_id), status_val(status));
        return;
    }

    void enclave_telemetry_service::on_transport_deletion(const rpc::telemetry::transport_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        std::ignore = on_transport_deletion_host(zone_val(zone_id), zone_val(adjacent_zone_id));
        return;
    }

    void enclave_telemetry_service::on_transport_status_change(const rpc::telemetry::transport_status_change_event& event) const
    {
        const auto& name = event.name;
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto old_status = event.old_status;
        auto new_status = event.new_status;
        std::ignore = on_transport_status_change_host(
            name.c_str(), zone_val(zone_id), zone_val(adjacent_zone_id), status_val(old_status), status_val(new_status));
        return;
    }

    void enclave_telemetry_service::on_transport_add_destination(const rpc::telemetry::transport_destination_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination = event.destination;
        auto caller = event.caller;
        std::ignore = on_transport_add_destination_host(
            zone_val(zone_id), zone_val(adjacent_zone_id), zone_val(destination), zone_val(caller));
        return;
    }

    void enclave_telemetry_service::on_transport_remove_destination(
        const rpc::telemetry::transport_destination_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination = event.destination;
        auto caller = event.caller;
        std::ignore = on_transport_remove_passthrough_host(
            zone_val(zone_id), zone_val(adjacent_zone_id), zone_val(destination), zone_val(caller));
        return;
    }

    void enclave_telemetry_service::on_transport_accept(const rpc::telemetry::transport_accept_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto result = event.result;
        ignore_event(zone_id, adjacent_zone_id, result);
        return;
    }

    void enclave_telemetry_service::on_transport_outbound_send(const rpc::telemetry::transport_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
        std::ignore = method_id;
        return;
    }

    void enclave_telemetry_service::on_transport_outbound_post(const rpc::telemetry::transport_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
        std::ignore = method_id;
        return;
    }

    void enclave_telemetry_service::on_transport_outbound_try_cast(const rpc::telemetry::transport_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
        return;
    }

    void enclave_telemetry_service::on_transport_outbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        std::ignore = on_transport_outbound_add_ref_host(
            zone_val(zone_id),
            zone_val(adjacent_zone_id),
            zone_val(remote_object_id.as_zone()),
            object_val(remote_object_id.get_object_id()),
            zone_val(caller_zone_id),
            zone_val(requesting_zone_id),
            options_val(options));
        return;
    }

    void enclave_telemetry_service::on_transport_outbound_release(const rpc::telemetry::transport_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = options;
        return;
    }

    void enclave_telemetry_service::on_transport_outbound_object_released(
        const rpc::telemetry::transport_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        return;
    }

    void enclave_telemetry_service::on_transport_outbound_transport_down(
        const rpc::telemetry::transport_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        return;
    }

    void enclave_telemetry_service::on_transport_inbound_send(const rpc::telemetry::transport_send_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
        std::ignore = method_id;
        return;
    }

    void enclave_telemetry_service::on_transport_inbound_post(const rpc::telemetry::transport_post_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        auto method_id = event.method_id;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
        std::ignore = method_id;
        return;
    }

    void enclave_telemetry_service::on_transport_inbound_try_cast(const rpc::telemetry::transport_try_cast_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto interface_id = event.interface_id;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = interface_id;
        return;
    }

    void enclave_telemetry_service::on_transport_inbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto requesting_zone_id = event.requesting_zone_id;
        auto options = event.options;
        std::ignore = on_transport_inbound_add_ref_host(
            zone_val(zone_id),
            zone_val(adjacent_zone_id),
            zone_val(remote_object_id.as_zone()),
            object_val(remote_object_id.get_object_id()),
            zone_val(caller_zone_id),
            zone_val(requesting_zone_id),
            options_val(options));
        return;
    }

    void enclave_telemetry_service::on_transport_inbound_release(const rpc::telemetry::transport_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        auto options = event.options;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        std::ignore = options;
        return;
    }

    void enclave_telemetry_service::on_transport_inbound_object_released(
        const rpc::telemetry::transport_object_released_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto remote_object_id = event.remote_object_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = remote_object_id;
        std::ignore = caller_zone_id;
        return;
    }

    void enclave_telemetry_service::on_transport_inbound_transport_down(
        const rpc::telemetry::transport_transport_down_event& event) const
    {
        auto zone_id = event.zone_id;
        auto adjacent_zone_id = event.adjacent_zone_id;
        auto destination_zone_id = event.destination_zone_id;
        auto caller_zone_id = event.caller_zone_id;
        std::ignore = zone_id;
        std::ignore = adjacent_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = caller_zone_id;
        return;
    }

    void enclave_telemetry_service::on_pass_through_creation(const rpc::telemetry::pass_through_creation_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto shared_count = event.shared_count;
        auto optimistic_count = event.optimistic_count;
        ignore_event(zone_id, forward_destination, reverse_destination, shared_count, optimistic_count);
        return;
    }

    void enclave_telemetry_service::on_pass_through_deletion(const rpc::telemetry::pass_through_deletion_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        ignore_event(zone_id, forward_destination, reverse_destination);
        return;
    }

    void enclave_telemetry_service::on_pass_through_add_ref(const rpc::telemetry::pass_through_add_ref_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto options = event.options;
        auto shared_delta = event.shared_delta;
        auto optimistic_delta = event.optimistic_delta;
        ignore_event(zone_id, forward_destination, reverse_destination, options, shared_delta, optimistic_delta);
        return;
    }

    void enclave_telemetry_service::on_pass_through_release(const rpc::telemetry::pass_through_release_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto shared_delta = event.shared_delta;
        auto optimistic_delta = event.optimistic_delta;
        ignore_event(zone_id, forward_destination, reverse_destination, shared_delta, optimistic_delta);
        return;
    }

    void enclave_telemetry_service::on_pass_through_status_change(
        const rpc::telemetry::pass_through_status_change_event& event) const
    {
        auto zone_id = event.zone_id;
        auto forward_destination = event.forward_destination;
        auto reverse_destination = event.reverse_destination;
        auto forward_status = event.forward_status;
        auto reverse_status = event.reverse_status;
        ignore_event(zone_id, forward_destination, reverse_destination, forward_status, reverse_status);
        return;
    }
}
