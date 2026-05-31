/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <functional>
#  include <memory>
#  include <string>

#  include <rpc/rpc.h>
#  include <transports/ipc_spsc_transport/bootstrap.h>
#  include <transports/ipc_spsc_transport/dll_abi.h>
#  include <transports/streaming/transport.h>

namespace rpc::ipc_spsc_transport
{
    enum class child_process_kind
    {
        host_dll,
        direct_service,
    };

    enum class peer_role
    {
        connector,
        acceptor,
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

    struct shared_memory_file_options
    {
        std::string path;
        bool create = false;
        bool unlink_on_destroy = false;
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

        static construction_bundle create_bundle(
            const std::shared_ptr<rpc::service>& service,
            const options& options);
        static void spawn_child(
            const std::shared_ptr<state>& state,
            const options& options);
        static void reap_child(const std::shared_ptr<state>& state);

        transport(
            std::string name,
            const std::shared_ptr<rpc::service>& service,
            options options,
            construction_bundle bundle);

    public:
        transport(
            std::string name,
            const std::shared_ptr<rpc::service>& service,
            options options);
        ~transport() override;
        void initialise();
#  ifdef CANOPY_BUILD_TEST
        [[nodiscard]] int child_pid_for_test() const;
#  endif

        void set_status(rpc::transport_status new_status) override;
    };

    std::shared_ptr<transport> make_client(
        std::string name,
        const std::shared_ptr<rpc::service>& service,
        options options);

    std::shared_ptr<streaming::stream> make_shared_memory_stream(
        shared_memory_file_options options,
        peer_role role,
        std::shared_ptr<rpc::coro::scheduler> scheduler);

    std::shared_ptr<rpc::stream_transport::transport> make_peer_client(
        std::string name,
        const std::shared_ptr<rpc::service>& service,
        shared_memory_file_options options);

    template<
        class Remote,
        class Local>
    CORO_TASK(std::shared_ptr<rpc::stream_transport::transport>)
    make_peer_acceptor(
        std::string name,
        const std::shared_ptr<rpc::service>& service,
        shared_memory_file_options options,
        std::function<CORO_TASK(rpc::service_connect_result<Local>)(
            rpc::shared_ptr<Remote>,
            std::shared_ptr<rpc::service>)> factory)
    {
        if (!service)
            CO_RETURN{};

        auto stream = make_shared_memory_stream(std::move(options), peer_role::acceptor, service->get_scheduler());
        if (!stream)
            CO_RETURN{};

        CO_RETURN std::static_pointer_cast<rpc::stream_transport::transport>(
            CO_AWAIT service->template make_acceptor<Remote, Local>(
                std::move(name), rpc::stream_transport::transport_factory(std::move(stream)), std::move(factory)));
    }

} // namespace rpc::ipc_spsc_transport

#endif // CANOPY_BUILD_COROUTINE
