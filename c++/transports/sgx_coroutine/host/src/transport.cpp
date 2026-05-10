/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/host/transport.h>
#include <transports/sgx_coroutine/common/startup_status.h>
#include <edl/coroutine_enclave.h>
#include <untrusted/canopy_coroutine_enclave_u.h>
#include <transports/streaming/transport.h>
#include <streaming/stream_transport.h>
#include <streaming/spsc_queue/stream.h>
#include <sgx_urts.h>
#include <cstdint>
#include <cstring>
#include <rpc/rpc.h>
#include <chrono>
#include <thread>
#include <tuple>
#include <utility>

#ifndef CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG
#  define CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG SGX_DEBUG_FLAG
#endif

namespace rpc::sgx::coro::host
{
    namespace
    {
        using namespace rpc::sgx::coro::protocol;

        void signal_peer_closed(common::queue_type* send_queue)
        {
            if (!send_queue)
                return;

            streaming::spsc_queue::blob close_blob{};
            for (size_t attempt = 0; attempt < 10000; ++attempt)
            {
                if (send_queue->push(close_blob))
                    break;
                std::this_thread::yield();
            }
        }

        template<typename T>
        int from_blob(
            rpc::byte_span buffer,
            T& value)
        {
            auto err = rpc::from_yas_binary(buffer, value);
            if (!err.empty())
            {
                RPC_ERROR("sgx_coroutine decode failed: {}", err);
                return rpc::error::INVALID_DATA();
            }
            return rpc::error::OK();
        }

        int wait_for_startup_status(
            const std::shared_ptr<common::startup_status>& status,
            common::startup_state expected_state,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
        {
            if (!status)
                return rpc::error::INVALID_DATA();

            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                auto state = static_cast<common::startup_state>(common::startup_load_u32(&status->state));
                if (state == expected_state)
                    return rpc::error::OK();

                if (state == common::startup_state::failed)
                    return common::startup_load_i32(&status->error_code);

                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }

            RPC_ERROR("startup status: timed out waiting for readiness");
            return rpc::error::CALL_TIMEOUT();
        }

        int wait_for_worker_request(
            const std::shared_ptr<common::startup_status>& status,
            uint32_t& requested_workers,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{20000})
        {
            auto err = wait_for_startup_status(status, common::startup_state::workers_requested, timeout);
            if (err != rpc::error::OK())
                return err;

            requested_workers = common::startup_load_u32(&status->requested_workers);
            return rpc::error::OK();
        }

        int wait_for_attached_workers(
            const std::shared_ptr<common::startup_status>& status,
            uint32_t expected_workers,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{20000})
        {
            if (!status)
                return rpc::error::INVALID_DATA();

            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                auto state = static_cast<common::startup_state>(common::startup_load_u32(&status->state));
                if (state == common::startup_state::failed)
                    return common::startup_load_i32(&status->error_code);

                if (common::startup_load_u32(&status->attached_workers) >= expected_workers)
                    return rpc::error::OK();

                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }

            RPC_ERROR("startup status: timed out waiting for worker attachment");
            return rpc::error::CALL_TIMEOUT();
        }

        uint64_t read_host_tick_counter() noexcept
        {
#if defined(__x86_64__) || defined(__i386__)
            return __builtin_ia32_rdtsc();
#else
            return 0;
#endif
        }

        uint64_t calibrate_host_ticks_per_millisecond() noexcept
        {
            const auto start_ticks = read_host_tick_counter();
            const auto start = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            const auto end = std::chrono::steady_clock::now();
            const auto end_ticks = read_host_tick_counter();

            const auto elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
            if (end_ticks <= start_ticks || elapsed_ms <= 0.0)
                return 0;

            return static_cast<uint64_t>(static_cast<double>(end_ticks - start_ticks) / elapsed_ms);
        }

        uint64_t host_ticks_per_millisecond() noexcept
        {
            // SGX enclave timing uses rdtsc, but the enclave runtime cannot
            // sleep against host wall time cheaply. Calibrate once in the host
            // transport and pass the value through the enclave bootstrap.
            static const auto ticks_per_millisecond = calibrate_host_ticks_per_millisecond();
            return ticks_per_millisecond;
        }

        void fail_startup_status(
            common::startup_status* status,
            int error_code)
        {
            if (!status)
                return;

            common::startup_store_i32(&status->error_code, error_code);
            common::startup_store_u32(&status->state, static_cast<std::uint32_t>(common::startup_state::failed));
        }

        CORO_TASK(void)
        join_init_thread_and_destroy_enclave_on_scheduler(
            rpc::coro::scheduler* scheduler,
            std::shared_ptr<std::thread> thread,
            uint64_t eid)
        {
            // This task runs on the scheduler, so it must not own a
            // shared_ptr to that scheduler. Dropping the last scheduler
            // reference from a scheduler worker would make the scheduler try
            // to join its own thread during destruction.
            CO_AWAIT scheduler->schedule();

            if (thread && thread->joinable())
                thread->join();

            sgx_destroy_enclave(eid);
            CO_RETURN;
        }

    }

    class transport::deferred_stream : public streaming::stream
    {
    public:
        void bind(std::shared_ptr<streaming::stream> stream) { stream_ = std::move(stream); }

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> ::coro::task<std::pair<
                ::coro::net::io_status,
                rpc::mutable_byte_span>> override
        {
            if (!stream_)
                CO_RETURN std::pair{
                    ::coro::net::io_status{.type = ::coro::net::io_status::kind::closed}, rpc::mutable_byte_span{}};
            CO_RETURN CO_AWAIT stream_->receive(buffer, timeout);
        }

        auto send(rpc::byte_span buffer) -> ::coro::task<::coro::net::io_status> override
        {
            if (!stream_)
                CO_RETURN ::coro::net::io_status{.type = ::coro::net::io_status::kind::closed};
            CO_RETURN CO_AWAIT stream_->send(buffer);
        }

        [[nodiscard]] bool is_closed() const override { return !stream_ || stream_->is_closed(); }

        auto set_closed() -> ::coro::task<void> override
        {
            if (stream_)
                CO_AWAIT stream_->set_closed();
            CO_RETURN;
        }

        [[nodiscard]] auto get_peer_info() const -> streaming::peer_info override
        {
            return stream_ ? stream_->get_peer_info() : streaming::peer_info{};
        }

    private:
        std::shared_ptr<streaming::stream> stream_;
    };

    transport::enclave_owner::~enclave_owner()
    {
        auto state = state_;
        if (state && state->init_status_)
        {
            auto startup_state = static_cast<common::startup_state>(common::startup_load_u32(&state->init_status_->state));
            if (startup_state != common::startup_state::failed && startup_state != common::startup_state::stopped)
            {
                common::startup_store_u32(
                    &state->init_status_->state, static_cast<std::uint32_t>(common::startup_state::shutting_down));
            }
        }

        if (init_thread_.joinable())
        {
            auto init_thread = std::make_shared<std::thread>(std::move(init_thread_));
            if (scheduler_ && !scheduler_->is_shutdown())
            {
                // Joining the init ECALL thread can block briefly, so prefer to
                // do it as scheduled teardown work while the host scheduler is
                // still running.
                auto scheduled = scheduler_->spawn_detached(
                    join_init_thread_and_destroy_enclave_on_scheduler(scheduler_.get(), init_thread, state->eid_));
                if (scheduled)
                    return;
            }

            // If the scheduler has already shut down, the owner is being
            // destroyed from ordinary host teardown. Join synchronously rather
            // than detaching the ECALL thread or leaving the enclave loaded.
            init_thread->join();
            sgx_destroy_enclave(state->eid_);
            return;
        }

        if (state)
            sgx_destroy_enclave(state->eid_);
    }

    transport::transport(
        std::string name,
        std::shared_ptr<rpc::service> service,
        std::string enclave_path)
        : transport(
              std::move(name),
              std::move(service),
              std::move(enclave_path),
              std::make_shared<deferred_stream>())
    {
    }

    transport::transport(
        std::string name,
        std::shared_ptr<rpc::service> service,
        std::string enclave_path,
        std::shared_ptr<deferred_stream> deferred_stream)
        : rpc::stream_transport::transport(
              std::move(name),
              std::move(service),
              deferred_stream,
              nullptr,
              rpc::stream_transport::stream_transport_options{.call_timeout = std::chrono::milliseconds{0},
                  .call_timeout_sweep = std::chrono::milliseconds{0}})
        , enclave_path_(std::move(enclave_path))
        , deferred_stream_(std::move(deferred_stream))
    {
    }

    void transport::on_destination_count_zero()
    {
        begin_clean_disconnect();
    }

    void transport::start_worker_thread(
        enclave_owner& owner,
        std::shared_ptr<std::vector<char>> enter_blob)
    {
        auto state = owner.state_;
        auto owner_eid = state->eid_;
        auto* init_status = state->init_status_.get();
        state->worker_threads_.emplace_back(
            [owner_eid, enter_blob = std::move(enter_blob), init_status]()
            {
                int err_code = rpc::error::OK();
                auto status = coroutine_enter_thread(owner_eid, &err_code, enter_blob->size(), enter_blob->data());

                if (status != SGX_SUCCESS)
                {
                    RPC_ERROR("coroutine_enter_thread returned sgx status {}", static_cast<int>(status));
                    fail_startup_status(init_status, rpc::error::TRANSPORT_ERROR());
                    return;
                }

                if (err_code != rpc::error::OK())
                {
                    RPC_ERROR("coroutine_enter_thread returned {}", err_code);
                    fail_startup_status(init_status, err_code);
                }
            });
    }

    CORO_TASK(rpc::connect_result)
    transport::inner_connect(
        std::shared_ptr<rpc::object_stub> stub,
        rpc::connection_settings input_descr)
    {
        auto svc = get_service();
        if (!svc)
        {
            CO_RETURN rpc::connect_result{rpc::error::ZONE_NOT_INITIALISED(), {}};
        }

        get_new_zone_id_params zone_params;
        zone_params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT svc->get_new_zone_id(std::move(zone_params));
        if (zone_result.error_code != rpc::error::OK())
        {
            CO_RETURN rpc::connect_result{zone_result.error_code, {}};
        }

        auto adjacent_zone_id = zone_result.zone_id;
        set_adjacent_zone_id(adjacent_zone_id);

        host_to_enclave_queue_ = std::make_shared<common::queue_type>();
        enclave_to_host_queue_ = std::make_shared<common::queue_type>();

        queue_stream_ = std::make_shared<streaming::spsc_queue::stream>(
            host_to_enclave_queue_, enclave_to_host_queue_, svc->get_scheduler());
        deferred_stream_->bind(queue_stream_);

        sgx_enclave_id_t eid = 0;
        // fire up the enclave
        {
            sgx_launch_token_t token = {0};
            int updated = 0;
            auto status = sgx_create_enclave(
                enclave_path_.c_str(), CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG, &token, &updated, &eid, nullptr);
            if (status != SGX_SUCCESS)
            {
                RPC_ERROR("sgx_create_enclave returned {}", static_cast<int>(status));
                CO_RETURN rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};
            }
        }

        auto owner = std::make_shared<enclave_owner>(eid, svc->get_scheduler());
        owner->state_->host_to_enclave_queue_ = host_to_enclave_queue_;
        owner->state_->enclave_to_host_queue_ = enclave_to_host_queue_;

        // spawn the main thread for this enclave
        {
            init_request request{get_zone_id(),
                adjacent_zone_id,
                input_descr.remote_object_id.is_set() ? input_descr.remote_object_id : get_zone_id().get_address()};
            auto request_blob = std::make_shared<std::vector<char>>(rpc::to_yas_binary<std::vector<char>>(request));
            auto* host_to_enclave_queue = host_to_enclave_queue_.get();
            auto* enclave_to_host_queue = enclave_to_host_queue_.get();
            const auto ticks_per_millisecond = host_ticks_per_millisecond();
            auto state = owner->state_;
            owner->init_thread_ = std::thread(
                [state, request_blob = std::move(request_blob), host_to_enclave_queue, enclave_to_host_queue, ticks_per_millisecond]()
                {
                    auto* init_status = state->init_status_.get();
                    std::vector<char> response_blob(1024);
                    size_t response_size = 0;
                    int err_code = rpc::error::OK();

                    auto status = coroutine_init_enclave(
                        state->eid_,
                        &err_code,
                        request_blob->size(),
                        request_blob->data(),
                        host_to_enclave_queue,
                        enclave_to_host_queue,
                        ticks_per_millisecond,
                        reinterpret_cast<canopy_coroutine_startup_status*>(init_status),
                        response_blob.size(),
                        response_blob.data(),
                        &response_size);

                    if (status != SGX_SUCCESS)
                    {
                        RPC_ERROR("coroutine_init_enclave returned sgx status {}", static_cast<int>(status));
                        fail_startup_status(init_status, rpc::error::TRANSPORT_ERROR());
                    }
                    else if (err_code != rpc::error::OK())
                    {
                        RPC_ERROR("coroutine_init_enclave returned {}", err_code);
                        fail_startup_status(init_status, err_code);
                    }

                    for (auto& worker_thread : state->worker_threads_)
                    {
                        if (worker_thread.joinable())
                            worker_thread.join();
                    }
                });
        }

        uint32_t requested_workers = 0;
        auto startup_error = wait_for_worker_request(owner->state_->init_status_, requested_workers);
        if (startup_error != rpc::error::OK())
        {
            enclave_owner_ = std::move(owner);
            set_status(rpc::transport_status::DISCONNECTING);
            enclave_owner_.reset();
            queue_stream_.reset();
            host_to_enclave_queue_.reset();
            enclave_to_host_queue_.reset();
            CO_RETURN rpc::connect_result{startup_error, {}};
        }

        for (uint32_t worker_index = 0; worker_index < requested_workers; ++worker_index)
        {
            auto enter_blob
                = std::make_shared<std::vector<char>>(rpc::to_yas_binary<std::vector<char>>(enter_thread_request{
                    adjacent_zone_id,
                    worker_index,
                }));
            start_worker_thread(*owner, std::move(enter_blob));
            startup_error = wait_for_attached_workers(
                owner->state_->init_status_, worker_index + 1, std::chrono::milliseconds{20000});
            if (startup_error != rpc::error::OK())
            {
                enclave_owner_ = std::move(owner);
                set_status(rpc::transport_status::DISCONNECTING);
                enclave_owner_.reset();
                queue_stream_.reset();
                host_to_enclave_queue_.reset();
                enclave_to_host_queue_.reset();
                CO_RETURN rpc::connect_result{startup_error, {}};
            }
        }

        startup_error = wait_for_startup_status(
            owner->state_->init_status_, common::startup_state::runtime_ready, std::chrono::milliseconds{20000});
        if (startup_error != rpc::error::OK())
        {
            enclave_owner_ = std::move(owner);
            set_status(rpc::transport_status::DISCONNECTING);
            enclave_owner_.reset();
            queue_stream_.reset();
            host_to_enclave_queue_.reset();
            enclave_to_host_queue_.reset();
            CO_RETURN rpc::connect_result{startup_error, {}};
        }

        enclave_owner_ = std::move(owner);
        initialise_after_construction();

        // now the complex dance of getting the enclave and workers up we now need to use the stream transport to do inner_connect
        auto connect_result
            = CO_AWAIT rpc::stream_transport::transport::inner_connect(std::move(stub), std::move(input_descr));
        if (connect_result.error_code != rpc::error::OK())
        {
            set_status(rpc::transport_status::DISCONNECTING);
            enclave_owner_.reset();
            queue_stream_.reset();
            host_to_enclave_queue_.reset();
            enclave_to_host_queue_.reset();
            CO_RETURN connect_result;
        }

        set_status(rpc::transport_status::CONNECTED);
        CO_RETURN connect_result;
    }

    void transport::set_status(rpc::transport_status status)
    {
        auto old_status = get_status();
        rpc::stream_transport::transport::set_status(status);

        if (status >= rpc::transport_status::DISCONNECTING)
        {
            if (status >= rpc::transport_status::DISCONNECTED && old_status < rpc::transport_status::DISCONNECTED
                && enclave_owner_ && enclave_owner_->state_ && enclave_owner_->state_->init_status_)
                common::startup_store_u32(
                    &enclave_owner_->state_->init_status_->state,
                    static_cast<std::uint32_t>(common::startup_state::shutting_down));

            if (queue_stream_ && status >= rpc::transport_status::DISCONNECTED
                && old_status < rpc::transport_status::DISCONNECTED)
            {
                signal_peer_closed(host_to_enclave_queue_.get());
                rpc::coro::sync_wait(queue_stream_->set_closed());
            }
        }
    }

}
