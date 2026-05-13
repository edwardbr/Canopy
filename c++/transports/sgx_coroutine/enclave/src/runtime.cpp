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
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#if defined(CANOPY_FAKE_SGX)
#  include <unistd.h>
#endif
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
    uint64_t read_runtime_tick_counter() noexcept;

    namespace
    {
        namespace protocol = rpc::sgx::coro::protocol;

        struct runtime_state
        {
            // Execution resources created by the init ECALL and owned until
            // the master ECALL tears the enclave runtime down.
            std::shared_ptr<rpc::io_uring::io_uring_scheduler> io_uring_scheduler;
            std::shared_ptr<::rpc::coro::scheduler> scheduler;

            // The enclave-side transport back to the host. This is kept weak
            // so runtime_state cannot keep the RPC graph alive by itself. The
            // pointer is published once per runtime generation; shutdown only
            // clears the ready flag so late logging observes best-effort
            // availability without mutating the weak_ptr during teardown.
            std::weak_ptr<host_transport> parent_transport;
            std::atomic<bool> parent_transport_ready{false};

            // Bootstrap identity and worker admission state shared between the
            // master init ECALL and worker ECALLs.
            rpc::zone enclave_zone{};
            std::atomic<uint32_t> requested_workers{0};
            std::atomic<uint32_t> attached_workers{0};
            std::atomic<bool> accepting_workers{false};
            std::atomic<bool> init_called{false};
            std::unique_ptr<std::atomic<bool>[]> admitted_workers;
            uint32_t admitted_worker_slots = 0;
            std::atomic<bool> registered{false};
            acceptor_factory acceptor_factory;

            // Host-owned startup words. They are not enclave-owned storage;
            // they are validated once during init and then used only for
            // low-level state publication.
            protocol::startup_state* startup_state = nullptr;
            error_code* startup_error_code = nullptr;

            // Runtime lifetime flags. The host asks for shutdown by changing
            // startup_state. Internal cleanup state prevents duplicate
            // controller shutdown and tracks when the parent transport has
            // released its final strong reference.
            std::atomic<bool> cleanup_requested{false};
            std::atomic<bool> connection_established{false};
            std::atomic<bool> host_transport_destroyed{false};
            std::atomic<bool> stop_workers{false};

            // The SGX chrono polyfill owns wall-clock calibration. The init
            // ECALL seeds it with an untrusted host bootstrap value; a later
            // attested time synchronisation path can replace that calibration
            // without changing runtime_state layout.
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

        void* pointer_from_request_address(uint64_t address) noexcept
        {
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
        }

        rpc::encoding normalise_request_encoding(uint64_t request_encoding) noexcept
        {
            if (request_encoding == 0)
                return rpc::encoding::yas_binary;
            return static_cast<rpc::encoding>(request_encoding);
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

        template<typename T>
        int decode_yas_blob(
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

        bool validate_shared_queue_pointer(const void* queue_ptr)
        {
            if (!queue_ptr)
                return false;

            if (!is_aligned(queue_ptr, alignof(common::queue_type)))
                return false;

            return sgx_is_outside_enclave(queue_ptr, sizeof(common::queue_type)) != 0;
        }

        template<typename T> auto validate_outside_word_pointer(T* pointer) -> T*
        {
            if (!pointer)
                return nullptr;

            if (!is_aligned(pointer, alignof(T)))
                return nullptr;

            if (sgx_is_outside_enclave(pointer, sizeof(T)) == 0)
                return nullptr;

            return pointer;
        }

        struct validated_init_request
        {
            protocol::init_request request{};
            common::queue_type* host_to_enclave_queue = nullptr;
            common::queue_type* enclave_to_host_queue = nullptr;
            protocol::startup_state* startup_state = nullptr;
            error_code* startup_error_code = nullptr;
            int error_code = rpc::error::OK();

            [[nodiscard]] auto has_startup_status() const noexcept -> bool
            {
                return startup_state && startup_error_code;
            }

            [[nodiscard]] auto ok() const noexcept -> bool { return error_code == rpc::error::OK(); }
        };

        int decode_init_request_blob(
            uint64_t request_encoding,
            uint64_t request_type_id,
            const char* req,
            size_t req_sz,
            protocol::init_request& request)
        {
            // request_type_id is the schema fingerprint. request_encoding is
            // the byte format. Keeping them separate lets the ECALL ABI stay
            // stable while future init payloads move to another encoding.
            if (request_type_id != rpc::id<protocol::init_request>::get(rpc::get_version()))
                return rpc::error::INVALID_DATA();

            auto decode_error = rpc::deserialise(
                normalise_request_encoding(request_encoding),
                rpc::byte_span{reinterpret_cast<const uint8_t*>(req), req_sz},
                request);
            if (!decode_error.empty())
            {
                RPC_ERROR("sgx_coroutine init decode failed: {}", decode_error);
                return rpc::error::INVALID_DATA();
            }

            return rpc::error::OK();
        }

        auto validate_init_request_memory(const protocol::init_request& request) -> validated_init_request
        {
            validated_init_request result{};
            result.request = request;
            result.host_to_enclave_queue
                = static_cast<common::queue_type*>(pointer_from_request_address(request.host_to_enclave_queue_ptr));
            result.enclave_to_host_queue
                = static_cast<common::queue_type*>(pointer_from_request_address(request.enclave_to_host_queue_ptr));
            result.startup_state = validate_outside_word_pointer(request.state);
            result.startup_error_code = validate_outside_word_pointer(request.error);

            // Without valid startup words there is nowhere safe to publish a
            // failure state. The host will time out, which is preferable to
            // writing to untrusted memory that has not been validated.
            if (!result.has_startup_status())
            {
                result.error_code = rpc::error::INVALID_DATA();
                return result;
            }

            if (!validate_shared_queue_pointer(result.host_to_enclave_queue)
                || !validate_shared_queue_pointer(result.enclave_to_host_queue))
            {
                result.error_code = rpc::error::SECURITY_ERROR();
                return result;
            }

            if (result.host_to_enclave_queue == result.enclave_to_host_queue)
            {
                result.error_code = rpc::error::FRAUDULANT_REQUEST();
                return result;
            }

            return result;
        }

        bool shutdown_requested(runtime_state& runtime)
        {
            auto* startup_state = runtime.startup_state;
            if (!startup_state)
                return false;

            auto state = common::startup_load_state(startup_state);
            return state == protocol::startup_state::shutting_down || state == protocol::startup_state::stopped;
        }

        bool runtime_stopped(runtime_state& runtime)
        {
            if (runtime.stop_workers.load(std::memory_order_acquire))
                return true;

            auto* startup_state = runtime.startup_state;
            if (!startup_state)
                return true;

            auto state = common::startup_load_state(startup_state);
            return state == protocol::startup_state::failed || state == protocol::startup_state::shutting_down
                   || state == protocol::startup_state::stopped;
        }

        void set_startup_status(
            protocol::startup_state* startup_state,
            error_code* startup_error_code,
            protocol::startup_state state,
            int error_code)
        {
            if (!startup_state || !startup_error_code)
                return;

            common::startup_store_error(startup_error_code, error_code);
            common::startup_store_state(startup_state, state);
        }

        void set_worker_request(
            protocol::startup_state* startup_state,
            error_code* startup_error_code)
        {
            set_startup_status(
                startup_state, startup_error_code, protocol::startup_state::workers_requested, rpc::error::OK());
        }

        int reject_worker_entry(runtime_state& runtime)
        {
            set_startup_status(
                runtime.startup_state,
                runtime.startup_error_code,
                protocol::startup_state::failed,
                rpc::error::FRAUDULANT_REQUEST());
            return rpc::error::FRAUDULANT_REQUEST();
        }

        void cpu_relax()
        {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }

        std::shared_ptr<host_transport> load_parent_transport(runtime_state& runtime)
        {
            if (!runtime.parent_transport_ready.load(std::memory_order_acquire))
                return {};

            return runtime.parent_transport.lock();
        }

        void publish_parent_transport(
            runtime_state& runtime,
            const std::shared_ptr<host_transport>& transport)
        {
            runtime.parent_transport = transport;
            runtime.parent_transport_ready.store(true, std::memory_order_release);
        }

        void unpublish_parent_transport(runtime_state& runtime)
        {
            runtime.parent_transport_ready.store(false, std::memory_order_release);
        }

        int idle_sleep(std::chrono::milliseconds duration)
        {
            if (duration <= std::chrono::milliseconds{0})
            {
                cpu_relax();
                return rpc::error::OK();
            }

#if defined(CANOPY_FAKE_SGX)
            if (::usleep(static_cast<useconds_t>(duration.count() * 1000)) != 0)
                return rpc::error::TRANSPORT_ERROR();
#else
            const auto count = duration.count();
            const auto milliseconds
                = static_cast<uint32_t>(count > static_cast<decltype(count)>(UINT32_MAX) ? UINT32_MAX : count);
            auto status = canopy_sgx_sleep_ms(milliseconds);
            if (status != SGX_SUCCESS)
                return rpc::error::TRANSPORT_ERROR();
#endif
            return rpc::error::OK();
        }

        int wait_for_workers(
            runtime_state& runtime,
            uint32_t requested_workers)
        {
            while (!shutdown_requested(runtime))
            {
                if (runtime.attached_workers.load(std::memory_order_acquire) >= requested_workers)
                    return rpc::error::OK();

                auto sleep_error = idle_sleep(std::chrono::milliseconds{1});
                if (sleep_error != rpc::error::OK())
                    return sleep_error;
            }

            return rpc::error::CALL_CANCELLED();
        }

        void drain_ready_scheduler_work(
            runtime_state& runtime,
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

        int drain_until_host_transport_released(
            runtime_state& runtime,
            const std::weak_ptr<host_transport>& weak_transport)
        {
            if (!runtime.scheduler)
                return rpc::error::OK();

            // The runtime must stay alive until the host-facing transport has
            // actually been destroyed. Service teardown can be the last owner
            // of the transport, so this wait belongs after service.reset().
            while (!host_transport_released(runtime, weak_transport))
            {
                if (!runtime.scheduler->process_ready_event())
                {
                    auto sleep_error = idle_sleep(std::chrono::milliseconds{1});
                    if (sleep_error != rpc::error::OK())
                        return sleep_error;
                }
            }
            return rpc::error::OK();
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

                if (!runtime.scheduler->process_ready_event())
                {
                    auto sleep_error = idle_sleep(std::chrono::milliseconds{1});
                    if (sleep_error != rpc::error::OK())
                        return sleep_error;
                }
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

        int wait_for_worker_loops_to_exit(runtime_state& runtime)
        {
            while (runtime.attached_workers.load(std::memory_order_acquire) > 0)
            {
                if (runtime.scheduler && runtime.scheduler->process_ready_event())
                    continue;

                auto sleep_error = idle_sleep(std::chrono::milliseconds{1});
                if (sleep_error != rpc::error::OK())
                    return sleep_error;
            }
            return rpc::error::OK();
        }

        int stop_worker_loops(runtime_state& runtime)
        {
            // The master ECALL owns runtime teardown. Once service and
            // transport cleanup has drained, ask worker ECALLs to leave the
            // scheduler before runtime state is reset.
            runtime.stop_workers.store(true, std::memory_order_release);
            return wait_for_worker_loops_to_exit(runtime);
        }

        void drain_ready_scheduler_work(
            runtime_state& runtime,
            int max_iterations)
        {
            if (!runtime.scheduler)
                return;

            for (int total_iterations = 0; total_iterations < max_iterations; ++total_iterations)
            {
                if (!runtime.scheduler->process_ready_event())
                    return;
            }
        }

        void reset_runtime_after_stop(runtime_state& runtime)
        {
            // This is the only place that clears runtime_state back to its
            // reusable baseline. It runs after parent transport cleanup and
            // worker exit so no later ECALL should observe partially reset
            // scheduler or startup pointers.
            unpublish_parent_transport(runtime);
            if (runtime.io_uring_scheduler)
                runtime.io_uring_scheduler->shutdown();
            else if (runtime.scheduler)
                runtime.scheduler->shutdown();
            runtime.io_uring_scheduler.reset();
            runtime.scheduler.reset();
            runtime.enclave_zone = {};
            runtime.requested_workers.store(0, std::memory_order_release);
            runtime.attached_workers.store(0, std::memory_order_release);
            runtime.accepting_workers.store(false, std::memory_order_release);
            std::chrono::sgx_reset_system_clock();
            runtime.startup_state = nullptr;
            runtime.startup_error_code = nullptr;
            runtime.admitted_workers.reset();
            runtime.admitted_worker_slots = 0;
            reset_runtime_cleanup_state(runtime);
            erase_runtime();
            runtime.init_called.store(false, std::memory_order_release);
        }

        void ensure_runtime_scheduler(
            runtime_state& runtime,
            uint32_t worker_thread_count)
        {
            if (!runtime.scheduler)
            {
                auto execution_strategy = worker_thread_count == 0
                                              ? ::rpc::coro::sgx::execution_strategy_t::process_tasks_inline
                                              : ::rpc::coro::sgx::execution_strategy_t::process_tasks_on_thread_pool;
                auto scheduler_options = ::rpc::coro::sgx::scheduler::options{worker_thread_count,
                    nullptr,
                    nullptr,
                    ::rpc::coro::sgx::thread_pool::options{worker_thread_count},
                    execution_strategy,
                    worker_thread_count != 0};
                runtime.scheduler = ::rpc::coro::make_shared_scheduler(std::move(scheduler_options));
            }
            if (!runtime.io_uring_scheduler)
                runtime.io_uring_scheduler = rpc::io_uring::io_uring_scheduler::create(runtime.scheduler);
            runtime.scheduler = runtime.io_uring_scheduler->scheduler();
        }

        int initialise_runtime_from_request(
            runtime_state& runtime,
            const validated_init_request& init)
        {
            ensure_runtime_scheduler(runtime, init.request.worker_thread_count);
            std::chrono::sgx_seed_system_clock_from_untrusted_host(
                init.request.initial_unix_epoch_milliseconds, read_runtime_tick_counter(), init.request.ticks_per_millisecond);
#ifdef CANOPY_USE_TELEMETRY
            rpc::telemetry::create_coro_enclave_telemetry_service(rpc::telemetry::telemetry_service_);
#endif
            runtime.enclave_zone = init.request.enclave_zone_id;
            runtime.startup_state = init.startup_state;
            runtime.startup_error_code = init.startup_error_code;
            return rpc::sgx::coro::enclave::register_runtime(runtime);
        }

        void prepare_worker_admission(
            runtime_state& runtime,
            uint32_t requested_workers)
        {
            runtime.admitted_workers = std::make_unique<std::atomic<bool>[]>(requested_workers);
            runtime.admitted_worker_slots = requested_workers;
            for (uint32_t worker_index = 0; worker_index < requested_workers; ++worker_index)
                runtime.admitted_workers[worker_index].store(false, std::memory_order_relaxed);
            runtime.requested_workers.store(requested_workers, std::memory_order_release);
            runtime.attached_workers.store(0, std::memory_order_release);
            runtime.accepting_workers.store(true, std::memory_order_release);
        }

        auto create_enclave_service(
            const validated_init_request& init,
            const std::shared_ptr<rpc::coro::scheduler>& scheduler) -> std::shared_ptr<rpc::enclave_service>
        {
            try
            {
                return std::make_shared<rpc::enclave_service>(
                    "sgx_coroutine_enclave", init.request.enclave_zone_id, init.request.host_zone_id, scheduler);
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while creating enclave service");
                std::terminate();
            }
            return {};
        }

        auto create_parent_host_transport(
            runtime_state& runtime,
            const validated_init_request& init,
            const std::shared_ptr<rpc::enclave_service>& service) -> std::shared_ptr<host_transport>
        {
            auto stream = std::make_shared<streaming::spsc_queue::stream>(
                init.enclave_to_host_queue, init.host_to_enclave_queue, runtime.scheduler);

            rpc::stream_transport::stream_transport_options transport_options{
                .call_timeout = std::chrono::milliseconds{0},
                .call_timeout_sweep = std::chrono::milliseconds{0},
            };
            auto transport = host_transport::create(
                "sgx_coroutine_enclave", service, std::move(stream), runtime.acceptor_factory, transport_options);
            if (!transport)
                return {};

            service->set_parent_transport(transport);
            transport->set_runtime_destroyed_handler(
                [&runtime]() { runtime.host_transport_destroyed.store(true, std::memory_order_release); });
            return transport;
        }

        void stop_runtime(
            runtime_state& runtime,
            protocol::startup_state* startup_state,
            error_code* startup_error_code,
            protocol::startup_state state,
            int error_code)
        {
            // Stop is intentionally ordered: cancel controller-owned io_uring
            // work, drain ready cleanup continuations without fixed idle
            // sleeps, release worker ECALLs, then publish terminal host-visible
            // state before clearing runtime storage.
            runtime.accepting_workers.store(false, std::memory_order_release);
            request_runtime_cleanup(runtime);
            drain_ready_scheduler_work(runtime, 100000);
            const auto worker_stop_error = stop_worker_loops(runtime);
            const auto final_error = error_code != rpc::error::OK() ? error_code : worker_stop_error;
            set_startup_status(startup_state, startup_error_code, state, final_error);
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
        return std::chrono::sgx_clock_ticks_per_millisecond();
    }

    uint64_t read_runtime_tick_counter() noexcept
    {
        return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    uint64_t runtime_unix_epoch_milliseconds() noexcept
    {
        return static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
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
        // Master ECALL for the enclave RPC runtime. The host enters this once
        // per enclave instance and the call intentionally remains active until
        // transport shutdown. While it is active it owns:
        // - validating host-owned queue/startup pointers;
        // - creating the coroutine scheduler and parent host_transport;
        // - publishing startup transitions for the host;
        // - pumping inline scheduler work when no worker threads are used;
        // - coordinating final cleanup before the enclave can be destroyed.
        void coroutine_init_enclave(
            size_t req_sz,
            const char* req,
            uint64_t request_encoding,
            uint64_t request_type_id)
        {
            bool expected = false;
            auto& runtime = runtime_storage();
            if (!runtime.init_called.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return;

            if (!validate_ecall_blob(req, req_sz))
            {
                runtime.init_called.store(false, std::memory_order_release);
                return;
            }

            // Phase 1: decode the stable ECALL payload and validate every
            // host pointer before the enclave stores or dereferences it.
            protocol::init_request request{};
            auto decode_error = decode_init_request_blob(request_encoding, request_type_id, req, req_sz, request);
            if (decode_error != rpc::error::OK())
            {
                runtime.init_called.store(false, std::memory_order_release);
                return;
            }

            auto init = validate_init_request_memory(request);
            if (!init.ok())
            {
                if (init.has_startup_status())
                    set_startup_status(
                        init.startup_state, init.startup_error_code, protocol::startup_state::failed, init.error_code);
                runtime.init_called.store(false, std::memory_order_release);
                return;
            }

            auto* startup_state = init.startup_state;
            auto* startup_error_code = init.startup_error_code;

            // Phase 2: publish runtime identity. From this point worker ECALLs
            // can find the runtime, but they are not admitted until
            // accepting_workers is set below.
            auto register_error = initialise_runtime_from_request(runtime, init);
            if (register_error != rpc::error::OK())
            {
                set_startup_status(startup_state, startup_error_code, protocol::startup_state::failed, register_error);
                runtime.init_called.store(false, std::memory_order_release);
                return;
            }

            // Phase 3: ask the host to enter the worker ECALLs. A zero-worker
            // runtime runs inline on the master ECALL and this wait completes
            // immediately.
            const auto requested_workers = init.request.worker_thread_count;
            prepare_worker_admission(runtime, requested_workers);
            set_worker_request(startup_state, startup_error_code);

            auto worker_error = wait_for_workers(runtime, requested_workers);
            runtime.accepting_workers.store(false, std::memory_order_release);
            if (worker_error != rpc::error::OK())
            {
                stop_runtime(runtime, startup_state, startup_error_code, protocol::startup_state::failed, worker_error);
                return;
            }

            // Phase 4: build the enclave-side RPC graph. The host_transport is
            // the enclave's parent transport: it carries traffic back over the
            // host-owned SPSC queues supplied in init_request.
            if (!runtime.acceptor_factory)
            {
                stop_runtime(
                    runtime,
                    startup_state,
                    startup_error_code,
                    protocol::startup_state::failed,
                    rpc::error::INCOMPATIBLE_SERVICE());
                return;
            }

            auto service = create_enclave_service(init, runtime.scheduler);
            auto host_runtime_transport = create_parent_host_transport(runtime, init, service);
            if (!host_runtime_transport)
            {
                stop_runtime(
                    runtime, startup_state, startup_error_code, protocol::startup_state::failed, rpc::error::TRANSPORT_ERROR());
                return;
            }

            publish_parent_transport(runtime, host_runtime_transport);
            set_startup_status(startup_state, startup_error_code, protocol::startup_state::runtime_ready, rpc::error::OK());

            // Phase 5: drive the scheduler from the master ECALL. The initial
            // loop lets the host-side connect handshake complete before this
            // function drops its strong transport reference. After that, normal
            // runtime progress continues until the host requests shutdown or
            // the parent transport is destroyed.
            std::weak_ptr<host_transport> weak_transport = host_runtime_transport;
            host_runtime_transport.reset();
            while (!runtime.connection_established.load(std::memory_order_acquire)
                   && !runtime.host_transport_destroyed.load(std::memory_order_acquire) && !shutdown_requested(runtime))
            {
                if (!runtime.scheduler->process_ready_event())
                {
                    auto sleep_error = idle_sleep(std::chrono::milliseconds{1});
                    if (sleep_error != rpc::error::OK())
                    {
                        stop_runtime(
                            runtime, startup_state, startup_error_code, protocol::startup_state::failed, sleep_error);
                        return;
                    }
                }
            }
            service.reset();
            auto loop_error = rpc::sgx::coro::enclave::run_runtime_loop(runtime, weak_transport);

            // Phase 6: cleanup happens on the master ECALL. Worker ECALLs are
            // asked to leave only after parent transport cleanup has had a
            // chance to schedule its final work.
            request_runtime_cleanup(runtime);
            drain_ready_scheduler_work(runtime, 100000);

            // The host owns the SPSC queues passed through the ECALL. Once the
            // host-facing transport is disconnecting, the enclave must let the
            // stream protocol drain normally instead of touching those raw queue
            // pointers during late teardown.
            auto drain_error = drain_until_host_transport_released(runtime, weak_transport);
            if (loop_error == rpc::error::OK())
                loop_error = drain_error;
            stop_runtime(runtime, startup_state, startup_error_code, protocol::startup_state::stopped, loop_error);
        }

        // Worker ECALL. The host starts one of these per requested worker
        // thread after the master ECALL publishes workers_requested. Each
        // worker is admitted exactly once by index, then runs the scheduler's
        // worker loop until the master ECALL asks workers to stop during
        // cleanup.
        int coroutine_enter_thread(
            size_t req_sz,
            const char* req)
        {
            if (!validate_ecall_blob(req, req_sz))
                return rpc::error::FRAUDULANT_REQUEST();

            protocol::enter_thread_request request{};
            auto err = rpc::sgx::coro::enclave::decode_yas_blob(
                rpc::byte_span{reinterpret_cast<const uint8_t*>(req), req_sz}, request);
            if (err != rpc::error::OK())
                return err;

            auto* runtime_ptr = rpc::sgx::coro::enclave::find_runtime();
            if (!runtime_ptr)
                return rpc::error::FRAUDULANT_REQUEST();
            auto& runtime = *runtime_ptr;
            if (!runtime.scheduler)
                return reject_worker_entry(runtime);

            if (request.enclave_zone_id != runtime.enclave_zone)
                return reject_worker_entry(runtime);

            if (!runtime.accepting_workers.load(std::memory_order_acquire))
                return reject_worker_entry(runtime);

            auto requested = runtime.requested_workers.load(std::memory_order_acquire);
            if (request.worker_index >= requested || request.worker_index >= runtime.admitted_worker_slots
                || !runtime.admitted_workers)
                return reject_worker_entry(runtime);

            bool expected = false;
            if (!runtime.admitted_workers[request.worker_index].compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel))
                return reject_worker_entry(runtime);

            runtime.attached_workers.fetch_add(1, std::memory_order_acq_rel);

            auto worker_error = rpc::sgx::coro::enclave::run_worker_loop(runtime, request.worker_index);
            runtime.attached_workers.fetch_sub(1, std::memory_order_acq_rel);
            return worker_error;
        }

        // Logging OCALL entry point used by enclave logging macros. It is
        // deliberately best-effort: shutdown must not block because a late log
        // record cannot be delivered. The parent transport weak_ptr is
        // published once for a runtime generation; the ready flag prevents
        // logging from touching it before publication or after shutdown starts.
        sgx_status_t rpc_log(
            int level,
            const char* str,
            size_t sz)
        {
            auto& runtime = runtime_storage();

            if (auto transport = load_parent_transport(runtime))
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
