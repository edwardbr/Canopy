/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/common/startup_status.h>
#include <transports/sgx_coroutine/common/shared_queue.h>
#include <transports/sgx_coroutine/enclave/runtime.h>
#include <transports/sgx_coroutine/enclave/host_transport.h>
#include <edl/coroutine_enclave.h>
#include <io_uring/io_uring_scheduler.h>
#include <trusted/canopy_coroutine_enclave_t.h>
#include <sgx_error.h>
#include <sgx_trts.h>
#include <cstring>
#include <rpc/rpc.h>
#ifdef CANOPY_USE_TELEMETRY
#  include <rpc/telemetry/i_telemetry_service.h>
#  include <rpc/telemetry/telemetry_service_factory.h>
#endif
#include <streaming/stream_transport.h>
#include <streaming/spsc_queue/stream.h>
#include <transports/streaming/transport.h>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <tuple>
#include <vector>

#ifdef CANOPY_USE_TELEMETRY
namespace rpc
{
    namespace telemetry
    {
        std::shared_ptr<i_telemetry_service> telemetry_service_ = nullptr;
    }
}
#endif

namespace rpc::sgx::coro::enclave
{
    namespace
    {
        namespace protocol = rpc::sgx::coro::protocol;

        struct runtime_state
        {
            std::shared_ptr<rpc::io_uring::io_uring_scheduler> io_uring_scheduler;
            std::shared_ptr<::rpc::coro::scheduler> scheduler;
            std::weak_ptr<host_transport> log_transport;
            rpc::zone enclave_zone{};
            std::atomic<uint32_t> requested_workers{0};
            std::atomic<uint32_t> attached_workers{0};
            std::atomic<bool> accepting_workers{false};
            std::atomic<bool> init_called{false};
            std::unique_ptr<std::atomic<bool>[]> admitted_workers;
            uint32_t admitted_worker_slots = 0;
            std::atomic<bool> registered{false};
            acceptor_factory acceptor_factory;
            common::startup_status* control_status = nullptr;
            std::atomic<bool> cleanup_requested{false};
            std::atomic<bool> connection_established{false};
            std::atomic<bool> host_transport_destroyed{false};
            std::atomic<bool> stop_workers{false};
            std::atomic<uint64_t> ticks_per_millisecond{
                static_cast<uint64_t>(std::chrono::sgx_rdtsc_ticks_per_millisecond)};
            std::atomic<uint64_t> initial_unix_epoch_milliseconds{0};
            std::atomic<uint64_t> initial_tick_counter{0};
        };

        auto runtime_storage() -> runtime_state&
        {
            static runtime_state runtime;
            return runtime;
        }

        int register_runtime(runtime_state& runtime)
        {
            bool expected = false;
            if (!runtime.registered.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
                return rpc::error::FRAUDULANT_REQUEST();

            return rpc::error::OK();
        }

        bool is_aligned(
            const void* pointer,
            std::size_t alignment)
        {
            return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
        }

        bool validate_ecall_blob(
            const char* data,
            size_t size)
        {
            if (size == 0)
                return data == nullptr;

            return data && sgx_is_within_enclave(data, size) != 0;
        }

        auto find_runtime() -> runtime_state*
        {
            auto& runtime = runtime_storage();
            return runtime.registered.load(std::memory_order_acquire) ? &runtime : nullptr;
        }

        void erase_runtime()
        {
            auto& runtime = runtime_storage();
            runtime.registered.store(false, std::memory_order_release);
        }

        void reset_runtime_cleanup_state(runtime_state& runtime)
        {
            runtime.cleanup_requested.store(false, std::memory_order_release);
            runtime.connection_established.store(false, std::memory_order_release);
            runtime.host_transport_destroyed.store(false, std::memory_order_release);
            runtime.stop_workers.store(false, std::memory_order_release);
        }

        uint64_t fallback_ticks_per_millisecond() noexcept
        {
            return static_cast<uint64_t>(std::chrono::sgx_rdtsc_ticks_per_millisecond);
        }

        uint64_t normalise_ticks_per_millisecond(uint64_t ticks_per_millisecond) noexcept
        {
            return ticks_per_millisecond != 0 ? ticks_per_millisecond : fallback_ticks_per_millisecond();
        }

        void request_runtime_cleanup(runtime_state& runtime)
        {
            bool expected = false;
            if (!runtime.cleanup_requested.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                return;
            }

            // Runtime cleanup is enclave-scoped rather than zone-scoped. The
            // io_uring scheduler owns the runtime controller and shuts it down
            // before the host transport performs its final host-control release.
            if (runtime.io_uring_scheduler)
                runtime.io_uring_scheduler->request_controller_shutdown();
        }

        template<typename T> std::vector<char> to_blob(const T& value)
        {
            return rpc::to_yas_binary<std::vector<char>>(value);
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

        template<typename T>
        int write_blob_response(
            const T& value,
            size_t output_capacity,
            char* output_buffer,
            size_t* output_size)
        {
            if (!output_size)
                return rpc::error::INVALID_DATA();

            auto blob = to_blob(value);
            *output_size = blob.size();
            if (*output_size > output_capacity)
                return rpc::error::NEED_MORE_MEMORY();

            if (output_buffer && !blob.empty())
                std::memcpy(output_buffer, blob.data(), blob.size());
            return rpc::error::OK();
        }

        bool validate_shared_queue_pointer(const void* queue_ptr)
        {
            if (!queue_ptr)
                return false;

            if (!is_aligned(queue_ptr, alignof(common::queue_type)))
                return false;

            return sgx_is_outside_enclave(queue_ptr, sizeof(common::queue_type)) != 0;
        }

        auto validate_startup_status_pointer(canopy_coroutine_startup_status* pointer) -> common::startup_status*
        {
            if (!pointer)
                return nullptr;

            if (!is_aligned(pointer, alignof(common::startup_status)))
                return nullptr;

            if (sgx_is_outside_enclave(pointer, sizeof(common::startup_status)) == 0)
                return nullptr;

            if (common::startup_load_u32(&pointer->abi_version) != common::startup_status_abi_version)
                return nullptr;

            return reinterpret_cast<common::startup_status*>(pointer);
        }

        bool shutdown_requested(runtime_state& runtime)
        {
            auto* status = runtime.control_status;
            if (!status)
                return false;

            auto state = static_cast<common::startup_state>(common::startup_load_u32(&status->state));
            return state == common::startup_state::shutting_down || state == common::startup_state::stopped;
        }

        bool runtime_stopped(runtime_state& runtime)
        {
            if (runtime.stop_workers.load(std::memory_order_acquire))
                return true;

            auto* status = runtime.control_status;
            if (!status)
                return true;

            auto state = static_cast<common::startup_state>(common::startup_load_u32(&status->state));
            return state == common::startup_state::failed || state == common::startup_state::shutting_down
                   || state == common::startup_state::stopped;
        }

        void set_startup_status(
            common::startup_status* status,
            common::startup_state state,
            int error_code)
        {
            if (!status)
                return;

            common::startup_store_i32(&status->error_code, error_code);
            common::startup_store_u32(&status->state, static_cast<std::uint32_t>(state));
        }

        void set_worker_request(
            common::startup_status* status,
            uint32_t requested_workers,
            uint32_t attached_workers)
        {
            if (!status)
                return;

            common::startup_store_u32(&status->requested_workers, requested_workers);
            common::startup_store_u32(&status->attached_workers, attached_workers);
            common::startup_store_i32(&status->error_code, rpc::error::OK());
            common::startup_store_u32(&status->state, static_cast<std::uint32_t>(common::startup_state::workers_requested));
        }

        void set_attached_workers(
            common::startup_status* status,
            uint32_t attached_workers)
        {
            if (!status)
                return;

            common::startup_store_u32(&status->attached_workers, attached_workers);
        }

        void cpu_relax()
        {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }

        int wait_for_workers(
            runtime_state& runtime,
            uint32_t requested_workers)
        {
            while (!shutdown_requested(runtime))
            {
                if (runtime.attached_workers.load(std::memory_order_acquire) >= requested_workers)
                    return rpc::error::OK();

                cpu_relax();
            }

            return rpc::error::CALL_CANCELLED();
        }

        // int wait_for_transport_io_loops(
        //     runtime_state& runtime,
        //     const std::shared_ptr<host_transport>& transport,
        //     std::chrono::milliseconds timeout)
        // {
        //     if (!runtime.scheduler || !transport)
        //         return rpc::error::ZONE_NOT_INITIALISED();

        //     auto deadline = std::chrono::steady_clock::now() + timeout;
        //     while (!transport->io_loops_started())
        //     {
        //         if (shutdown_requested(runtime))
        //             return rpc::error::CALL_CANCELLED();
        //         if (std::chrono::steady_clock::now() >= deadline)
        //             return rpc::error::CALL_TIMEOUT();

        //         runtime.scheduler->process_events();
        //         cpu_relax();
        //     }

        //     return rpc::error::OK();
        // }

        void drain_ready_scheduler_work(
            runtime_state& runtime,
            int idle_target,
            int max_iterations);

        bool host_transport_released(
            runtime_state& runtime,
            const std::weak_ptr<host_transport>& weak_transport)
        {
            if (runtime.host_transport_destroyed.load(std::memory_order_acquire))
                return true;

            // The destructor callback is the intended shutdown signal. The
            // weak-pointer check is only a fallback for older/private acceptors
            // that do not install the host_transport destructor handler.
            return weak_transport.expired();
        }

        void drain_until_host_transport_released(
            runtime_state& runtime,
            const std::weak_ptr<host_transport>& weak_transport)
        {
            if (!runtime.scheduler)
                return;

            // The runtime must stay alive until the host-facing transport has
            // actually been destroyed. Service teardown can be the last owner
            // of the transport, so this wait belongs after service.reset().
            while (!host_transport_released(runtime, weak_transport))
            {
                runtime.scheduler->process_events();
                cpu_relax();
            }
        }

        int run_runtime_loop(
            runtime_state& runtime,
            std::weak_ptr<host_transport> weak_transport)
        {
            (void)weak_transport;
            if (!runtime.scheduler)
                return rpc::error::ZONE_NOT_INITIALISED();

            while (true)
            {
                const bool host_transport_destroyed = runtime.host_transport_destroyed.load(std::memory_order_acquire);

                if (host_transport_destroyed || shutdown_requested(runtime))
                    break;

                auto processed = runtime.scheduler->process_events();
                (void)processed;
            }
            return rpc::error::OK();
        }

        int run_worker_loop(
            runtime_state& runtime,
            uint64_t worker_index)
        {
            if (!runtime.scheduler)
                return rpc::error::ZONE_NOT_INITIALISED();

            runtime.scheduler->run_worker_until(
                worker_index, [&runtime]() { return runtime_stopped(runtime); }, [](bool) { });
            return rpc::error::OK();
        }

        void wait_for_worker_loops_to_exit(runtime_state& runtime)
        {
            while (runtime.attached_workers.load(std::memory_order_acquire) > 0)
            {
                if (runtime.scheduler)
                    runtime.scheduler->process_events();

                cpu_relax();
            }
        }

        void stop_worker_loops(runtime_state& runtime)
        {
            // The master ECALL owns runtime teardown. Once service and
            // transport cleanup has drained, ask worker ECALLs to leave the
            // scheduler before runtime state is reset.
            runtime.stop_workers.store(true, std::memory_order_release);
            wait_for_worker_loops_to_exit(runtime);
        }

        void drain_ready_scheduler_work(
            runtime_state& runtime,
            int idle_target,
            int max_iterations)
        {
            if (!runtime.scheduler)
                return;

            for (int idle_iterations = 0, total_iterations = 0;
                idle_iterations < idle_target && total_iterations < max_iterations;
                ++total_iterations)
            {
                if (runtime.scheduler->process_ready_event())
                    idle_iterations = 0;
                else
                {
                    ++idle_iterations;
                    cpu_relax();
                }
            }
        }

        void reset_runtime_after_stop(runtime_state& runtime)
        {
            if (runtime.io_uring_scheduler)
                runtime.io_uring_scheduler->shutdown();
            else if (runtime.scheduler)
                runtime.scheduler->shutdown();
            runtime.io_uring_scheduler.reset();
            runtime.scheduler.reset();
            runtime.log_transport.reset();
            runtime.enclave_zone = {};
            runtime.requested_workers.store(0, std::memory_order_release);
            runtime.attached_workers.store(0, std::memory_order_release);
            runtime.accepting_workers.store(false, std::memory_order_release);
            runtime.initial_unix_epoch_milliseconds.store(0, std::memory_order_release);
            runtime.initial_tick_counter.store(0, std::memory_order_release);
            runtime.control_status = nullptr;
            runtime.admitted_workers.reset();
            runtime.admitted_worker_slots = 0;
            reset_runtime_cleanup_state(runtime);
            erase_runtime();
            runtime.init_called.store(false, std::memory_order_release);
        }

        void ensure_runtime_scheduler(runtime_state& runtime)
        {
            if (!runtime.io_uring_scheduler)
                runtime.io_uring_scheduler = rpc::io_uring::io_uring_scheduler::create(runtime.scheduler);
            runtime.scheduler = runtime.io_uring_scheduler->scheduler();
        }

        void stop_runtime(
            runtime_state& runtime,
            common::startup_status* startup_status,
            common::startup_state state,
            int error_code)
        {
            runtime.accepting_workers.store(false, std::memory_order_release);
            set_startup_status(startup_status, state, error_code);
            request_runtime_cleanup(runtime);
            drain_ready_scheduler_work(runtime, 1000, 100000);
            stop_worker_loops(runtime);
            reset_runtime_after_stop(runtime);
        }

    }

    void register_acceptor_factory(acceptor_factory factory)
    {
        runtime_storage().acceptor_factory = std::move(factory);
    }

    void mark_runtime_connection_established()
    {
        auto& runtime = runtime_storage();
        runtime.connection_established.store(true, std::memory_order_release);
    }

    CORO_TASK(runtime_io_uring_controller_result)
    get_or_create_runtime_io_uring_controller(
        std::shared_ptr<host_transport> transport,
        rpc::coro::scheduler* scheduler)
    {
        runtime_io_uring_controller_result result{rpc::error::OK(), {}};
        if (!transport || !scheduler)
            CO_RETURN runtime_io_uring_controller_result{rpc::error::INVALID_DATA(), {}};

        auto& runtime = runtime_storage();
        if (!runtime.io_uring_scheduler)
            CO_RETURN runtime_io_uring_controller_result{rpc::error::ZONE_NOT_INITIALISED(), {}};

        if (auto controller = runtime.io_uring_scheduler->get_controller())
        {
            result.controller = std::move(controller);
            CO_RETURN result;
        }

        std::shared_ptr<rpc::io_uring::controller> controller;
        try
        {
            controller = std::make_shared<rpc::sgx::coro::enclave::enclave_io_uring_controller>(scheduler, transport);
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating enclave io_uring controller");
            std::terminate();
        }

        runtime.io_uring_scheduler->set_controller(controller);

        result.controller = std::move(controller);
        CO_RETURN result;
    }

    uint64_t runtime_ticks_per_millisecond() noexcept
    {
        auto& runtime = runtime_storage();
        return normalise_ticks_per_millisecond(runtime.ticks_per_millisecond.load(std::memory_order_acquire));
    }

    uint64_t read_runtime_tick_counter() noexcept
    {
#if defined(__x86_64__) || defined(__i386__)
        return __builtin_ia32_rdtsc();
#else
        return 0;
#endif
    }

    uint64_t runtime_unix_epoch_milliseconds() noexcept
    {
        auto& runtime = runtime_storage();
        const auto initial_time = runtime.initial_unix_epoch_milliseconds.load(std::memory_order_acquire);
        const auto initial_ticks = runtime.initial_tick_counter.load(std::memory_order_acquire);
        const auto current_ticks = read_runtime_tick_counter();
        const auto ticks_per_millisecond = runtime_ticks_per_millisecond();

        if (initial_time == 0 || current_ticks <= initial_ticks || ticks_per_millisecond == 0)
            return initial_time;

        return initial_time + ((current_ticks - initial_ticks) / ticks_per_millisecond);
    }

    uint64_t runtime_ticks_to_microseconds(uint64_t ticks) noexcept
    {
        const auto ticks_per_millisecond = runtime_ticks_per_millisecond();
        if (ticks == 0 || ticks_per_millisecond == 0)
            return 0;

        // Keep conversion out of benchmark code so logging and tests use
        // the same enclave runtime calibration.
        const auto converted = ((ticks / ticks_per_millisecond) * 1000ULL)
                               + (((ticks % ticks_per_millisecond) * 1000ULL) / ticks_per_millisecond);
        return converted == 0 ? 1 : converted;
    }

    uint64_t runtime_ticks_to_nanoseconds(uint64_t ticks) noexcept
    {
        const auto ticks_per_millisecond = runtime_ticks_per_millisecond();
        if (ticks == 0 || ticks_per_millisecond == 0)
            return 0;

        const auto converted = ((ticks / ticks_per_millisecond) * 1000000ULL)
                               + (((ticks % ticks_per_millisecond) * 1000000ULL) / ticks_per_millisecond);
        return converted == 0 ? 1 : converted;
    }

    extern "C"
    {
        int coroutine_init_enclave(
            size_t req_sz,
            const char* req,
            void* host_to_enclave_queue,
            void* enclave_to_host_queue,
            uint64_t ticks_per_millisecond,
            uint64_t initial_unix_epoch_milliseconds,
            canopy_coroutine_startup_status* startup_status_pointer,
            size_t resp_cap,
            char* resp,
            size_t* resp_sz)
        {
            bool expected = false;
            auto& runtime = runtime_storage();
            if (!runtime.init_called.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return rpc::error::FRAUDULANT_REQUEST();

            if (!validate_ecall_blob(req, req_sz))
            {
                runtime.init_called.store(false, std::memory_order_release);
                return rpc::error::FRAUDULANT_REQUEST();
            }

            protocol::init_request request{};
            auto err = rpc::sgx::coro::enclave::from_blob(
                rpc::byte_span{reinterpret_cast<const uint8_t*>(req), req_sz}, request);
            if (err != rpc::error::OK())
            {
                runtime.init_called.store(false, std::memory_order_release);
                return err;
            }

            auto* startup_status = validate_startup_status_pointer(startup_status_pointer);
            if (!startup_status)
            {
                runtime.init_called.store(false, std::memory_order_release);
                return rpc::error::SECURITY_ERROR();
            }
            if (!rpc::sgx::coro::enclave::validate_shared_queue_pointer(host_to_enclave_queue)
                || !rpc::sgx::coro::enclave::validate_shared_queue_pointer(enclave_to_host_queue))
            {
                runtime.init_called.store(false, std::memory_order_release);
                return rpc::error::SECURITY_ERROR();
            }
            if (host_to_enclave_queue == enclave_to_host_queue)
            {
                runtime.init_called.store(false, std::memory_order_release);
                return rpc::error::FRAUDULANT_REQUEST();
            }

            ensure_runtime_scheduler(runtime);
            runtime.ticks_per_millisecond.store(
                normalise_ticks_per_millisecond(ticks_per_millisecond), std::memory_order_release);
            runtime.initial_tick_counter.store(read_runtime_tick_counter(), std::memory_order_release);
            runtime.initial_unix_epoch_milliseconds.store(initial_unix_epoch_milliseconds, std::memory_order_release);
#ifdef CANOPY_USE_TELEMETRY
            rpc::telemetry::create_coro_enclave_telemetry_service(rpc::telemetry::telemetry_service_);
#endif
            runtime.enclave_zone = request.enclave_zone_id;
            runtime.control_status = startup_status;
            auto register_error = rpc::sgx::coro::enclave::register_runtime(runtime);
            if (register_error != rpc::error::OK())
            {
                set_startup_status(startup_status, common::startup_state::failed, register_error);
                runtime.init_called.store(false, std::memory_order_release);
                return register_error;
            }

            const auto requested_workers = static_cast<uint32_t>(runtime.scheduler->worker_count());
            runtime.admitted_workers = std::make_unique<std::atomic<bool>[]>(requested_workers);
            runtime.admitted_worker_slots = requested_workers;
            for (uint32_t worker_index = 0; worker_index < requested_workers; ++worker_index)
                runtime.admitted_workers[worker_index].store(false, std::memory_order_relaxed);
            runtime.requested_workers.store(requested_workers, std::memory_order_release);
            runtime.attached_workers.store(0, std::memory_order_release);
            runtime.accepting_workers.store(true, std::memory_order_release);
            set_worker_request(startup_status, requested_workers, 0);

            auto worker_error = wait_for_workers(runtime, requested_workers);
            runtime.accepting_workers.store(false, std::memory_order_release);
            if (worker_error != rpc::error::OK())
            {
                stop_runtime(runtime, startup_status, common::startup_state::failed, worker_error);
                return worker_error;
            }

            if (!runtime.acceptor_factory)
            {
                stop_runtime(runtime, startup_status, common::startup_state::failed, rpc::error::INCOMPATIBLE_SERVICE());
                return rpc::error::INCOMPATIBLE_SERVICE();
            }

            std::shared_ptr<rpc::enclave_service> service;
            try
            {
                service = std::make_shared<rpc::enclave_service>(
                    "sgx_coroutine_enclave", request.enclave_zone_id, request.host_zone_id, runtime.scheduler);
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while creating enclave service");
                std::terminate();
            }
            // auto service_shutdown_event = std::make_shared<rpc::event>(false);
            // service_shutdown_event->set_scheduler(runtime.scheduler.get());
            // service->set_shutdown_event(service_shutdown_event);

            auto stream = std::make_shared<streaming::spsc_queue::stream>(
                static_cast<common::queue_type*>(enclave_to_host_queue),
                static_cast<common::queue_type*>(host_to_enclave_queue),
                runtime.scheduler);

            rpc::stream_transport::stream_transport_options transport_options{
                .call_timeout = std::chrono::milliseconds{0},
                .call_timeout_sweep = std::chrono::milliseconds{0},
            };
            auto host_runtime_transport = host_transport::create(
                "sgx_coroutine_enclave", service, std::move(stream), runtime.acceptor_factory, transport_options);
            if (!host_runtime_transport)
            {
                stop_runtime(runtime, startup_status, common::startup_state::failed, rpc::error::TRANSPORT_ERROR());
                return rpc::error::TRANSPORT_ERROR();
            }
            service->set_parent_transport(host_runtime_transport);
            host_runtime_transport->set_runtime_destroyed_handler(
                [&runtime
                    // , service_shutdown_event
            ]()
                {
                    runtime.host_transport_destroyed.store(true, std::memory_order_release);
                    // service_shutdown_event->set();
                });

            runtime.log_transport = host_runtime_transport;
            // auto io_loop_error
            //     = wait_for_transport_io_loops(runtime, host_runtime_transport, std::chrono::milliseconds{20000});
            // if (io_loop_error != rpc::error::OK())
            // {
            //     // the transport may be trashed so stop all threads and trash this enclave
            //     //  if (host_runtime_transport->get_status() < rpc::transport_status::DISCONNECTED)
            //     //      host_runtime_transport->set_status(rpc::transport_status::DISCONNECTED);
            //     stop_runtime(runtime, startup_status, common::startup_state::failed, io_loop_error);
            //     return io_loop_error;
            // }

            set_startup_status(startup_status, common::startup_state::runtime_ready, rpc::error::OK());

            std::weak_ptr<host_transport> weak_transport = host_runtime_transport;
            host_runtime_transport.reset();
            while (!runtime.connection_established.load(std::memory_order_acquire)
                   && !runtime.host_transport_destroyed.load(std::memory_order_acquire) && !shutdown_requested(runtime))
            {
                runtime.scheduler->process_events();
                cpu_relax();
            }
            service.reset();
            auto loop_error = rpc::sgx::coro::enclave::run_runtime_loop(
                runtime, weak_transport
                // , service_shutdown_event
            );

            request_runtime_cleanup(runtime);
            drain_ready_scheduler_work(runtime, 1000, 100000);

            // The host owns the SPSC queues passed through the ECALL. Once the
            // host-facing transport is disconnecting, the enclave must let the
            // stream protocol drain normally instead of touching those raw queue
            // pointers during late teardown.
            drain_ready_scheduler_work(runtime, 1000, 100000);
            drain_ready_scheduler_work(runtime, 1000, 100000);
            drain_until_host_transport_released(runtime, weak_transport);
            stop_runtime(runtime, startup_status, common::startup_state::stopped, loop_error);

            return rpc::sgx::coro::enclave::write_blob_response(
                protocol::init_response{loop_error, {}}, resp_cap, resp, resp_sz);
        }

        int coroutine_enter_thread(
            size_t req_sz,
            const char* req)
        {
            if (!validate_ecall_blob(req, req_sz))
                return rpc::error::FRAUDULANT_REQUEST();

            protocol::enter_thread_request request{};
            auto err = rpc::sgx::coro::enclave::from_blob(
                rpc::byte_span{reinterpret_cast<const uint8_t*>(req), req_sz}, request);
            if (err != rpc::error::OK())
                return err;

            auto* runtime_ptr = rpc::sgx::coro::enclave::find_runtime();
            if (!runtime_ptr)
                return rpc::error::FRAUDULANT_REQUEST();
            auto& runtime = *runtime_ptr;
            if (!runtime.scheduler)
            {
                set_startup_status(runtime.control_status, common::startup_state::failed, rpc::error::FRAUDULANT_REQUEST());
                return rpc::error::FRAUDULANT_REQUEST();
            }

            if (request.enclave_zone_id != runtime.enclave_zone)
            {
                set_startup_status(runtime.control_status, common::startup_state::failed, rpc::error::FRAUDULANT_REQUEST());
                return rpc::error::FRAUDULANT_REQUEST();
            }

            if (!runtime.accepting_workers.load(std::memory_order_acquire))
            {
                set_startup_status(runtime.control_status, common::startup_state::failed, rpc::error::FRAUDULANT_REQUEST());
                return rpc::error::FRAUDULANT_REQUEST();
            }

            auto requested = runtime.requested_workers.load(std::memory_order_acquire);
            if (request.worker_index >= requested || request.worker_index >= runtime.admitted_worker_slots
                || !runtime.admitted_workers)
            {
                set_startup_status(runtime.control_status, common::startup_state::failed, rpc::error::FRAUDULANT_REQUEST());
                return rpc::error::FRAUDULANT_REQUEST();
            }

            bool expected = false;
            if (!runtime.admitted_workers[request.worker_index].compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel))
            {
                set_startup_status(runtime.control_status, common::startup_state::failed, rpc::error::FRAUDULANT_REQUEST());
                return rpc::error::FRAUDULANT_REQUEST();
            }

            auto new_attached = runtime.attached_workers.fetch_add(1, std::memory_order_acq_rel) + 1;
            set_attached_workers(runtime.control_status, new_attached);

            auto worker_error = rpc::sgx::coro::enclave::run_worker_loop(runtime, request.worker_index);
            auto remaining = runtime.attached_workers.fetch_sub(1, std::memory_order_acq_rel) - 1;
            set_attached_workers(runtime.control_status, remaining);
            return worker_error;
        }

        sgx_status_t rpc_log(
            int level,
            const char* str,
            size_t sz)
        {
            auto& runtime = runtime_storage();
            if (shutdown_requested(runtime))
                return SGX_SUCCESS;

            if (auto transport = runtime.log_transport.lock())
            {
                if (transport->get_status() != rpc::transport_status::CONNECTED)
                    return SGX_SUCCESS;

                std::vector<char> payload;
                if (str && sz > 0)
                    payload.assign(str, str + sz);
                rpc::telemetry_event event{transport->get_zone_id(),
                    rpc::id<rpc::log_record>::get(rpc::get_version()),
                    rpc::to_yas_binary<std::vector<char>>(
                        rpc::log_record{static_cast<uint64_t>(level), std::string(payload.begin(), payload.end())})};
                if (auto service = transport->get_service())
                {
                    service->spawn(transport->post_report(std::move(event)));
                    return SGX_SUCCESS;
                }
            }
            return SGX_SUCCESS;
        }
    }
}
