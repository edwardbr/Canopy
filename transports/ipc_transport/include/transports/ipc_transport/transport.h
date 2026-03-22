/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <memory>
#  include <string>

#  include <rpc/rpc.h>
#  include <transports/ipc_transport/bootstrap.h>
#  include <transports/libcoro_spsc_dynamic_dll/dll_abi.h>
#  include <transports/streaming/transport.h>

namespace rpc::ipc_transport
{
    enum class child_process_kind
    {
        host_dll,
        direct_service,
    };

    struct options
    {
        std::string process_executable;
        std::string dll_path;
        rpc::zone dll_zone;
        child_process_kind process_kind = child_process_kind::host_dll;
        size_t child_scheduler_thread_count = 1;
        bool kill_child_on_parent_death = false;
    };

    class transport : public rpc::stream_transport::transport
    {
        struct state;

        std::shared_ptr<state> state_;
        bool child_started_ = false;

        struct construction_bundle
        {
            std::shared_ptr<state> state;
            std::shared_ptr<streaming::stream> stream;
        };

        static construction_bundle create_bundle(const std::shared_ptr<rpc::service>& service, const options& options);
        static void spawn_child(const std::shared_ptr<state>& state, const options& options);
        static void reap_child(const std::shared_ptr<state>& state);

        transport(
            std::string name, const std::shared_ptr<rpc::service>& service, options options, construction_bundle bundle);

    public:
        transport(std::string name, const std::shared_ptr<rpc::service>& service, options options);
        ~transport() override;
        void initialise();
#  ifdef CANOPY_BUILD_TEST
        [[nodiscard]] int child_pid_for_test() const;
#  endif

        void set_status(rpc::transport_status new_status) override;
    };

    std::shared_ptr<transport> make_client(std::string name, const std::shared_ptr<rpc::service>& service, options options);

} // namespace rpc::ipc_transport

#endif // CANOPY_BUILD_COROUTINE
