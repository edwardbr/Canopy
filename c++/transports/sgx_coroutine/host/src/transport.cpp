/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/host/transport.h>
#include <untrusted/sgx_coroutine_transport_u.h>
#include <transports/streaming/transport.h>
#include <transports/sgx_coroutine/sidecar/bootstrap.h>
#include <streaming/stream_transport.h>
#include <streaming/spsc_queue/stream.h>
#include <sgx_urts.h>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <rpc/rpc.h>
#include <chrono>
#include <filesystem>
#include <new>
#include <map>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <secure_coroutine_module/secure_coroutine_module.h>
#include <transports/secure_coroutine_module/startup_options.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#  include <sys/prctl.h>
#endif

#ifndef CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG
#  define CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG SGX_DEBUG_FLAG
#endif

extern char** environ;

extern "C" void canopy_sgx_sleep_ms(uint32_t milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
}

namespace rpc::sgx_coroutine_transport::host
{
    namespace
    {
        using namespace rpc::v4::secure_coroutine_module;

        constexpr rpc::encoding sgx_bootstrap_init_request_encoding = rpc::encoding::yas_binary;
        constexpr uint64_t sgx_bootstrap_init_request_encoding_value
            = static_cast<uint64_t>(sgx_bootstrap_init_request_encoding);

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

        inline auto startup_store_request_size(
            uint64_t* value,
            uint64_t new_value) noexcept -> void
        {
            __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
        }

        inline auto initialise_startup_status(
            startup_state& state,
            error_code& error) noexcept -> void
        {
            startup_store_state(&state, startup_state::pending);
            startup_store_error(&error, 0);
        }

        bool signal_peer_closed(::streaming::spsc_queue::queue_type* send_queue)
        {
            if (!send_queue)
                return false;

            ::streaming::spsc_queue::blob close_blob{};
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
                auto state = startup_load_state(startup_state_word);
                if (startup_state_matches(state, expected_state))
                    CO_RETURN rpc::error::OK();

                if (state == startup_state::failed)
                    CO_RETURN startup_load_error(startup_error_code);

                // This is host-side enclave bootstrap, before the stream pumps
                // are established. It must be driven by the service scheduler;
                // tests with manual schedulers must not wrap this connect path
                // in sync_wait() without also pumping scheduler events.
                CO_AWAIT scheduler->schedule_after(std::chrono::milliseconds{1});
            }

            RPC_ERROR("timed out waiting for startup readiness");
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

            startup_store_error(startup_error_code, error_code);
            startup_store_state(startup_state_word, startup_state::failed);
        }

        [[nodiscard]] bool process_is_alive(pid_t pid)
        {
            if (pid <= 0)
                return false;

            return ::kill(pid, 0) == 0 || errno == EPERM;
        }

        pid_t waitpid_nointr(
            pid_t pid,
            int* status,
            int options) noexcept
        {
            pid_t wait_result = -1;
            do
            {
                wait_result = ::waitpid(pid, status, options);
            } while (wait_result < 0 && errno == EINTR);
            return wait_result;
        }

        bool read_exact_nointr(
            int fd,
            void* data,
            size_t size) noexcept
        {
            auto* bytes = static_cast<uint8_t*>(data);
            size_t bytes_read_total = 0;
            while (bytes_read_total < size)
            {
                auto bytes_read = ::read(fd, bytes + bytes_read_total, size - bytes_read_total);
                if (bytes_read < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return false;
                }
                if (bytes_read == 0)
                    return false;
                bytes_read_total += static_cast<size_t>(bytes_read);
            }
            return true;
        }

        [[nodiscard]] std::string path_basename(std::string_view path)
        {
            auto pos = path.find_last_of('/');
            if (pos == std::string_view::npos)
                return std::string(path);
            return std::string(path.substr(pos + 1));
        }

        [[nodiscard]] std::string default_sidecar_executable_path()
        {
#ifdef CANOPY_SGX_COROUTINE_SIDECAR_EXECUTABLE
            return CANOPY_SGX_COROUTINE_SIDECAR_EXECUTABLE;
#else
            std::array<char, 4096> executable_path{};
            auto length = ::readlink("/proc/self/exe", executable_path.data(), executable_path.size() - 1);
            if (length > 0)
            {
                executable_path[static_cast<std::size_t>(length)] = '\0';
                return (std::filesystem::path(executable_path.data()).parent_path() / "canopy_sgx_coroutine_sidecar").string();
            }
            return "canopy_sgx_coroutine_sidecar";
#endif
        }

    }

    class transport::deferred_stream : public ::streaming::stream
    {
    public:
        void bind(std::shared_ptr<::streaming::stream> stream) { stream_ = std::move(stream); }

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

        [[nodiscard]] auto get_peer_info() const -> ::streaming::peer_info override
        {
            return stream_ ? stream_->get_peer_info() : ::streaming::peer_info{};
        }

    private:
        std::shared_ptr<::streaming::stream> stream_;
    };

    class transport::sidecar_owner
    {
    public:
        static std::shared_ptr<sidecar_owner> create(std::string executable_path)
        {
            auto mapping = sidecar::create_shared_memory();
            if (!mapping.memory)
                return {};

            return std::make_shared<sidecar_owner>(std::move(executable_path), std::move(mapping));
        }

        static std::shared_ptr<sidecar_owner> open_peer_to_peer(std::string shared_memory_file)
        {
            auto mapping = sidecar::map_shared_memory(shared_memory_file);
            if (!mapping.memory)
                return {};

            return std::make_shared<sidecar_owner>(std::string{}, std::move(mapping));
        }

        sidecar_owner(
            std::string executable_path,
            sidecar::mapped_shared_memory mapping)
            : executable_path_(std::move(executable_path))
            , mapping_(std::move(mapping))
        {
        }

        ~sidecar_owner()
        {
            request_shutdown();
            stop_child();
            join_or_detach_if_current(watcher_thread_);
            sidecar::unmap_shared_memory(mapping_);
        }

        sidecar::shared_memory* memory() const noexcept { return mapping_.memory; }

        int child_pid_for_test() const noexcept { return child_pid_.load(std::memory_order_acquire); }

        int write_init_request(
            const init_request& request,
            uint64_t request_encoding,
            uint64_t request_type_id)
        {
            auto* shared = memory();
            if (!shared)
                return rpc::error::INVALID_DATA();

            initialise_startup_status(shared->startup_state, shared->startup_error_code);
            auto request_blob = rpc::to_yas_binary<std::vector<char>>(request);
            if (request_blob.size() > sidecar::init_request_capacity)
                return rpc::error::RESOURCE_EXHAUSTED();

            startup_store_request_size(&shared->request_size, 0);
            shared->request_encoding = request_encoding;
            shared->request_type_id = request_type_id;
            std::memcpy(shared->request.data(), request_blob.data(), request_blob.size());
            startup_store_request_size(&shared->request_size, request_blob.size());
            return rpc::error::OK();
        }

        int spawn(
            const std::string& enclave_path,
            uint32_t worker_thread_count,
            std::weak_ptr<transport> transport)
        {
            // These three values are the minimum state required by the sidecar
            // executable. executable_path_ is the program to exec, mapping_.path
            // is the mmap backing file name passed on the command line, and
            // mapping_.memory is the parent-side view used to write the enclave
            // bootstrap request before the sidecar starts.
            if (executable_path_.empty() || mapping_.path.empty() || !mapping_.memory)
                return rpc::error::INVALID_DATA();

            // The sidecar is launched with the usual Unix fork()+execve()
            // pattern. fork() briefly creates a child that is a copy-on-write
            // view of this process, but the child immediately replaces itself
            // with canopy_sgx_coroutine_sidecar via execve() below.
            //
            // The pipe is a one-byte readiness handshake. It lets the parent
            // wait until the child has armed PR_SET_PDEATHSIG before the parent
            // records the sidecar as live. Without that handshake there is a
            // small race where the parent could die before the child has asked
            // the kernel to kill it when its parent exits.
            int ready_pipe[2] = {-1, -1};
            if (::pipe(ready_pipe) != 0)
                return rpc::error::TRANSPORT_ERROR();

            pid_t child_pid = ::fork();
            if (child_pid < 0)
            {
                ::close(ready_pipe[0]);
                ::close(ready_pipe[1]);
                return rpc::error::TRANSPORT_ERROR();
            }

            if (child_pid == 0)
            {
                ::close(ready_pipe[0]);

#if defined(__linux__)
                // Keep the sidecar lifetime tied to the launcher. This survives
                // execve(), so the actual sidecar executable also receives
                // SIGKILL if the host process dies unexpectedly.
                if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0)
                    _exit(126);
                // If the parent died between fork() and prctl(), init/systemd
                // will have adopted us. Exit rather than running an orphaned
                // sidecar with access to the shared-memory queues.
                if (::getppid() == 1)
                    _exit(127);
#endif

                // Tell the parent that the parent-death contract is armed. Keep
                // the child-side work before execve() intentionally tiny:
                // async-signal-safe syscalls and simple argument construction.
                uint8_t ready = 1;
                if (::write(ready_pipe[1], &ready, sizeof(ready)) != sizeof(ready))
                    _exit(125);
                ::close(ready_pipe[1]);

                auto argv0 = path_basename(executable_path_);
                auto bootstrap = sidecar::bootstrap(enclave_path, mapping_.path, worker_thread_count);
                auto arguments = bootstrap.make_command_line();
                std::vector<char*> argv;
                argv.reserve(arguments.size() + 2);
                argv.push_back(argv0.data());
                for (auto& argument : arguments)
                    argv.push_back(argument.data());
                argv.push_back(nullptr);

                ::execve(executable_path_.c_str(), argv.data(), environ);
                // Only reached if execve() failed. Use _exit() so the forked
                // child does not run host-process destructors or atexit hooks.
                _exit(127);
            }

            // Parent side: wait for the child to confirm PR_SET_PDEATHSIG is
            // active before exposing child_pid_ to the rest of the transport.
            ::close(ready_pipe[1]);
            uint8_t ready = 0;
            auto ready_received = read_exact_nointr(ready_pipe[0], &ready, sizeof(ready));
            ::close(ready_pipe[0]);
            if (!ready_received || ready != 1)
            {
                int status = 0;
                waitpid_nointr(child_pid, &status, 0);
                return rpc::error::TRANSPORT_ERROR();
            }

            child_pid_.store(child_pid, std::memory_order_release);
            try
            {
                watcher_thread_ = std::thread(&sidecar_owner::watch_child, this, std::move(transport));
            }
            catch (const std::system_error&)
            {
                shutdown_requested_.store(true, std::memory_order_release);
                ::kill(child_pid, SIGKILL);
                int status = 0;
                waitpid_nointr(child_pid, &status, 0);
                child_reaped_.store(true, std::memory_order_release);
                child_pid_.store(-1, std::memory_order_release);
                return rpc::error::RESOURCE_EXHAUSTED();
            }

            return rpc::error::OK();
        }

        void request_shutdown() noexcept
        {
            shutdown_requested_.store(true, std::memory_order_release);
            auto* shared = memory();
            if (!shared)
                return;

            auto current_state = startup_load_state(&shared->startup_state);
            if (current_state != startup_state::failed && current_state != startup_state::stopped)
                startup_store_state(&shared->startup_state, startup_state::shutting_down);

            signal_peer_closed(&shared->parent_to_runtime);
        }

    private:
        std::string executable_path_;
        sidecar::mapped_shared_memory mapping_;
        std::thread watcher_thread_;
        std::atomic<int> child_pid_{-1};
        std::atomic<bool> child_reaped_{false};
        std::atomic<bool> shutdown_requested_{false};

        static bool is_current_thread(const std::thread& thread) noexcept
        {
            return thread.joinable() && thread.get_id() == std::this_thread::get_id();
        }

        static void join_or_detach_if_current(std::thread& thread)
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

        bool wait_until_child_reaped(std::chrono::milliseconds timeout) const
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!child_reaped_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds{10});

            return child_reaped_.load(std::memory_order_acquire);
        }

        void stop_child() noexcept
        {
            const auto child_pid = static_cast<pid_t>(child_pid_.load(std::memory_order_acquire));
            if (child_pid <= 0 || child_reaped_.load(std::memory_order_acquire))
                return;

            if (wait_until_child_reaped(std::chrono::seconds{5}))
                return;

            if (process_is_alive(child_pid))
                ::kill(child_pid, SIGTERM);
            if (wait_until_child_reaped(std::chrono::seconds{1}))
                return;

            if (process_is_alive(child_pid))
                ::kill(child_pid, SIGKILL);
            wait_until_child_reaped(std::chrono::seconds{1});
        }

        void watch_child(std::weak_ptr<transport> transport)
        {
            const auto child_pid = static_cast<pid_t>(child_pid_.load(std::memory_order_acquire));
            if (child_pid <= 0)
                return;

            int status = 0;
            waitpid_nointr(child_pid, &status, 0);

            child_reaped_.store(true, std::memory_order_release);
            child_pid_.store(-1, std::memory_order_release);

            if (shutdown_requested_.load(std::memory_order_acquire))
                return;

            auto* shared = memory();
            if (shared)
            {
                fail_startup_status(&shared->startup_state, &shared->startup_error_code, rpc::error::TRANSPORT_ERROR());
                signal_peer_closed(&shared->runtime_to_parent);
            }

            if (auto locked_transport = transport.lock())
            {
                if (locked_transport->get_status() < rpc::transport_status::DISCONNECTING)
                    locked_transport->set_status(rpc::transport_status::DISCONNECTING);
            }
        }
    };

    transport::enclave_owner::thread_state::thread_state(uint64_t eid)
        : eid_(eid)
    {
        initialise_startup_status(startup_state_, startup_error_code_);
    }

    void transport::enclave_owner::request_shutdown() const
    {
        auto state = state_;
        if (state)
        {
            auto current_state = startup_load_state(&state->startup_state_);
            if (current_state != startup_state::failed && current_state != startup_state::stopped)
                startup_store_state(&state->startup_state_, startup_state::shutting_down);
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
        if (get_status() < rpc::transport_status::DISCONNECTING)
            begin_clean_disconnect();
    }

    void transport::set_enclave_worker_thread_count(uint32_t worker_thread_count)
    {
        enclave_worker_thread_count_ = worker_thread_count;
    }

    void transport::set_use_sidecar(bool use_sidecar)
    {
        use_sidecar_ = use_sidecar;
    }

    void transport::set_sidecar_executable_path(std::string sidecar_executable_path)
    {
        sidecar_executable_path_ = std::move(sidecar_executable_path);
    }

    void transport::set_peer_to_peer_shared_memory_file(std::string shared_memory_file)
    {
        peer_to_peer_shared_memory_file_ = std::move(shared_memory_file);
    }

#ifdef CANOPY_BUILD_TEST
    int transport::sidecar_pid_for_test() const
    {
        if (!sidecar_owner_)
            return -1;
        return sidecar_owner_->child_pid_for_test();
    }
#endif

    int transport::set_enclave_startup_applications(rpc::v4::secure_coroutine_module::startup_applications applications)
    {
        auto validation_error
            = rpc::v4::secure_coroutine_module::validate_startup_applications_resource_budget(applications);
        if (validation_error != rpc::error::OK())
            return validation_error;

        enclave_startup_applications_ = std::move(applications);
        return rpc::error::OK();
    }

    int transport::set_enclave_startup_options(json::v1::object options)
    {
        auto materialised = rpc::v4::secure_coroutine_module::materialise_startup_applications(options);
        if (materialised.error_code != rpc::error::OK())
            return materialised.error_code;

        return set_enclave_startup_applications(std::move(materialised.applications));
    }

    int transport::set_enclave_runtime_settings(json::v1::object settings)
    {
        auto validation_error
            = rpc::v4::secure_coroutine_module::validate_startup_runtime_settings_resource_budget(settings);
        if (validation_error != rpc::error::OK())
            return validation_error;

        enclave_runtime_settings_ = std::make_shared<const json::v1::object>(std::move(settings));
        return rpc::error::OK();
    }

    void transport::set_enclave_io_uring_options(rpc::io_uring::host_controller::options options)
    {
        enclave_io_uring_options_ = std::move(options);
    }

    void transport::set_host_stream_layer_applier(stream_layer_applier applier)
    {
        host_stream_layer_applier_ = std::move(applier);
    }

    int transport::set_enclave_stream_layers(std::vector<rpc::stream_layers::stream_layer_settings> layers)
    {
        auto validation_error = rpc::v4::secure_coroutine_module::validate_startup_stream_layers_resource_budget(layers);
        if (validation_error != rpc::error::OK())
            return validation_error;

        enclave_stream_layers_ = std::move(layers);
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
        auto sidecar_to_stop = sidecar_owner_;
        auto queue_stream = queue_stream_;
        auto host_to_enclave_queue = host_to_enclave_queue_;
        rpc::coro::scheduler_ptr scheduler;
        if (auto service = get_service())
            scheduler = service->get_scheduler();

        if (owner_to_stop)
            owner_to_stop->request_shutdown();
        if (sidecar_to_stop)
            sidecar_to_stop->request_shutdown();

        if (queue_stream)
        {
            bool peer_closed = false;
            if (host_to_enclave_queue)
                peer_closed = signal_peer_closed(host_to_enclave_queue.get());
            else if (sidecar_to_stop && sidecar_to_stop->memory())
                peer_closed = signal_peer_closed(&sidecar_to_stop->memory()->parent_to_runtime);

            if (!peer_closed)
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
                auto status = sgx_coroutine_enter_thread(owner_eid, &err_code, enter_blob->size(), enter_blob->data());

                if (status != SGX_SUCCESS)
                {
                    RPC_ERROR("enclave worker entry returned SGX status {}", static_cast<int>(status));
                    fail_startup_status(startup_state_word, startup_error_code, rpc::error::TRANSPORT_ERROR());
                    return;
                }

                if (err_code != rpc::error::OK())
                {
                    RPC_ERROR("enclave worker entry returned error {}", err_code);
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

        host_to_enclave_queue_ = std::make_shared<::streaming::spsc_queue::queue_type>();
        enclave_to_host_queue_ = std::make_shared<::streaming::spsc_queue::queue_type>();

        std::shared_ptr<sidecar_owner> pending_sidecar_owner;
        if (use_sidecar_)
        {
            if (!peer_to_peer_shared_memory_file_.empty())
            {
                pending_sidecar_owner = sidecar_owner::open_peer_to_peer(peer_to_peer_shared_memory_file_);
            }
            else
            {
                auto executable_path
                    = sidecar_executable_path_.empty() ? default_sidecar_executable_path() : sidecar_executable_path_;
                pending_sidecar_owner = sidecar_owner::create(std::move(executable_path));
            }
            if (!pending_sidecar_owner || !pending_sidecar_owner->memory())
                CO_RETURN rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};

            host_to_enclave_queue_.reset();
            enclave_to_host_queue_.reset();
            auto* shared = pending_sidecar_owner->memory();
            auto send_queue_owner
                = std::shared_ptr<::streaming::spsc_queue::queue_type>(pending_sidecar_owner, &shared->parent_to_runtime);
            auto receive_queue_owner
                = std::shared_ptr<::streaming::spsc_queue::queue_type>(pending_sidecar_owner, &shared->runtime_to_parent);
            queue_stream_ = std::make_shared<::streaming::spsc_queue::stream>(
                std::move(send_queue_owner), std::move(receive_queue_owner), svc->get_scheduler());
        }
        else
        {
            queue_stream_ = std::make_shared<::streaming::spsc_queue::stream>(
                host_to_enclave_queue_, enclave_to_host_queue_, svc->get_scheduler());
        }

        stream_layer_applier host_stream_layer_applier;
        std::vector<rpc::stream_layers::stream_layer_settings> enclave_stream_layers;
        host_stream_layer_applier = host_stream_layer_applier_;
        enclave_stream_layers = enclave_stream_layers_;

        if (!enclave_stream_layers.empty())
        {
            if (!host_stream_layer_applier)
                CO_RETURN rpc::connect_result{rpc::error::INVALID_DATA(), {}};

            auto wrapped_stream = CO_AWAIT host_stream_layer_applier(queue_stream_);
            if (wrapped_stream.error_code != rpc::error::OK())
                CO_RETURN rpc::connect_result{wrapped_stream.error_code, {}};
            if (!wrapped_stream.stream)
                CO_RETURN rpc::connect_result{rpc::error::INVALID_DATA(), {}};

            deferred_stream_->bind(std::move(wrapped_stream.stream));
        }
        else
        {
            deferred_stream_->bind(queue_stream_);
        }

        // spawn the main thread for this enclave
        const auto worker_thread_count = enclave_worker_thread_count_;
        std::map<std::string, json::v1::object> startup_applications;
        std::shared_ptr<const json::v1::object> runtime_settings;
        startup_applications = enclave_startup_applications_;
        runtime_settings = enclave_runtime_settings_;

        if (pending_sidecar_owner)
        {
            auto* shared = pending_sidecar_owner->memory();
            init_request request{};
            request.parent_zone_id = get_zone_id();
            request.runtime_zone_id = adjacent_zone_id;
            request.root_remote_object
                = input_descr.remote_object_id.is_set() ? input_descr.remote_object_id : get_zone_id().get_address();
            request.worker_thread_count = worker_thread_count;
            request.parent_to_runtime_queue_ptr = host_pointer_address(&shared->parent_to_runtime);
            request.runtime_to_parent_queue_ptr = host_pointer_address(&shared->runtime_to_parent);
            request.state = &shared->startup_state;
            request.error = &shared->startup_error_code;
            request.ticks_per_millisecond = host_ticks_per_millisecond();
            request.initial_unix_epoch_milliseconds = host_unix_epoch_milliseconds();
            request.applications = std::move(startup_applications);
            request.runtime_settings = runtime_settings ? *runtime_settings : get_enclave_runtime_settings();
            request.stream_layers = std::move(enclave_stream_layers);

            const auto request_type_id = rpc::id<init_request>::get(rpc::get_version());
            auto startup_error = pending_sidecar_owner->write_init_request(
                request, sgx_bootstrap_init_request_encoding_value, request_type_id);
            if (startup_error != rpc::error::OK())
            {
                sidecar_owner_ = std::move(pending_sidecar_owner);
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN rpc::connect_result{startup_error, {}};
            }

            if (peer_to_peer_shared_memory_file_.empty())
            {
                auto self = std::static_pointer_cast<transport>(shared_from_this());
                startup_error = pending_sidecar_owner->spawn(enclave_path_, worker_thread_count, self);
            }
            sidecar_owner_ = std::move(pending_sidecar_owner);
            if (startup_error != rpc::error::OK())
            {
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN rpc::connect_result{startup_error, {}};
            }

            startup_error = CO_AWAIT wait_for_startup_status(
                svc->get_scheduler(),
                &shared->startup_state,
                &shared->startup_error_code,
                startup_state::runtime_ready,
                std::chrono::milliseconds{20000});
            if (startup_error != rpc::error::OK())
            {
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN rpc::connect_result{startup_error, {}};
            }

            initialise_after_construction();

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

        sgx_enclave_id_t eid = 0;
        // fire up the enclave
        {
            sgx_launch_token_t token = {0};
            int updated = 0;
            auto status = sgx_create_enclave(
                enclave_path_.c_str(), CANOPY_SGX_CREATE_ENCLAVE_DEBUG_FLAG, &token, &updated, &eid, nullptr);
            if (status != SGX_SUCCESS)
            {
                RPC_ERROR("enclave creation returned SGX status {}", static_cast<int>(status));
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

        {
            auto* host_to_enclave_queue = host_to_enclave_queue_.get();
            auto* enclave_to_host_queue = enclave_to_host_queue_.get();
            auto state = owner->state_;
            // This ECALL blob is decoded before the enclave-side service
            // exists. Keep it on the fixed SGX bootstrap ABI; the stream/RPC
            // layers use service/request encoding after the link is live.
            // These host-provided clock values are bootstrap hints only. They
            // must not be treated as trusted time for certificate validation,
            // attestation decisions, or other enclave security policy.
            init_request request{};
            request.parent_zone_id = get_zone_id();
            request.runtime_zone_id = adjacent_zone_id;
            request.root_remote_object
                = input_descr.remote_object_id.is_set() ? input_descr.remote_object_id : get_zone_id().get_address();
            request.worker_thread_count = worker_thread_count;
            request.parent_to_runtime_queue_ptr = host_pointer_address(host_to_enclave_queue);
            request.runtime_to_parent_queue_ptr = host_pointer_address(enclave_to_host_queue);
            request.state = &state->startup_state_;
            request.error = &state->startup_error_code_;
            request.ticks_per_millisecond = host_ticks_per_millisecond();
            request.initial_unix_epoch_milliseconds = host_unix_epoch_milliseconds();
            request.applications = std::move(startup_applications);
            request.runtime_settings = runtime_settings ? *runtime_settings : get_enclave_runtime_settings();
            request.stream_layers = std::move(enclave_stream_layers);
            const auto request_type_id = rpc::id<init_request>::get(rpc::get_version());
            auto request_blob = std::make_shared<std::vector<char>>(rpc::to_yas_binary<std::vector<char>>(request));
            owner->init_thread_ = std::thread(
                [state, request_blob = std::move(request_blob), request_type_id]()
                {
                    auto status = sgx_coroutine_init_enclave(
                        state->eid_,
                        request_blob->size(),
                        request_blob->data(),
                        sgx_bootstrap_init_request_encoding_value,
                        request_type_id);

                    if (status != SGX_SUCCESS)
                    {
                        RPC_ERROR("enclave initialisation returned SGX status {}", static_cast<int>(status));
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
        std::lock_guard lock(status_transition_mutex_);
        if (status == rpc::transport_status::DISCONNECTING && get_status() >= rpc::transport_status::DISCONNECTING)
        {
            begin_enclave_shutdown_once();
            return;
        }

        rpc::stream_transport::transport::set_status(status);

        if (status >= rpc::transport_status::DISCONNECTING)
            begin_enclave_shutdown_once();
    }

}
