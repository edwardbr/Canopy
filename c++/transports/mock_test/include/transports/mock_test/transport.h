/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <functional>
#include <rpc/rpc.h>

namespace rpc::mock_test
{
    // Mock transport for testing passthrough and other components
    // Allows simulation of transport failures and tracking of all method calls
    class mock_transport : public rpc::transport
    {
    public:
        // Call tracking structure
        struct call_record
        {
            enum class call_type
            {
                SEND,
                POST,
                TRY_CAST,
                ADD_REF,
                RELEASE,
                OBJECT_RELEASED,
                TRANSPORT_DOWN
            };

            call_type type;
            uint64_t protocol_version;
            rpc::remote_object destination_zone_id;
            rpc::caller_zone caller_zone_id;
            rpc::object object_id;
            std::chrono::steady_clock::time_point timestamp;
        };

    private:
        std::atomic<bool> force_failure_{false};
        std::atomic<int> forced_error_code_{rpc::error::TRANSPORT_ERROR()};
        std::vector<call_record> call_history_;
        mutable std::mutex call_history_mtx_;

        std::atomic<uint64_t> send_count_{0};
        std::atomic<uint64_t> post_count_{0};
        std::atomic<uint64_t> try_cast_count_{0};
        std::atomic<uint64_t> add_ref_count_{0};
        std::atomic<uint64_t> release_count_{0};
        std::atomic<uint64_t> object_released_count_{0};
        std::atomic<uint64_t> transport_down_count_{0};

        // Optional response handlers for custom behavior

        typedef std::function<CORO_TASK(send_result)(send_params)> send_handler;

        send_handler send_handler_;
        std::mutex send_handler_mtx_;

        void record_call(
            call_record::call_type type,
            uint64_t protocol_version,
            rpc::remote_object destination_zone_id,
            rpc::caller_zone caller_zone_id);

    public:
        mock_transport(
            std::string name,
            std::shared_ptr<rpc::service> service);
        mock_transport(
            std::string name,
            rpc::zone zone_id);
        ~mock_transport() override = default;

        // Control methods for testing
        void set_force_failure(
            bool force_failure,
            int error_code = rpc::error::TRANSPORT_ERROR());
        void clear_call_history();
        std::vector<call_record> get_call_history() const;
        void mark_as_down();
        void mark_as_up();

        // Call count accessors
        uint64_t get_send_count() const { return send_count_.load(std::memory_order_acquire); }
        uint64_t get_post_count() const { return post_count_.load(std::memory_order_acquire); }
        uint64_t get_try_cast_count() const { return try_cast_count_.load(std::memory_order_acquire); }
        uint64_t get_add_ref_count() const { return add_ref_count_.load(std::memory_order_acquire); }
        uint64_t get_release_count() const { return release_count_.load(std::memory_order_acquire); }
        uint64_t get_object_released_count() const { return object_released_count_.load(std::memory_order_acquire); }
        uint64_t get_transport_down_count() const { return transport_down_count_.load(std::memory_order_acquire); }

        // Set custom handler for send (for advanced testing scenarios)
        template<typename Handler> void set_send_handler(Handler&& handler)
        {
            std::scoped_lock lock(send_handler_mtx_);
            send_handler_ = std::forward<Handler>(handler);
        }

        void clear_send_handler()
        {
            std::scoped_lock lock(send_handler_mtx_);
            send_handler_ = nullptr;
        }

        // outbound i_marshaller implementations
        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub> stub,
            connection_settings input_descr) override;
        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        CORO_TASK(send_result) outbound_send(send_params params) override;
        CORO_TASK(void) outbound_post(post_params params) override;
        CORO_TASK(standard_result) outbound_try_cast(try_cast_params params) override;
        CORO_TASK(standard_result) outbound_add_ref(add_ref_params params) override;
        CORO_TASK(standard_result) outbound_release(release_params params) override;
        CORO_TASK(void) outbound_object_released(object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(transport_down_params params) override;
    };
}
