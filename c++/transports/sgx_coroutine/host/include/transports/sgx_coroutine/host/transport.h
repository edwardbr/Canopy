/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <cstdint>

#include <json/json_dom.h>
#include <rpc/rpc.h>
#include <secure_coroutine_module/secure_coroutine_module.h>
#include <streaming/spsc_queue/stream.h>
#include <transports/streaming/transport.h>

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
                explicit thread_state(uint64_t eid);

                uint64_t eid_ = 0;
                std::vector<std::thread> worker_threads_;
                rpc::v4::secure_coroutine_module::startup_state startup_state_{};
                error_code startup_error_code_{};
                // The enclave receives raw queue pointers through the ECALL.
                // Keep the queue storage tied to the ECALL thread lifetime so
                // transport destruction cannot unmap it while workers are
                // still returning from the enclave.
                std::shared_ptr<::streaming::spsc_queue::queue_type> host_to_enclave_queue_;
                std::shared_ptr<::streaming::spsc_queue::queue_type> enclave_to_host_queue_;
            };

            explicit enclave_owner(uint64_t eid)
                : state_(std::make_shared<thread_state>(eid))
            {
            }

            std::thread init_thread_;
            std::shared_ptr<thread_state> state_;
            ~enclave_owner();

            void request_shutdown() const;
            static bool is_current_thread(const std::thread& thread) noexcept;
            static void join_or_detach_if_current(std::thread& thread);
            static void join_worker_threads(const std::shared_ptr<thread_state>& state);
            static void cleanup_threads_and_destroy_enclave(
                std::shared_ptr<thread_state> state,
                std::thread init_thread);
        };

        std::string enclave_path_;
        std::shared_ptr<::streaming::spsc_queue::queue_type> host_to_enclave_queue_;
        std::shared_ptr<::streaming::spsc_queue::queue_type> enclave_to_host_queue_;
        std::shared_ptr<deferred_stream> deferred_stream_;
        std::shared_ptr<::streaming::spsc_queue::stream> queue_stream_;
        std::shared_ptr<enclave_owner> enclave_owner_;
        transport(
            std::string name,
            std::shared_ptr<rpc::service> service,
            std::string enclave_path,
            std::shared_ptr<deferred_stream> deferred_stream);
        void start_worker_thread(
            enclave_owner& owner,
            std::shared_ptr<std::vector<char>> enter_blob);
        void begin_enclave_shutdown_once() noexcept;
        static void destroy_enclave_owner_async(
            std::shared_ptr<enclave_owner> owner,
            rpc::coro::scheduler_ptr scheduler) noexcept;

    protected:
        void on_destination_count_zero() override;

    public:
        transport(
            std::string name,
            std::shared_ptr<rpc::service> service,
            std::string enclave_path);

        ~transport() override;

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub> stub,
            rpc::connection_settings input_descr) override;

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        void set_status(rpc::transport_status status) override;

        const std::string& get_enclave_path() const { return enclave_path_; }
        void set_enclave_worker_thread_count(uint32_t worker_thread_count);
        [[nodiscard]] int set_enclave_startup_options(json::v1::object options);

    private:
        std::atomic<bool> enclave_shutdown_started_{false};
        std::atomic<uint32_t> enclave_worker_thread_count_{0};
        std::mutex enclave_startup_options_mutex_;
        std::map<std::string, json::v1::object> enclave_startup_applications_{};
    };
}
