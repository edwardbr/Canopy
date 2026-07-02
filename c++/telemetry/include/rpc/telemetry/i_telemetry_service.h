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

#include <filesystem>

// Numeric values mirror spdlog levels and are serialized in telemetry events.
inline constexpr int telemetry_level_debug = 0;
inline constexpr int telemetry_level_trace = 1;
inline constexpr int telemetry_level_info = 2;
inline constexpr int telemetry_level_warn = 3;
inline constexpr int telemetry_level_error = 4;
inline constexpr int telemetry_level_critical = 5;
inline constexpr int telemetry_level_off = 6;

namespace rpc::telemetry
{
    class i_telemetry_service
    {
    public:
        enum level_enum // NOLINT(cppcoreguidelines-use-enum-class): telemetry levels are serialized as integer values.
        {
            debug = telemetry_level_debug,
            trace = telemetry_level_trace,
            info = telemetry_level_info,
            warn = telemetry_level_warn,
            err = telemetry_level_error,
            critical = telemetry_level_critical,
            off = telemetry_level_off,
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
    };

    // Global telemetry service.
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
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    inline auto& telemetry_service_ = telemetry::telemetry_service_;

    inline std::shared_ptr<i_telemetry_service> get_telemetry_service()
    {
        return telemetry::get_telemetry_service();
    }
}
#endif
