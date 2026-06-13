/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <transports/ipc_spsc/factory.h>

#  include <utility>

#  include <transports/ipc_spsc/transport.h>

namespace rpc::ipc_spsc
{
    namespace
    {
        [[nodiscard]] std::string default_sidecar_process_path()
        {
#  ifdef CANOPY_IPC_SPSC_SIDECAR_PROCESS_PATH
            return CANOPY_IPC_SPSC_SIDECAR_PROCESS_PATH;
#  else
            return {};
#  endif
        }

        [[nodiscard]] rpc::zone child_zone_from_offset(uint64_t offset)
        {
            if (offset == 0)
                return {};

            auto address = rpc::DEFAULT_PREFIX;
            if (!address.set_subnet(address.get_subnet() + offset))
                return {};

            return rpc::zone(address);
        }

        [[nodiscard]] std::string sidecar_executable_for_settings(const transport_settings& settings)
        {
            if (!settings.sidecar_executable_path.empty())
                return settings.sidecar_executable_path;

            if (!settings.dynamic_library_path.empty())
                return default_sidecar_process_path();

            return {};
        }
    } // namespace

    rpc::transport_creation::connect_result connect_transport(
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service)
    {
        auto child_zone = child_zone_from_offset(settings.child_subnet_offset);
        if (!child_zone.is_set())
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto resolved_service
            = rpc::transport_creation::ensure_service(std::move(service), settings.encoding, "ipc_spsc_rpc_client");
        if (!resolved_service)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto transport_name = rpc::transport_creation::configured_name(settings.name, "ipc_spsc");
        auto proxy_name = rpc::transport_creation::configured_name(
            settings.service_proxy_name, rpc::transport_creation::configured_name(settings.name, "ipc_spsc_child"));

        if (!settings.use_sidecar)
        {
            if (settings.peer_to_peer_shared_memory_file.empty())
                return {rpc::error::INVALID_DATA(), {}, {}, {}};

            // Peer-to-peer mode is the config-driven file-backed case: map an
            // existing rendezvous file by default, or create and initialise it
            // when explicitly requested by the caller.
            auto transport = rpc::ipc_spsc::make_peer_client(
                transport_name,
                resolved_service,
                shared_memory_file_options{.path = settings.peer_to_peer_shared_memory_file,
                    .create = settings.create_peer_to_peer_shared_memory_file,
                    .unlink_on_destroy = settings.unlink_peer_to_peer_shared_memory_file_on_close});
            if (!transport)
                return {rpc::error::TRANSPORT_ERROR(), {}, {}, {}};

            return {rpc::error::OK(), std::move(resolved_service), std::move(transport), std::move(proxy_name)};
        }

        auto executable_path = sidecar_executable_for_settings(settings);
        if (executable_path.empty())
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        rpc::ipc_spsc::options options;
        options.process_executable = std::move(executable_path);
        options.dll_path = settings.dynamic_library_path;
        options.dll_zone = child_zone;
        options.process_kind
            = settings.dynamic_library_path.empty() ? child_process_kind::direct_service : child_process_kind::host_dll;
        options.child_scheduler_thread_count = settings.scheduler_thread_count;
        options.kill_child_on_parent_death = settings.kill_child_on_parent_death;

        auto transport = rpc::ipc_spsc::make_client(transport_name, resolved_service, std::move(options));
        return {rpc::error::OK(), std::move(resolved_service), std::move(transport), std::move(proxy_name)};
    }
} // namespace rpc::ipc_spsc

#endif // CANOPY_BUILD_COROUTINE
