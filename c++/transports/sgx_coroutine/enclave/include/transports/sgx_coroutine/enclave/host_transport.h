/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <chrono>
#include <edl/coroutine_enclave.h>
#include <functional>
#include <io_uring/controller.h>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <rpc/rpc.h>
#include <streaming/stream.h>
#include <string>
#include <transports/sgx_coroutine/enclave/service.h>
#include <transports/streaming/transport.h>
#include <utility>

namespace rpc::sgx::coro::enclave
{
    class host_transport : public rpc::stream_transport::transport
    {
    public:
        static std::shared_ptr<host_transport> create(
            std::string name,
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<streaming::stream> stream,
            connection_handler handler,
            rpc::stream_transport::stream_transport_options options = {});

        ~host_transport() override;

        void set_runtime_destroyed_handler(std::function<void()> handler);

        CORO_TASK(int)
        retain_io_uring_control_reference(const rpc::shared_ptr<rpc::sgx::coro::protocol::i_io_uring_control>& control);

        CORO_TASK(int) wake_host_iouring();
        CORO_TASK(int) get_iouring_data(rpc::io_uring::data& ring_data);
        CORO_TASK(int)
        host_tcp_operation(
            rpc::sgx::coro::protocol::host_tcp_request request,
            rpc::sgx::coro::protocol::host_tcp_result& result);

        int release_io_uring_control_reference();

    private:
        struct host_control_reference
        {
            uint64_t protocol_version{rpc::get_version()};
            rpc::encoding encoding{rpc::encoding::yas_binary};
            rpc::remote_object remote_object_id;
            rpc::caller_zone caller_zone_id;
        };

        host_transport(
            std::string name,
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<streaming::stream> stream,
            connection_handler handler,
            rpc::stream_transport::stream_transport_options options);

        void on_disconnecting() override;

        mutable std::mutex host_control_reference_mutex_;
        std::optional<host_control_reference> host_control_reference_;
        std::optional<rpc::release_params> host_control_release_;
        std::function<void()> runtime_destroyed_handler_;
    };
}
