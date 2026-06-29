/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cerrno>
#include <cstdint>
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <streaming/tcp_coroutine/factory.h>
#include <transport/tests/streaming_setup_base.h>

template<bool UseHostInChild, bool RunStandardTests, bool CreateNewZoneThenCreateSubordinatedZone>
class streaming_tcp_coroutine_setup
    : public streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>
{
    using base = streaming_setup_base<UseHostInChild, RunStandardTests, CreateNewZoneThenCreateSubordinatedZone>;

    std::shared_ptr<rpc::stream_transport::listener_handle> rpc_listener_;

protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = this->make_peer_zone_id();

        this->peer_service_ = rpc::root_service::create("peer", peer_zone_id, this->io_scheduler_);
        this->root_service_ = rpc::root_service::create("host", root_zone_id, this->io_scheduler_);
        current_host_service = this->root_service_;

        rpc::shared_ptr<yyy::i_host> hst(new host());
        this->local_host_ptr_ = hst;

        ::rpc::tcp_coroutine_stream::endpoint accept_tcp_coroutine_options;

        rpc::stream_transport::connection_settings accept_factory_options;
        accept_factory_options.transport.call_timeout = uint64_t{30000};
        accept_factory_options.transport.call_timeout_sweep = uint64_t{1};

        auto accept_result = CO_AWAIT rpc::tcp_coroutine::accept_rpc<yyy::i_host, yyy::i_example>(
            this->make_interface_setup_factory(),
            accept_tcp_coroutine_options,
            accept_factory_options,
            this->peer_service_,
            [this](std::shared_ptr<rpc::stream_transport::transport> transport)
            { this->responder_transport_ = std::move(transport); });

        if (accept_result.error_code != rpc::error::OK() || !accept_result.handle)
        {
            RPC_ERROR("Failed to start TCP coroutine listener task");
            CO_RETURN false;
        }
        rpc_listener_ = std::move(accept_result.handle);

        ::rpc::tcp_coroutine_stream::endpoint connect_tcp_coroutine_options;
        connect_tcp_coroutine_options.port = rpc_listener_->port();

        rpc::stream_transport::connection_settings connect_factory_options;
        connect_factory_options.transport.call_timeout = uint64_t{30000};
        connect_factory_options.transport.call_timeout_sweep = uint64_t{1};

        auto connect_to_zone_result = CO_AWAIT rpc::tcp_coroutine::connect_rpc<yyy::i_host, yyy::i_example>(
            hst, connect_tcp_coroutine_options, connect_factory_options, this->root_service_);
        this->i_example_ptr_ = std::move(connect_to_zone_result.output_interface);

        if (connect_to_zone_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("Failed to connect to zone: {}", connect_to_zone_result.error_code);
            CO_RETURN false;
        }

        CO_RETURN true;
    }

    CORO_TASK(void) do_coro_teardown() override
    {
        if (rpc_listener_)
        {
            CO_AWAIT rpc_listener_->stop();
        }
        CO_RETURN;
    }

public:
    ~streaming_tcp_coroutine_setup() override = default;

    void set_up() override
    {
        // Probe io_uring availability before starting the coroutine machinery.
        // A null params pointer gives EFAULT before the permission check, so we
        // must pass a valid (zeroed) params struct.  With flags=0 and entries=2:
        //   fd >= 0  → io_uring is available; close the probe fd and proceed.
        //   EPERM    → io_uring disabled by policy (io_uring_disabled sysctl or
        //              seccomp); skip rather than crash with SIGABRT.
        //   ENOSYS   → kernel has no io_uring support at all; skip.
        io_uring_params probe_params{};
        const long probe_fd = ::syscall(SYS_io_uring_setup, 2u, &probe_params);
        if (probe_fd >= 0)
            ::close(static_cast<int>(probe_fd));
        else if (errno == EPERM || errno == ENOSYS)
            GTEST_SKIP() << "io_uring not available in this environment (errno=" << errno << ")";
        base::set_up();
    }

    void tear_down() override
    {
        // base::tear_down() dereferences io_scheduler_, which is only created by
        // base::set_up(). If set_up() skipped early (e.g. GTEST_SKIP), the
        // scheduler is null and must not be touched.
        if (this->io_scheduler_)
            base::tear_down();
        rpc_listener_.reset();
    }

private:
};
