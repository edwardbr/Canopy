/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <transports/sgx_coroutine/sidecar/bootstrap.h>

#  include <chrono>
#  include <cstdint>
#  include <memory>
#  include <thread>
#  include <vector>

#  include <rpc/rpc.h>
#  include <secure_coroutine_module/secure_coroutine_module.h>
#  include <sgx_urts.h>
#  include <untrusted/sgx_coroutine_transport_u.h>

#  ifndef CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG
#    define CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG SGX_DEBUG_FLAG
#  endif

extern "C" void canopy_sgx_sleep_ms(uint32_t milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
}

namespace
{
    using namespace rpc::v4::secure_coroutine_module;

    inline auto startup_load_error(const error_code* value) noexcept -> error_code
    {
        static_assert(sizeof(error_code) == sizeof(int));
        return __atomic_load_n(value, __ATOMIC_ACQUIRE);
    }

    inline auto startup_store_error(
        error_code* value,
        error_code new_value) noexcept -> void
    {
        static_assert(sizeof(error_code) == sizeof(int));
        __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
    }

    inline auto startup_load_state(const startup_state* value) noexcept -> startup_state
    {
        using state_word = std::uint32_t;
        static_assert(sizeof(startup_state) == sizeof(state_word));
        const auto loaded = __atomic_load_n(reinterpret_cast<const state_word*>(value), __ATOMIC_ACQUIRE);
        return static_cast<startup_state>(loaded);
    }

    inline auto startup_store_state(
        startup_state* value,
        startup_state state) noexcept -> void
    {
        using state_word = std::uint32_t;
        static_assert(sizeof(startup_state) == sizeof(state_word));
        const auto stored = static_cast<state_word>(state);
        __atomic_store_n(reinterpret_cast<state_word*>(value), stored, __ATOMIC_RELEASE);
    }

    void fail_startup_status(
        startup_state* startup_state_word,
        error_code* startup_error_code,
        int error_code)
    {
        if (!startup_state_word || !startup_error_code)
            return;

        startup_store_error(startup_error_code, error_code);
        startup_store_state(startup_state_word, startup_state::failed);
    }

    bool startup_state_matches(
        startup_state state,
        startup_state expected_state) noexcept
    {
        if (state == expected_state)
            return true;

        return expected_state == startup_state::workers_requested && state == startup_state::runtime_ready;
    }

    int wait_for_startup_status(
        const startup_state* startup_state_word,
        const error_code* startup_error_code,
        startup_state expected_state,
        std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto state = startup_load_state(startup_state_word);
            if (startup_state_matches(state, expected_state))
                return rpc::error::OK();
            if (state == startup_state::failed)
                return startup_load_error(startup_error_code);
            if (state == startup_state::shutting_down || state == startup_state::stopped)
                return rpc::error::RESOURCE_CLOSED();

            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return rpc::error::CALL_TIMEOUT();
    }

    uint64_t load_request_size(const uint64_t* value) noexcept
    {
        return __atomic_load_n(value, __ATOMIC_ACQUIRE);
    }

    int wait_for_init_request(
        const rpc::sgx_coroutine_transport::sidecar::shared_memory* shared,
        std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto request_size = load_request_size(&shared->request_size);
            if (request_size > 0 && request_size <= rpc::sgx_coroutine_transport::sidecar::init_request_capacity)
                return rpc::error::OK();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return rpc::error::CALL_TIMEOUT();
    }

    uint64_t read_host_tick_counter() noexcept
    {
#  if defined(__x86_64__) || defined(__i386__)
        return __builtin_ia32_rdtsc();
#  else
        return 0;
#  endif
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
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    }
}

int main(
    int argc,
    char** argv)
{
    auto bootstrap = rpc::sgx_coroutine_transport::sidecar::bootstrap::from_command_line(argc, argv);
    if (!bootstrap)
        return 1;

    auto mapping = bootstrap->create_shared_memory_file()
                       ? rpc::sgx_coroutine_transport::sidecar::create_shared_memory(bootstrap->shared_memory_file())
                       : rpc::sgx_coroutine_transport::sidecar::map_shared_memory(bootstrap->shared_memory_file());
    if (!mapping.memory)
        return 2;

    auto unmap_on_return = std::unique_ptr<
        rpc::sgx_coroutine_transport::sidecar::mapped_shared_memory,
        void (*)(rpc::sgx_coroutine_transport::sidecar::mapped_shared_memory*)>(
        &mapping,
        [](rpc::sgx_coroutine_transport::sidecar::mapped_shared_memory* mapped)
        {
            if (mapped)
                rpc::sgx_coroutine_transport::sidecar::unmap_shared_memory(*mapped);
        });

    auto* shared = mapping.memory;
    if (!rpc::sgx_coroutine_transport::sidecar::valid_shared_memory(*shared))
        return 3;

    if (wait_for_init_request(shared, std::chrono::seconds{20}) != rpc::error::OK())
        return 9;

    init_request request{};
    auto err = rpc::from_yas_binary(
        rpc::byte_span{reinterpret_cast<const uint8_t*>(shared->request.data()), shared->request_size}, request);
    if (!err.empty())
        return 4;

    startup_store_state(&shared->startup_state, startup_state::pending);
    startup_store_error(&shared->startup_error_code, 0);

    request.worker_thread_count = bootstrap->worker_thread_count();
    request.parent_to_runtime_queue_ptr = host_pointer_address(&shared->parent_to_runtime);
    request.runtime_to_parent_queue_ptr = host_pointer_address(&shared->runtime_to_parent);
    request.state = &shared->startup_state;
    request.error = &shared->startup_error_code;
    request.ticks_per_millisecond = host_ticks_per_millisecond();
    request.initial_unix_epoch_milliseconds = host_unix_epoch_milliseconds();

    sgx_enclave_id_t eid = 0;
    {
        sgx_launch_token_t token = {0};
        int updated = 0;
        auto status = sgx_create_enclave(
            bootstrap->enclave_path().c_str(), CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG, &token, &updated, &eid, nullptr);
        if (status != SGX_SUCCESS)
            return 5;
    }

    auto request_blob = std::make_shared<std::vector<char>>(rpc::to_yas_binary<std::vector<char>>(request));
    std::thread init_thread(
        [eid, request_blob, request_encoding = shared->request_encoding, request_type_id = shared->request_type_id, shared]()
        {
            auto status = sgx_coroutine_init_enclave(
                eid, request_blob->size(), request_blob->data(), request_encoding, request_type_id);

            if (status != SGX_SUCCESS)
                fail_startup_status(&shared->startup_state, &shared->startup_error_code, rpc::error::TRANSPORT_ERROR());
        });

    auto startup_error = wait_for_startup_status(
        &shared->startup_state, &shared->startup_error_code, startup_state::workers_requested, std::chrono::seconds{20});
    if (startup_error != rpc::error::OK())
    {
        fail_startup_status(&shared->startup_state, &shared->startup_error_code, startup_error);
        startup_store_state(&shared->startup_state, startup_state::shutting_down);
        if (init_thread.joinable())
            init_thread.join();
        sgx_destroy_enclave(eid);
        return 6;
    }

    std::vector<std::thread> worker_threads;
    try
    {
        worker_threads.reserve(bootstrap->worker_thread_count());
        for (uint32_t worker_index = 0; worker_index < bootstrap->worker_thread_count(); ++worker_index)
        {
            auto enter_blob = std::make_shared<std::vector<char>>(
                rpc::to_yas_binary<std::vector<char>>(enter_thread_request{request.runtime_zone_id, worker_index}));
            worker_threads.emplace_back(
                [eid, enter_blob, shared]()
                {
                    int err_code = rpc::error::OK();
                    auto status = sgx_coroutine_enter_thread(eid, &err_code, enter_blob->size(), enter_blob->data());
                    if (status != SGX_SUCCESS)
                    {
                        fail_startup_status(
                            &shared->startup_state, &shared->startup_error_code, rpc::error::TRANSPORT_ERROR());
                        return;
                    }
                    if (err_code != rpc::error::OK())
                        fail_startup_status(&shared->startup_state, &shared->startup_error_code, err_code);
                });
        }
    }
    catch (...)
    {
        fail_startup_status(&shared->startup_state, &shared->startup_error_code, rpc::error::RESOURCE_EXHAUSTED());
        startup_store_state(&shared->startup_state, startup_state::shutting_down);
        if (init_thread.joinable())
            init_thread.join();
        sgx_destroy_enclave(eid);
        return 7;
    }

    startup_error = wait_for_startup_status(
        &shared->startup_state, &shared->startup_error_code, startup_state::runtime_ready, std::chrono::seconds{20});
    if (startup_error != rpc::error::OK())
    {
        fail_startup_status(&shared->startup_state, &shared->startup_error_code, startup_error);
        startup_store_state(&shared->startup_state, startup_state::shutting_down);
    }

    if (init_thread.joinable())
        init_thread.join();
    for (auto& worker : worker_threads)
    {
        if (worker.joinable())
            worker.join();
    }

    sgx_destroy_enclave(eid);
    return startup_error == rpc::error::OK() ? 0 : 8;
}

#endif
