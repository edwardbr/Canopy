/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rpc/rpc.h>
#include <transports/streaming/transport.h>
#include <transports/sgx_coroutine/common/startup_status.h>
#include <transports/sgx_coroutine/common/shared_queue.h>

namespace streaming
{
    class stream;
}

namespace rpc::sgx::coro::host
{
    class transport : public rpc::stream_transport::transport
    {
        class deferred_stream;

        struct enclave_owner
        {
            struct thread_state
            {
                explicit thread_state(uint64_t eid)
                    : eid_(eid)
                {
                    init_status_ = std::make_shared<common::startup_status>();
                    common::initialise_startup_status(*init_status_);
                }

                uint64_t eid_ = 0;
                std::vector<std::thread> worker_threads_;
                std::shared_ptr<common::startup_status> init_status_;
                // The enclave receives raw queue pointers through the ECALL.
                // Keep the queue storage tied to the ECALL thread lifetime so
                // transport destruction cannot unmap it while workers are
                // still returning from the enclave.
                std::shared_ptr<common::queue_type> host_to_enclave_queue_;
                std::shared_ptr<common::queue_type> enclave_to_host_queue_;
            };

            enclave_owner(
                uint64_t eid,
                rpc::coro::scheduler_ptr scheduler)
                : scheduler_(std::move(scheduler))
                , state_(std::make_shared<thread_state>(eid))
            {
            }

            rpc::coro::scheduler_ptr scheduler_;
            std::thread init_thread_;
            std::shared_ptr<thread_state> state_;
            ~enclave_owner();
        };

        std::string enclave_path_;
        std::shared_ptr<common::queue_type> host_to_enclave_queue_;
        std::shared_ptr<common::queue_type> enclave_to_host_queue_;
        std::shared_ptr<deferred_stream> deferred_stream_;
        std::shared_ptr<streaming::stream> queue_stream_;
        std::shared_ptr<enclave_owner> enclave_owner_;
        transport(
            std::string name,
            std::shared_ptr<rpc::service> service,
            std::string enclave_path,
            std::shared_ptr<deferred_stream> deferred_stream);
        void start_worker_thread(
            enclave_owner& owner,
            std::shared_ptr<std::vector<char>> enter_blob);

    protected:
        void on_destination_count_zero() override;

    public:
        transport(
            std::string name,
            std::shared_ptr<rpc::service> service,
            std::string enclave_path);

        ~transport() override = default;

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub> stub,
            rpc::connection_settings input_descr) override;

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        void set_status(rpc::transport_status status) override;

        const std::string& get_enclave_path() const { return enclave_path_; }
    };
}
