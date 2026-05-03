/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/common/startup_status.h>
#include <transports/sgx_coroutine/common/shared_queue.h>
#include <transports/sgx_coroutine/common/telemetry.h>
#include <transports/sgx_coroutine/host/runtime.h>
#include <edl/canopy_coroutine_enclave.h>
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
#include <memory>
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

namespace rpc::sgx::coro::host
{
    namespace
    {
        namespace protocol = rpc::sgx::coro::protocol;

        struct runtime_state
        {
            std::shared_ptr<::rpc::coro::scheduler> scheduler;
            std::weak_ptr<rpc::transport> transport;
            std::weak_ptr<rpc::stream_transport::transport> log_transport;
            rpc::zone enclave_zone{};
            std::atomic<uint32_t> requested_workers{0};
            std::atomic<uint32_t> attached_workers{0};
            std::atomic<bool> accepting_workers{false};
            std::atomic<bool> init_called{false};
            std::unique_ptr<std::atomic<bool>[]> admitted_workers;
            uint32_t admitted_worker_slots = 0;
            std::atomic<bool> registered{false};
            rpc::connection_handler connection_handler;
            acceptor_factory acceptor_factory;
            common::startup_status* control_status = nullptr;
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
            auto* status = runtime.control_status;
            if (!status)
                return true;

            auto state = static_cast<common::startup_state>(common::startup_load_u32(&status->state));
            return state == common::startup_state::failed || state == common::startup_state::stopped;
        }

        void signal_peer_closed(common::queue_type* send_queue)
        {
            if (!send_queue)
                return;

            streaming::spsc_queue::blob close_blob{};
            for (size_t attempt = 0; attempt < 10000; ++attempt)
            {
                if (send_queue->push(close_blob))
                    break;
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#endif
            }
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

        int wait_for_transport_io_loops(
            runtime_state& runtime,
            const std::shared_ptr<rpc::stream_transport::transport>& transport,
            std::chrono::milliseconds timeout)
        {
            if (!runtime.scheduler || !transport)
                return rpc::error::ZONE_NOT_INITIALISED();

            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!transport->io_loops_started())
            {
                if (shutdown_requested(runtime))
                    return rpc::error::CALL_CANCELLED();
                if (std::chrono::steady_clock::now() >= deadline)
                    return rpc::error::CALL_TIMEOUT();

                runtime.scheduler->process_events();
                cpu_relax();
            }

            return rpc::error::OK();
        }

        int run_runtime_loop(
            runtime_state& runtime,
            std::weak_ptr<rpc::service> weak_service,
            const std::shared_ptr<rpc::event>& service_shutdown_event)
        {
            if (!runtime.scheduler || runtime.transport.expired())
                return rpc::error::ZONE_NOT_INITIALISED();

            bool shutdown_phase = false;
            while (true)
            {
                auto transport = runtime.transport.lock();

                if (!transport && !shutdown_phase)
                    break;

                const bool transport_done = !transport || transport->get_status() >= rpc::transport_status::DISCONNECTED;
                const bool service_expired = weak_service.expired();

                if ((shutdown_requested(runtime) || transport_done) && !shutdown_phase)
                {
                    shutdown_phase = true;
                    if (service_shutdown_event)
                        service_shutdown_event->set();
                    if (transport && transport->get_status() < rpc::transport_status::DISCONNECTING)
                        transport->set_status(rpc::transport_status::DISCONNECTING);
                }

                if (transport_done && service_expired)
                    break;

                transport.reset();
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

        void drain_ready_scheduler_work_while_service_lives(
            runtime_state& runtime,
            std::weak_ptr<rpc::service>& weak_service,
            int idle_target,
            int max_iterations)
        {
            if (!runtime.scheduler)
                return;

            for (int idle_iterations = 0, total_iterations = 0;
                !weak_service.expired() && idle_iterations < idle_target && total_iterations < max_iterations;
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
            if (runtime.scheduler)
                runtime.scheduler->shutdown();
            runtime.scheduler.reset();
            runtime.transport.reset();
            runtime.log_transport.reset();
            runtime.enclave_zone = {};
            runtime.requested_workers.store(0, std::memory_order_release);
            runtime.attached_workers.store(0, std::memory_order_release);
            runtime.accepting_workers.store(false, std::memory_order_release);
            runtime.control_status = nullptr;
            runtime.admitted_workers.reset();
            runtime.admitted_worker_slots = 0;
            erase_runtime();
            runtime.init_called.store(false, std::memory_order_release);
        }

        void stop_runtime(
            runtime_state& runtime,
            common::startup_status* startup_status,
            common::startup_state state,
            int error_code)
        {
            runtime.accepting_workers.store(false, std::memory_order_release);
            set_startup_status(startup_status, state, error_code);
            wait_for_worker_loops_to_exit(runtime);
            reset_runtime_after_stop(runtime);
        }

    }

    void register_connection_handler(rpc::connection_handler handler)
    {
        runtime_storage().connection_handler = std::move(handler);
    }

    rpc::connection_handler get_connection_handler()
    {
        return runtime_storage().connection_handler;
    }

    void register_acceptor_factory(acceptor_factory factory)
    {
        runtime_storage().acceptor_factory = std::move(factory);
    }

    acceptor_factory get_acceptor_factory()
    {
        return runtime_storage().acceptor_factory;
    }

    extern "C"
    {
        int canopy_coroutine_init_enclave(
            size_t req_sz,
            const char* req,
            void* host_to_enclave_queue,
            void* enclave_to_host_queue,
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
            auto err
                = rpc::sgx::coro::host::from_blob(rpc::byte_span{reinterpret_cast<const uint8_t*>(req), req_sz}, request);
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
            if (!rpc::sgx::coro::host::validate_shared_queue_pointer(host_to_enclave_queue)
                || !rpc::sgx::coro::host::validate_shared_queue_pointer(enclave_to_host_queue))
            {
                runtime.init_called.store(false, std::memory_order_release);
                return rpc::error::SECURITY_ERROR();
            }
            if (host_to_enclave_queue == enclave_to_host_queue)
            {
                runtime.init_called.store(false, std::memory_order_release);
                return rpc::error::FRAUDULANT_REQUEST();
            }

            if (!runtime.scheduler)
                runtime.scheduler = ::rpc::coro::make_shared_scheduler();
#ifdef CANOPY_USE_TELEMETRY
            rpc::telemetry::create_coro_enclave_telemetry_service(rpc::telemetry::telemetry_service_);
#endif
            runtime.enclave_zone = request.enclave_zone_id;
            runtime.control_status = startup_status;
            auto register_error = rpc::sgx::coro::host::register_runtime(runtime);
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

            auto service = rpc::root_service::create("sgx_coroutine_enclave", request.enclave_zone_id, runtime.scheduler);
#ifdef CANOPY_USE_TELEMETRY
            if (rpc::telemetry::telemetry_service_)
            {
                rpc::telemetry::telemetry_service_->on_service_creation(
                    {"sgx_coroutine_enclave", request.enclave_zone_id, rpc::destination_zone()});
            }
#endif
            auto service_shutdown_event = std::make_shared<rpc::event>(false);
            service_shutdown_event->set_scheduler(runtime.scheduler.get());
            service->set_shutdown_event(service_shutdown_event);

            auto stream = std::make_shared<streaming::spsc_queue::stream>(
                static_cast<common::queue_type*>(enclave_to_host_queue),
                static_cast<common::queue_type*>(host_to_enclave_queue),
                runtime.scheduler);

            auto transport = runtime.acceptor_factory("sgx_coroutine_enclave", service, stream);
            if (!transport)
            {
                stop_runtime(runtime, startup_status, common::startup_state::failed, rpc::error::TRANSPORT_ERROR());
                return rpc::error::TRANSPORT_ERROR();
            }

            runtime.transport = transport;
            runtime.log_transport = transport;
            auto io_loop_error = wait_for_transport_io_loops(runtime, transport, std::chrono::milliseconds{20000});
            if (io_loop_error != rpc::error::OK())
            {
                if (transport->get_status() < rpc::transport_status::DISCONNECTED)
                    std::static_pointer_cast<rpc::transport>(transport)->set_status(rpc::transport_status::DISCONNECTED);
                stop_runtime(runtime, startup_status, common::startup_state::failed, io_loop_error);
                return io_loop_error;
            }

            set_startup_status(startup_status, common::startup_state::runtime_ready, rpc::error::OK());
            std::weak_ptr<rpc::service> weak_service = service;
            transport.reset();
            service.reset();

            auto loop_error = rpc::sgx::coro::host::run_runtime_loop(runtime, weak_service, service_shutdown_event);

            drain_ready_scheduler_work(runtime, 1000, 100000);

            if (auto current_transport = runtime.transport.lock();
                current_transport && current_transport->get_status() < rpc::transport_status::DISCONNECTED)
            {
                signal_peer_closed(static_cast<common::queue_type*>(enclave_to_host_queue));
                current_transport->set_status(rpc::transport_status::DISCONNECTED);
            }
            drain_ready_scheduler_work_while_service_lives(runtime, weak_service, 1000, 100000);
            stop_runtime(runtime, startup_status, common::startup_state::stopped, loop_error);

            return rpc::sgx::coro::host::write_blob_response(
                protocol::init_response{loop_error, {}}, resp_cap, resp, resp_sz);
        }

        int canopy_coroutine_enter_thread(
            size_t req_sz,
            const char* req)
        {
            if (!validate_ecall_blob(req, req_sz))
                return rpc::error::FRAUDULANT_REQUEST();

            protocol::enter_thread_request request{};
            auto err
                = rpc::sgx::coro::host::from_blob(rpc::byte_span{reinterpret_cast<const uint8_t*>(req), req_sz}, request);
            if (err != rpc::error::OK())
                return err;

            auto* runtime_ptr = rpc::sgx::coro::host::find_runtime();
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

            auto worker_error = rpc::sgx::coro::host::run_worker_loop(runtime, request.worker_index);
            runtime.attached_workers.fetch_sub(1, std::memory_order_acq_rel);
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
