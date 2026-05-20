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
#include <new>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#ifndef CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG
#  define CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG SGX_DEBUG_FLAG
#endif

extern "C" void canopy_sgx_sleep_ms(uint32_t milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
}

namespace rpc::sgx::coro::host
{
    namespace
    {
        using namespace rpc::sgx::coro::protocol;

        constexpr rpc::encoding sgx_bootstrap_init_request_encoding = rpc::encoding::yas_binary;
        constexpr uint64_t sgx_bootstrap_init_request_encoding_value
            = static_cast<uint64_t>(sgx_bootstrap_init_request_encoding);
        constexpr size_t max_startup_option_count = 64;
        constexpr size_t max_startup_option_key_bytes = 128;
        constexpr size_t max_startup_option_value_bytes = 4096;
        constexpr size_t max_startup_options_total_bytes = 64U * 1024U;

        bool signal_peer_closed(common::queue_type* send_queue)
        {
            if (!send_queue)
                return false;

            streaming::spsc_queue::blob close_blob{};
            return send_queue->push(close_blob);
        }

        bool startup_state_matches(
            startup_state state,
            startup_state expected_state) noexcept
        {
            if (state == expected_state)
                return true;

            return expected_state == startup_state::workers_requested && state == startup_state::runtime_ready;
        }

        CORO_TASK(int)
        wait_for_startup_status(
            rpc::coro::scheduler_ptr scheduler,
            const startup_state* startup_state_word,
            const error_code* startup_error_code,
            startup_state expected_state,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
        {
            if (!startup_state_word || !startup_error_code)
                CO_RETURN rpc::error::INVALID_DATA();
            if (!scheduler)
                CO_RETURN rpc::error::TRANSPORT_ERROR();

            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                auto state = common::startup_load_state(startup_state_word);
                if (startup_state_matches(state, expected_state))
                    CO_RETURN rpc::error::OK();

                if (state == startup_state::failed)
                    CO_RETURN common::startup_load_error(startup_error_code);

                // This is host-side enclave bootstrap, before the stream pumps
                // are established. It must be driven by the service scheduler;
                // tests with manual schedulers must not wrap this connect path
                // in sync_wait() without also pumping scheduler events.
                CO_AWAIT scheduler->schedule_after(std::chrono::milliseconds{1});
            }

            RPC_ERROR("startup status: timed out waiting for readiness");
            CO_RETURN rpc::error::CALL_TIMEOUT();
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

        uint64_t host_pointer_address(const void* pointer) noexcept
        {
            static_assert(sizeof(std::uintptr_t) <= sizeof(uint64_t));
            return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
        }

        uint64_t host_unix_epoch_milliseconds() noexcept
        {
            const auto now = std::chrono::system_clock::now();
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        }

        void fail_startup_status(
            startup_state* startup_state_word,
            error_code* startup_error_code,
            int error_code)
        {
            if (!startup_state_word || !startup_error_code)
                return;

            common::startup_store_error(startup_error_code, error_code);
            common::startup_store_state(startup_state_word, startup_state::failed);
        }

        int validate_startup_options(const std::map<std::string, std::string>& options)
        {
            if (options.size() > max_startup_option_count)
                return rpc::error::RESOURCE_EXHAUSTED();

            size_t total_bytes = 0;
            for (const auto& [key, value] : options)
            {
                if (key.empty() || key.size() > max_startup_option_key_bytes
                    || value.size() > max_startup_option_value_bytes)
                {
                    return rpc::error::INVALID_DATA();
                }

                const auto entry_bytes = key.size() + value.size();
                if (total_bytes > max_startup_options_total_bytes - entry_bytes)
                    return rpc::error::RESOURCE_EXHAUSTED();
                total_bytes += entry_bytes;
            }

            return rpc::error::OK();
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

    void transport::enclave_owner::request_shutdown() const
    {
        auto state = state_;
        if (state)
        {
            auto current_state = common::startup_load_state(&state->startup_state_);
            if (current_state != startup_state::failed && current_state != startup_state::stopped)
                common::startup_store_state(&state->startup_state_, startup_state::shutting_down);
        }
    }

    bool transport::enclave_owner::is_current_thread(const std::thread& thread) noexcept
    {
        return thread.joinable() && thread.get_id() == std::this_thread::get_id();
    }

    void transport::enclave_owner::join_or_detach_if_current(std::thread& thread)
    {
        if (!thread.joinable())
            return;

        if (is_current_thread(thread))
        {
            thread.detach();
            return;
        }

        thread.join();
    }

    void transport::enclave_owner::join_worker_threads(const std::shared_ptr<thread_state>& state)
    {
        if (!state)
            return;

        for (auto& worker_thread : state->worker_threads_)
            join_or_detach_if_current(worker_thread);
    }

    void transport::enclave_owner::cleanup_threads_and_destroy_enclave(
        std::shared_ptr<thread_state> state,
        std::thread init_thread)
    {
        join_or_detach_if_current(init_thread);
        join_worker_threads(state);

        if (state && state->eid_ != 0)
        {
            sgx_destroy_enclave(state->eid_);
            state->eid_ = 0;
        }
    }

    transport::enclave_owner::~enclave_owner()
    {
        request_shutdown();

        auto state = state_;
        if (!state)
            return;

        if (init_thread_.joinable())
        {
            auto init_thread = std::move(init_thread_);
            cleanup_threads_and_destroy_enclave(std::move(state), std::move(init_thread));
            return;
        }

        join_worker_threads(state);
        if (state->eid_ != 0)
        {
            sgx_destroy_enclave(state->eid_);
            state->eid_ = 0;
        }
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

    transport::~transport()
    {
        // Most paths request disconnect before the transport graph releases its
        // final reference. This safety net must not join ECALL threads from a
        // destructor; it only requests shutdown and hands ownership to an async
        // cleanup thread.
        begin_enclave_shutdown_once();
    }

    void transport::on_destination_count_zero()
    {
        begin_clean_disconnect();
    }

    void transport::set_enclave_worker_thread_count(uint32_t worker_thread_count)
    {
        enclave_worker_thread_count_.store(worker_thread_count, std::memory_order_release);
    }

    int transport::set_enclave_startup_options(std::map<std::string, std::string> options)
    {
        auto validation_error = validate_startup_options(options);
        if (validation_error != rpc::error::OK())
            return validation_error;

        std::lock_guard lock(enclave_startup_options_mutex_);
        enclave_startup_options_ = std::move(options);
        return rpc::error::OK();
    }

    void transport::destroy_enclave_owner_async(
        std::shared_ptr<enclave_owner> owner,
        rpc::coro::scheduler_ptr scheduler) noexcept
    {
        if (!owner)
            return;

        if (scheduler)
        {
            auto destroy_owner = [](std::shared_ptr<enclave_owner> owner_to_destroy) -> CORO_TASK(void)
            {
                // Dropping this final owner reference joins ECALL threads and
                // calls sgx_destroy_enclave(). Schedule it off the caller so a
                // transport status change or destructor never performs that
                // blocking work inline.
                owner_to_destroy.reset();
                CO_RETURN;
            };

            try
            {
                auto owner_for_scheduler = owner;
                if (scheduler->spawn_detached(destroy_owner(std::move(owner_for_scheduler))))
                    return;
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while scheduling SGX enclave teardown");
                std::terminate();
            }
        }

        try
        {
            auto owner_for_thread = owner;
            std::thread(
                [owner = std::move(owner_for_thread)]() mutable
                {
                    // Destroying the owner joins ECALL threads and calls
                    // sgx_destroy_enclave(). That can legitimately block, so keep
                    // it off the stream pump/status caller whenever thread
                    // creation succeeds.
                    owner.reset();
                })
                .detach();
            return;
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while starting SGX enclave teardown thread");
            std::terminate();
        }
        catch (const std::system_error& ex)
        {
            RPC_WARNING("failed to start SGX enclave teardown thread: {}", ex.what());
            std::terminate();
        }
    }

    void transport::begin_enclave_shutdown_once() noexcept
    {
        bool expected = false;
        if (!enclave_shutdown_started_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return;
        }

        auto owner_to_stop = std::move(enclave_owner_);
        auto queue_stream = queue_stream_;
        auto host_to_enclave_queue = host_to_enclave_queue_;
        rpc::coro::scheduler_ptr scheduler;
        if (auto service = get_service())
            scheduler = service->get_scheduler();

        if (owner_to_stop)
            owner_to_stop->request_shutdown();

        if (queue_stream)
        {
            if (!signal_peer_closed(host_to_enclave_queue.get()))
                RPC_WARNING("failed to enqueue SGX coroutine peer-close marker during shutdown");
            queue_stream->close_now();
        }

        queue_stream_.reset();
        host_to_enclave_queue_.reset();
        enclave_to_host_queue_.reset();

        destroy_enclave_owner_async(std::move(owner_to_stop), std::move(scheduler));
    }

    void transport::start_worker_thread(
        enclave_owner& owner,
        std::shared_ptr<std::vector<char>> enter_blob)
    {
        auto state = owner.state_;
        auto owner_eid = state->eid_;
        auto* startup_state_word = &state->startup_state_;
        auto* startup_error_code = &state->startup_error_code_;
        state->worker_threads_.emplace_back(
            [owner_eid, enter_blob = std::move(enter_blob), startup_state_word, startup_error_code]()
            {
                int err_code = rpc::error::OK();
                auto status = coroutine_enter_thread(owner_eid, &err_code, enter_blob->size(), enter_blob->data());

                if (status != SGX_SUCCESS)
                {
                    RPC_ERROR("coroutine_enter_thread returned sgx status {}", static_cast<int>(status));
                    fail_startup_status(startup_state_word, startup_error_code, rpc::error::TRANSPORT_ERROR());
                    return;
                }

                if (err_code != rpc::error::OK())
                {
                    RPC_ERROR("coroutine_enter_thread returned {}", err_code);
                    fail_startup_status(startup_state_word, startup_error_code, err_code);
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

        std::shared_ptr<enclave_owner> owner;
        try
        {
            owner = std::make_shared<enclave_owner>(eid);
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating SGX enclave owner");
            std::terminate();
        }
        owner->state_->host_to_enclave_queue_ = host_to_enclave_queue_;
        owner->state_->enclave_to_host_queue_ = enclave_to_host_queue_;

        // spawn the main thread for this enclave
        const auto worker_thread_count = enclave_worker_thread_count_.load(std::memory_order_acquire);
        {
            std::map<std::string, std::string> startup_options;
            {
                std::lock_guard lock(enclave_startup_options_mutex_);
                startup_options = enclave_startup_options_;
            }
            auto* host_to_enclave_queue = host_to_enclave_queue_.get();
            auto* enclave_to_host_queue = enclave_to_host_queue_.get();
            auto state = owner->state_;
            // This ECALL blob is decoded before the enclave-side service
            // exists. Keep it on the fixed SGX bootstrap ABI; the stream/RPC
            // layers use service/request encoding after the link is live.
            const auto request_encoding = sgx_bootstrap_init_request_encoding;
            // These host-provided clock values are bootstrap hints only. They
            // must not be treated as trusted time for certificate validation,
            // attestation decisions, or other enclave security policy.
            init_request request{get_zone_id(),
                adjacent_zone_id,
                input_descr.remote_object_id.is_set() ? input_descr.remote_object_id : get_zone_id().get_address(),
                worker_thread_count,
                host_pointer_address(host_to_enclave_queue),
                host_pointer_address(enclave_to_host_queue),
                &state->startup_state_,
                &state->startup_error_code_,
                host_ticks_per_millisecond(),
                host_unix_epoch_milliseconds(),
                std::move(startup_options)};
            const auto request_type_id = rpc::id<init_request>::get(rpc::get_version());
            auto request_blob
                = std::make_shared<std::vector<char>>(rpc::serialise<std::vector<char>>(request, request_encoding));
            owner->init_thread_ = std::thread(
                [state, request_blob = std::move(request_blob), request_type_id]()
                {
                    auto status = coroutine_init_enclave(
                        state->eid_,
                        request_blob->size(),
                        request_blob->data(),
                        sgx_bootstrap_init_request_encoding_value,
                        request_type_id);

                    if (status != SGX_SUCCESS)
                    {
                        RPC_ERROR("coroutine_init_enclave returned sgx status {}", static_cast<int>(status));
                        fail_startup_status(
                            &state->startup_state_, &state->startup_error_code_, rpc::error::TRANSPORT_ERROR());
                    }

                    // The owner teardown joins worker ECALL threads after the
                    // init ECALL has returned. Joining them here can create a
                    // join cycle if teardown is itself triggered from a worker
                    // or scheduler thread.
                });
        }

        auto startup_error = CO_AWAIT wait_for_startup_status(
            svc->get_scheduler(),
            &owner->state_->startup_state_,
            &owner->state_->startup_error_code_,
            startup_state::workers_requested,
            std::chrono::milliseconds{20000});
        if (startup_error != rpc::error::OK())
        {
            enclave_owner_ = std::move(owner);
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN rpc::connect_result{startup_error, {}};
        }

        for (uint32_t worker_index = 0; worker_index < worker_thread_count; ++worker_index)
        {
            auto enter_blob
                = std::make_shared<std::vector<char>>(rpc::to_yas_binary<std::vector<char>>(enter_thread_request{
                    adjacent_zone_id,
                    worker_index,
                }));
            start_worker_thread(*owner, std::move(enter_blob));
        }

        startup_error = CO_AWAIT wait_for_startup_status(
            svc->get_scheduler(),
            &owner->state_->startup_state_,
            &owner->state_->startup_error_code_,
            startup_state::runtime_ready,
            std::chrono::milliseconds{20000});
        if (startup_error != rpc::error::OK())
        {
            enclave_owner_ = std::move(owner);
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN rpc::connect_result{startup_error, {}};
        }

        enclave_owner_ = std::move(owner);
        initialise_after_construction();

        // Once the enclave runtime is live, complete the normal stream transport
        // handshake over the host/enclave SPSC queues.
        auto connect_result
            = CO_AWAIT rpc::stream_transport::transport::inner_connect(std::move(stub), std::move(input_descr));
        if (connect_result.error_code != rpc::error::OK())
        {
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN connect_result;
        }

        set_status(rpc::transport_status::CONNECTED);
        CO_RETURN connect_result;
    }

    void transport::set_status(rpc::transport_status status)
    {
        rpc::stream_transport::transport::set_status(status);

        if (status >= rpc::transport_status::DISCONNECTING)
            begin_enclave_shutdown_once();
    }

}
