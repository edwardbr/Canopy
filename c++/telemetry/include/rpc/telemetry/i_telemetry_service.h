/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <rpc/internal/build_modifiers.h>
#include <rpc/internal/polyfill/int128.h>
#include <rpc/internal/polyfill/expected.h>
#include <rpc/internal/version.h>
#include <rpc/logging.h>
#include <rpc/telemetry_types.h>

#ifndef FOR_SGX
#  include <filesystem>
#endif

// copied from spdlog
#define I_TELEMETRY_LEVEL_DEBUG 0
#define I_TELEMETRY_LEVEL_TRACE 1
#define I_TELEMETRY_LEVEL_INFO 2
#define I_TELEMETRY_LEVEL_WARN 3
#define I_TELEMETRY_LEVEL_ERROR 4
#define I_TELEMETRY_LEVEL_CRITICAL 5
#define I_TELEMETRY_LEVEL_OFF 6

namespace rpc::telemetry
{
    class i_telemetry_service
    {
    public:
        enum level_enum
        {
            debug = I_TELEMETRY_LEVEL_DEBUG,
            trace = I_TELEMETRY_LEVEL_TRACE,
            info = I_TELEMETRY_LEVEL_INFO,
            warn = I_TELEMETRY_LEVEL_WARN,
            err = I_TELEMETRY_LEVEL_ERROR,
            critical = I_TELEMETRY_LEVEL_CRITICAL,
            off = I_TELEMETRY_LEVEL_OFF,
            n_levels
        };
        virtual ~i_telemetry_service() = default;

        virtual void handle_telemetry_event(rpc::telemetry_event event) const
        {
            std::ignore = event;
            return;
        }

        // telemetry event methods
        virtual void on_service_creation(const rpc::telemetry::service_creation_event& event) const = 0;
        virtual void on_service_deletion(const rpc::telemetry::service_deletion_event& event) const = 0;
        virtual void on_service_send(const rpc::telemetry::service_send_event& event) const = 0;
        virtual void on_service_post(const rpc::telemetry::service_post_event& event) const = 0;
        virtual void on_service_try_cast(const rpc::telemetry::service_try_cast_event& event) const = 0;
        virtual void on_service_add_ref(const rpc::telemetry::service_add_ref_event& event) const = 0;
        virtual void on_service_release(const rpc::telemetry::service_release_event& event) const = 0;
        virtual void on_service_object_released(const rpc::telemetry::service_object_released_event& event) const = 0;
        virtual void on_service_transport_down(const rpc::telemetry::service_transport_down_event& event) const = 0;
        virtual void on_service_proxy_creation(const rpc::telemetry::service_proxy_creation_event& event) const = 0;
        virtual void on_cloned_service_proxy_creation(
            const rpc::telemetry::cloned_service_proxy_creation_event& event) const = 0;
        virtual void on_service_proxy_deletion(const rpc::telemetry::service_proxy_deletion_event& event) const = 0;
        virtual void on_service_proxy_send(const rpc::telemetry::service_proxy_send_event& event) const = 0;
        virtual void on_service_proxy_post(const rpc::telemetry::service_proxy_post_event& event) const = 0;
        virtual void on_service_proxy_try_cast(const rpc::telemetry::service_proxy_try_cast_event& event) const = 0;
        virtual void on_service_proxy_add_ref(const rpc::telemetry::service_proxy_add_ref_event& event) const = 0;
        virtual void on_service_proxy_release(const rpc::telemetry::service_proxy_release_event& event) const = 0;
        virtual void on_service_proxy_object_released(
            const rpc::telemetry::service_proxy_object_released_event& event) const = 0;
        virtual void on_service_proxy_transport_down(const rpc::telemetry::service_proxy_transport_down_event& event) const
            = 0;
        virtual void on_service_proxy_add_external_ref(const rpc::telemetry::service_proxy_external_ref_event& event) const
            = 0;
        virtual void on_service_proxy_release_external_ref(
            const rpc::telemetry::service_proxy_external_ref_event& event) const = 0;
        virtual void on_transport_creation(const rpc::telemetry::transport_creation_event& event) const = 0;
        virtual void on_transport_deletion(const rpc::telemetry::transport_deletion_event& event) const = 0;
        virtual void on_transport_status_change(const rpc::telemetry::transport_status_change_event& event) const = 0;
        virtual void on_transport_add_destination(const rpc::telemetry::transport_destination_event& event) const = 0;
        virtual void on_transport_remove_destination(const rpc::telemetry::transport_destination_event& event) const = 0;
        virtual void on_transport_accept(const rpc::telemetry::transport_accept_event& event) const = 0;
        virtual void on_transport_outbound_send(const rpc::telemetry::transport_send_event& event) const = 0;
        virtual void on_transport_outbound_post(const rpc::telemetry::transport_post_event& event) const = 0;
        virtual void on_transport_outbound_try_cast(const rpc::telemetry::transport_try_cast_event& event) const = 0;
        virtual void on_transport_outbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const = 0;
        virtual void on_transport_outbound_release(const rpc::telemetry::transport_release_event& event) const = 0;
        virtual void on_transport_outbound_object_released(
            const rpc::telemetry::transport_object_released_event& event) const = 0;
        virtual void on_transport_outbound_transport_down(
            const rpc::telemetry::transport_transport_down_event& event) const = 0;
        virtual void on_transport_inbound_send(const rpc::telemetry::transport_send_event& event) const = 0;
        virtual void on_transport_inbound_post(const rpc::telemetry::transport_post_event& event) const = 0;
        virtual void on_transport_inbound_try_cast(const rpc::telemetry::transport_try_cast_event& event) const = 0;
        virtual void on_transport_inbound_add_ref(const rpc::telemetry::transport_add_ref_event& event) const = 0;
        virtual void on_transport_inbound_release(const rpc::telemetry::transport_release_event& event) const = 0;
        virtual void on_transport_inbound_object_released(
            const rpc::telemetry::transport_object_released_event& event) const = 0;
        virtual void on_transport_inbound_transport_down(const rpc::telemetry::transport_transport_down_event& event) const
            = 0;
        virtual void on_impl_creation(const rpc::telemetry::impl_creation_event& event) const = 0;
        virtual void on_impl_deletion(const rpc::telemetry::impl_deletion_event& event) const = 0;
        virtual void on_stub_creation(const rpc::telemetry::stub_creation_event& event) const = 0;
        virtual void on_stub_deletion(const rpc::telemetry::stub_deletion_event& event) const = 0;
        virtual void on_stub_send(const rpc::telemetry::stub_send_event& event) const = 0;
        virtual void on_stub_add_ref(const rpc::telemetry::stub_add_ref_event& event) const = 0;
        virtual void on_stub_release(const rpc::telemetry::stub_release_event& event) const = 0;
        virtual void on_object_proxy_creation(const rpc::telemetry::object_proxy_creation_event& event) const = 0;
        virtual void on_object_proxy_deletion(const rpc::telemetry::object_proxy_deletion_event& event) const = 0;
        virtual void on_interface_proxy_creation(const rpc::telemetry::interface_proxy_creation_event& event) const = 0;
        virtual void on_interface_proxy_deletion(const rpc::telemetry::interface_proxy_deletion_event& event) const = 0;
        virtual void on_interface_proxy_send(const rpc::telemetry::interface_proxy_send_event& event) const = 0;
        virtual void on_pass_through_creation(const rpc::telemetry::pass_through_creation_event& event) const = 0;
        virtual void on_pass_through_deletion(const rpc::telemetry::pass_through_deletion_event& event) const = 0;
        virtual void on_pass_through_add_ref(const rpc::telemetry::pass_through_add_ref_event& event) const = 0;
        virtual void on_pass_through_release(const rpc::telemetry::pass_through_release_event& event) const = 0;
        virtual void on_pass_through_status_change(const rpc::telemetry::pass_through_status_change_event& event) const
            = 0;
        virtual void message(const rpc::log_record& event) const = 0;

        void on_service_creation(const std::string& name, rpc::zone zone_id, rpc::destination_zone parent_zone_id) const
        {
            on_service_creation({name, zone_id, parent_zone_id});
        }
        void on_service_deletion(rpc::zone zone_id) const
        {
            on_service_deletion(rpc::telemetry::service_deletion_event{zone_id});
        }
        void on_service_send(rpc::zone zone_id, rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id,
            rpc::interface_ordinal interface_id, rpc::method method_id) const
        {
            on_service_send({zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
        }
        void on_service_post(rpc::zone zone_id, rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id,
            rpc::interface_ordinal interface_id, rpc::method method_id) const
        {
            on_service_post({zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
        }
        void on_service_try_cast(rpc::zone zone_id, rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id,
            rpc::interface_ordinal interface_id) const
        {
            on_service_try_cast({zone_id, remote_object_id, caller_zone_id, interface_id});
        }
        void on_service_add_ref(rpc::zone zone_id, rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id,
            rpc::requesting_zone requesting_zone_id, rpc::add_ref_options options) const
        {
            on_service_add_ref({zone_id, remote_object_id, caller_zone_id, requesting_zone_id, options});
        }
        void on_service_release(rpc::zone zone_id, rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id,
            rpc::release_options options) const
        {
            on_service_release({zone_id, remote_object_id, caller_zone_id, options});
        }
        void on_service_object_released(
            rpc::zone zone_id, rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id) const
        {
            on_service_object_released({zone_id, remote_object_id, caller_zone_id});
        }
        void on_service_transport_down(
            rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
        {
            on_service_transport_down({zone_id, destination_zone_id, caller_zone_id});
        }

        void on_service_proxy_creation(const std::string& service_name, const std::string& service_proxy_name,
            rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
        {
            on_service_proxy_creation(
                {service_name, service_proxy_name, zone_id, destination_zone_id, caller_zone_id});
        }
        void on_cloned_service_proxy_creation(const std::string& service_name, const std::string& service_proxy_name,
            rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
        {
            on_cloned_service_proxy_creation(
                {service_name, service_proxy_name, zone_id, destination_zone_id, caller_zone_id});
        }
        void on_service_proxy_deletion(
            rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
        {
            on_service_proxy_deletion({zone_id, destination_zone_id, caller_zone_id});
        }
        void on_service_proxy_send(rpc::zone zone_id, rpc::remote_object remote_object_id,
            rpc::caller_zone caller_zone_id, rpc::interface_ordinal interface_id, rpc::method method_id) const
        {
            on_service_proxy_send({zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
        }
        void on_service_proxy_post(rpc::zone zone_id, rpc::remote_object remote_object_id,
            rpc::caller_zone caller_zone_id, rpc::interface_ordinal interface_id, rpc::method method_id) const
        {
            on_service_proxy_post({zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
        }
        void on_service_proxy_try_cast(rpc::zone zone_id, rpc::remote_object remote_object_id,
            rpc::caller_zone caller_zone_id, rpc::interface_ordinal interface_id) const
        {
            on_service_proxy_try_cast({zone_id, remote_object_id, caller_zone_id, interface_id});
        }
        void on_service_proxy_add_ref(rpc::zone zone_id, rpc::remote_object remote_object_id,
            rpc::caller_zone caller_zone_id, rpc::requesting_zone requesting_zone_id, rpc::add_ref_options options) const
        {
            on_service_proxy_add_ref({zone_id, remote_object_id, caller_zone_id, requesting_zone_id, options});
        }
        void on_service_proxy_release(rpc::zone zone_id, rpc::remote_object remote_object_id,
            rpc::caller_zone caller_zone_id, rpc::release_options options) const
        {
            on_service_proxy_release({zone_id, remote_object_id, caller_zone_id, options});
        }
        void on_service_proxy_object_released(
            rpc::zone zone_id, rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id) const
        {
            on_service_proxy_object_released({zone_id, remote_object_id, caller_zone_id});
        }
        void on_service_proxy_transport_down(
            rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
        {
            on_service_proxy_transport_down({zone_id, destination_zone_id, caller_zone_id});
        }
        void on_service_proxy_add_external_ref(
            rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
        {
            on_service_proxy_add_external_ref({zone_id, destination_zone_id, caller_zone_id});
        }
        void on_service_proxy_release_external_ref(
            rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
        {
            on_service_proxy_release_external_ref({zone_id, destination_zone_id, caller_zone_id});
        }

        void on_transport_creation(
            const std::string& name, rpc::zone zone_id, rpc::zone adjacent_zone_id, rpc::transport_status status) const
        {
            on_transport_creation({name, zone_id, adjacent_zone_id, status});
        }
        void on_transport_deletion(rpc::zone zone_id, rpc::zone adjacent_zone_id) const
        {
            on_transport_deletion(rpc::telemetry::transport_deletion_event{zone_id, adjacent_zone_id});
        }
        void on_transport_status_change(const std::string& name, rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::transport_status old_status, rpc::transport_status new_status) const
        {
            on_transport_status_change({name, zone_id, adjacent_zone_id, old_status, new_status});
        }
        void on_transport_add_destination(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::destination_zone destination, rpc::caller_zone caller) const
        {
            on_transport_add_destination({zone_id, adjacent_zone_id, destination, caller});
        }
        void on_transport_remove_destination(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::destination_zone destination, rpc::caller_zone caller) const
        {
            on_transport_remove_destination({zone_id, adjacent_zone_id, destination, caller});
        }
        void on_transport_accept(rpc::zone zone_id, rpc::zone adjacent_zone_id, int result) const
        {
            on_transport_accept({zone_id, adjacent_zone_id, result});
        }
        void on_transport_outbound_send(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id, rpc::interface_ordinal interface_id,
            rpc::method method_id) const
        {
            on_transport_outbound_send(
                {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
        }
        void on_transport_outbound_post(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id, rpc::interface_ordinal interface_id,
            rpc::method method_id) const
        {
            on_transport_outbound_post(
                {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
        }
        void on_transport_outbound_try_cast(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id, rpc::interface_ordinal interface_id) const
        {
            on_transport_outbound_try_cast({zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id});
        }
        void on_transport_outbound_add_ref(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id,
            rpc::requesting_zone requesting_zone_id, rpc::add_ref_options options) const
        {
            on_transport_outbound_add_ref(
                {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, requesting_zone_id, options});
        }
        void on_transport_outbound_release(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id, rpc::release_options options) const
        {
            on_transport_outbound_release(
                {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, options});
        }
        void on_transport_outbound_object_released(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id) const
        {
            on_transport_outbound_object_released({zone_id, adjacent_zone_id, remote_object_id, caller_zone_id});
        }
        void on_transport_outbound_transport_down(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
        {
            on_transport_outbound_transport_down({zone_id, adjacent_zone_id, destination_zone_id, caller_zone_id});
        }
        void on_transport_inbound_send(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id, rpc::interface_ordinal interface_id,
            rpc::method method_id) const
        {
            on_transport_inbound_send(
                {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
        }
        void on_transport_inbound_post(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id, rpc::interface_ordinal interface_id,
            rpc::method method_id) const
        {
            on_transport_inbound_post(
                {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id, method_id});
        }
        void on_transport_inbound_try_cast(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id, rpc::interface_ordinal interface_id) const
        {
            on_transport_inbound_try_cast({zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, interface_id});
        }
        void on_transport_inbound_add_ref(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id,
            rpc::requesting_zone requesting_zone_id, rpc::add_ref_options options) const
        {
            on_transport_inbound_add_ref(
                {zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, requesting_zone_id, options});
        }
        void on_transport_inbound_release(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id, rpc::release_options options) const
        {
            on_transport_inbound_release({zone_id, adjacent_zone_id, remote_object_id, caller_zone_id, options});
        }
        void on_transport_inbound_object_released(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::remote_object remote_object_id, rpc::caller_zone caller_zone_id) const
        {
            on_transport_inbound_object_released({zone_id, adjacent_zone_id, remote_object_id, caller_zone_id});
        }
        void on_transport_inbound_transport_down(rpc::zone zone_id, rpc::zone adjacent_zone_id,
            rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
        {
            on_transport_inbound_transport_down({zone_id, adjacent_zone_id, destination_zone_id, caller_zone_id});
        }

        void on_impl_creation(const std::string& name, uint64_t address, rpc::zone zone_id) const
        {
            on_impl_creation({name, address, zone_id});
        }
        void on_impl_deletion(uint64_t address, rpc::zone zone_id) const
        {
            on_impl_deletion(rpc::telemetry::impl_deletion_event{address, zone_id});
        }
        void on_stub_creation(rpc::zone zone_id, rpc::object object_id, uint64_t address) const
        {
            on_stub_creation({zone_id, object_id, address});
        }
        void on_stub_deletion(rpc::zone zone_id, rpc::object object_id) const
        {
            on_stub_deletion(rpc::telemetry::stub_deletion_event{zone_id, object_id});
        }
        void on_stub_send(
            rpc::zone zone_id, rpc::object object_id, rpc::interface_ordinal interface_id, rpc::method method_id) const
        {
            on_stub_send({zone_id, object_id, interface_id, method_id});
        }
        void on_stub_add_ref(rpc::zone destination_zone_id, rpc::object object_id,
            rpc::interface_ordinal interface_id, uint64_t count, rpc::caller_zone caller_zone_id) const
        {
            on_stub_add_ref({destination_zone_id, object_id, interface_id, count, caller_zone_id});
        }
        void on_stub_release(rpc::zone destination_zone_id, rpc::object object_id,
            rpc::interface_ordinal interface_id, uint64_t count, rpc::caller_zone caller_zone_id) const
        {
            on_stub_release({destination_zone_id, object_id, interface_id, count, caller_zone_id});
        }
        void on_object_proxy_creation(
            rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::object object_id, bool add_ref_done) const
        {
            on_object_proxy_creation({zone_id, destination_zone_id, object_id, add_ref_done});
        }
        void on_object_proxy_deletion(
            rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::object object_id) const
        {
            on_object_proxy_deletion({zone_id, destination_zone_id, object_id});
        }
        void on_interface_proxy_creation(const std::string& name, rpc::zone zone_id,
            rpc::destination_zone destination_zone_id, rpc::object object_id, rpc::interface_ordinal interface_id) const
        {
            on_interface_proxy_creation({name, zone_id, destination_zone_id, object_id, interface_id});
        }
        void on_interface_proxy_deletion(rpc::zone zone_id, rpc::destination_zone destination_zone_id,
            rpc::object object_id, rpc::interface_ordinal interface_id) const
        {
            on_interface_proxy_deletion({zone_id, destination_zone_id, object_id, interface_id});
        }
        void on_interface_proxy_send(const std::string& method_name, rpc::zone zone_id,
            rpc::destination_zone destination_zone_id, rpc::object object_id, rpc::interface_ordinal interface_id,
            rpc::method method_id) const
        {
            on_interface_proxy_send({method_name, zone_id, destination_zone_id, object_id, interface_id, method_id});
        }

        void on_pass_through_creation(rpc::zone zone_id, rpc::destination_zone forward_destination,
            rpc::destination_zone reverse_destination, uint64_t shared_count, uint64_t optimistic_count) const
        {
            on_pass_through_creation(
                {zone_id, forward_destination, reverse_destination, shared_count, optimistic_count});
        }
        void on_pass_through_deletion(rpc::zone zone_id, rpc::destination_zone forward_destination,
            rpc::destination_zone reverse_destination) const
        {
            on_pass_through_deletion({zone_id, forward_destination, reverse_destination});
        }
        void on_pass_through_add_ref(rpc::zone zone_id, rpc::destination_zone forward_destination,
            rpc::destination_zone reverse_destination, rpc::add_ref_options options, int64_t shared_delta,
            int64_t optimistic_delta) const
        {
            on_pass_through_add_ref(
                {zone_id, forward_destination, reverse_destination, options, shared_delta, optimistic_delta});
        }
        void on_pass_through_release(rpc::zone zone_id, rpc::destination_zone forward_destination,
            rpc::destination_zone reverse_destination, int64_t shared_delta, int64_t optimistic_delta) const
        {
            on_pass_through_release(
                {zone_id, forward_destination, reverse_destination, shared_delta, optimistic_delta});
        }
        void on_pass_through_status_change(rpc::zone zone_id, rpc::destination_zone forward_destination,
            rpc::destination_zone reverse_destination, rpc::transport_status forward_status,
            rpc::transport_status reverse_status) const
        {
            on_pass_through_status_change(
                {zone_id, forward_destination, reverse_destination, forward_status, reverse_status});
        }
        void message(level_enum level, const std::string& text) const
        {
            message({static_cast<uint64_t>(level), text});
        }
    };

    // Global telemetry service - defined in host or enclave telemetry runtime code.
    extern std::shared_ptr<i_telemetry_service> telemetry_service_; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

    inline std::shared_ptr<i_telemetry_service> get_telemetry_service()
    {
        return telemetry_service_;
    }
}

#ifndef RPC_TELEMETRY_COMPAT_DECLARED
#  define RPC_TELEMETRY_COMPAT_DECLARED
namespace rpc
{
    using i_telemetry_service = telemetry::i_telemetry_service;
    inline auto& telemetry_service_ = telemetry::telemetry_service_;

    inline std::shared_ptr<i_telemetry_service> get_telemetry_service()
    {
        return telemetry::get_telemetry_service();
    }
}
#endif
