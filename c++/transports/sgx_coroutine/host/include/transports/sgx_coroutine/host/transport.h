/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <cstdint>

#include <io_uring/host_controller.h>
#include <json/json_dom.h>
#include <rpc/rpc.h>
#include <secure_coroutine_module/secure_coroutine_module.h>
#include <sgx_coroutine_transport/sgx_coroutine_transport_config.h>
#include <streaming/stream_layers.h>
#include <streaming/spsc_queue/stream.h>
#include <transports/secure_coroutine_module/startup_options.h>
#include <transports/streaming/transport.h>

namespace streaming
{
    class stream;
}

namespace rpc::sgx_coroutine_transport::host
{
    class transport : public rpc::stream_transport::transport
    {
        class deferred_stream;
        class sidecar_owner;

    public:
        struct stream_layer_result
        {
            int error_code{rpc::error::OK()};
            std::shared_ptr<::streaming::stream> stream;
        };

        using stream_layer_applier = std::function<CORO_TASK(stream_layer_result)(std::shared_ptr<::streaming::stream>)>;

        [[nodiscard]] static int validate_startup_settings(
            const rpc::sgx_coroutine_transport::transport_settings& settings);

    private:
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

        const std::string enclave_path_;
        std::shared_ptr<::streaming::spsc_queue::queue_type> host_to_enclave_queue_;
        std::shared_ptr<::streaming::spsc_queue::queue_type> enclave_to_host_queue_;
        std::shared_ptr<deferred_stream> deferred_stream_;
        std::shared_ptr<::streaming::spsc_queue::stream> queue_stream_;
        std::shared_ptr<enclave_owner> enclave_owner_;
        std::shared_ptr<sidecar_owner> sidecar_owner_;
        rpc::optional<rpc::sgx_enclave_runtime::runtime_settings> enclave_runtime_settings_;
        std::optional<rpc::io_uring::host_controller::options> enclave_io_uring_options_;
        stream_layer_applier host_stream_layer_applier_;
        bool use_sidecar_{false};
        std::string sidecar_executable_path_;
        std::string peer_to_peer_shared_memory_file_;
        transport(
            std::string name,
            std::shared_ptr<rpc::service> service,
            rpc::sgx_coroutine_transport::transport_settings settings,
            std::shared_ptr<deferred_stream> deferred_stream);
        void apply_startup_settings(rpc::sgx_coroutine_transport::transport_settings settings);
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
        transport(
            std::string name,
            std::shared_ptr<rpc::service> service,
            rpc::sgx_coroutine_transport::transport_settings settings);

        ~transport() override;

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub> stub,
            rpc::connection_settings input_descr) override;

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        void set_status(rpc::transport_status status) override;

        const std::string& get_enclave_path() const { return enclave_path_; }
        [[nodiscard]] rpc::optional<rpc::sgx_enclave_runtime::runtime_settings> get_enclave_runtime_startup_settings() const
        {
            return enclave_runtime_settings_;
        }
        [[nodiscard]] std::optional<rpc::io_uring::host_controller::options> get_enclave_io_uring_options() const
        {
            return enclave_io_uring_options_;
        }
        [[nodiscard]] bool get_use_sidecar() const { return use_sidecar_; }
#ifdef CANOPY_BUILD_TEST
        [[nodiscard]] int sidecar_pid_for_test() const;
        [[nodiscard]] uint32_t enclave_worker_thread_count_for_test() const { return enclave_worker_thread_count_; }
        [[nodiscard]] const std::string& sidecar_executable_path_for_test() const { return sidecar_executable_path_; }
        [[nodiscard]] const std::string& peer_to_peer_shared_memory_file_for_test() const
        {
            return peer_to_peer_shared_memory_file_;
        }
        [[nodiscard]] const rpc::v4::secure_coroutine_module::startup_applications& enclave_startup_applications_for_test() const
        {
            return enclave_startup_applications_;
        }
#endif

    private:
        std::atomic<bool> enclave_shutdown_started_{false};
        uint32_t enclave_worker_thread_count_{0};
        std::mutex status_transition_mutex_;
        rpc::v4::secure_coroutine_module::startup_applications enclave_startup_applications_{};
        std::vector<rpc::stream_layers::stream_layer_settings> enclave_stream_layers_{};
    };
}
