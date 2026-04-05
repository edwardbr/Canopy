/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <atomic>
#  include <cerrno>
#  include <csignal>
#  include <cstdio>
#  include <cstdlib>
#  include <cstring>
#  include <filesystem>
#  include <fcntl.h>
#  include <chrono>
#  include <memory>
#  include <random>
#  include <string_view>
#  include <vector>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>

#  if defined(__linux__)
#    include <sys/prctl.h>
#  endif

#  include <streaming/spsc_queue/stream.h>
#  include <transports/ipc_transport/bootstrap.h>
#  include <transports/ipc_transport/transport.h>

extern char** environ;

namespace rpc::ipc_transport
{
    struct transport::state
    {
        std::string mapped_file;
        rpc::libcoro_spsc_dynamic_dll::queue_pair* queues = nullptr;
        pid_t child_pid = -1;
        std::atomic<bool> child_reaped = false;
    };

    namespace
    {
        [[nodiscard]] std::string make_temp_file_template()
        {
            auto temp_dir = std::filesystem::temp_directory_path();
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            auto random_value = std::random_device{}();
            auto unique_prefix
                = std::to_string(::getpid()) + "_" + std::to_string(now) + "_" + std::to_string(random_value);
            auto template_path = temp_dir / ("canopy_ipc_transport_" + unique_prefix + "_XXXXXX");
            return template_path.string();
        }

        [[nodiscard]] bool process_is_alive(pid_t pid)
        {
            if (pid <= 0)
                return false;

            return ::kill(pid, 0) == 0 || errno == EPERM;
        }

        [[nodiscard]] bool wait_for_child_exit(
            pid_t child_pid,
            int& status,
            std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            for (;;)
            {
                auto wait_result = ::waitpid(child_pid, &status, WNOHANG);
                if (wait_result == child_pid)
                    return true;
                if (wait_result < 0)
                    return false;
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        [[nodiscard]] std::string path_basename(std::string_view path)
        {
            auto pos = path.find_last_of('/');
            if (pos == std::string_view::npos)
                return std::string(path);
            return std::string(path.substr(pos + 1));
        }

    }

    transport::construction_bundle transport::create_bundle(
        const std::shared_ptr<rpc::service>& service,
        const options&)
    {
        RPC_ASSERT(service);

        auto result_state = std::make_shared<transport::state>();

        auto file_name_template = make_temp_file_template();
        std::vector<char> file_name(file_name_template.begin(), file_name_template.end());
        file_name.push_back('\0');

        int fd = ::mkstemp(file_name.data());
        RPC_ASSERT(fd >= 0);

        result_state->mapped_file = file_name.data();
        RPC_ASSERT(::ftruncate(fd, sizeof(rpc::libcoro_spsc_dynamic_dll::queue_pair)) == 0);

        auto* queues = static_cast<rpc::libcoro_spsc_dynamic_dll::queue_pair*>(::mmap(
            nullptr, sizeof(rpc::libcoro_spsc_dynamic_dll::queue_pair), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        ::close(fd);

        RPC_ASSERT(queues != MAP_FAILED);
        new (queues) rpc::libcoro_spsc_dynamic_dll::queue_pair{};
        result_state->queues = queues;

        auto stream = std::make_shared<streaming::spsc_queue::stream>(
            &queues->host_to_dll, &queues->dll_to_host, service->get_scheduler());

        return construction_bundle{.state = std::move(result_state), .stream = std::move(stream)};
    }

    void transport::spawn_child(
        const std::shared_ptr<state>& state,
        const options& options)
    {
        RPC_ASSERT(state);
        RPC_ASSERT(state->queues);
        RPC_ASSERT(!options.process_executable.empty());
        RPC_ASSERT(options.process_kind != child_process_kind::host_dll || !options.dll_path.empty());

        // This pipe is a one-byte readiness handshake between parent and child after fork().
        // Child writes once PR_SET_PDEATHSIG is armed; parent blocks until then so callers only
        // observe a transport whose "kill child when parent dies" contract is already active.
        int ready_pipe[2] = {-1, -1};
        if (options.kill_child_on_parent_death)
            RPC_ASSERT(::pipe(ready_pipe) == 0);

        pid_t child_pid = ::fork();
        RPC_ASSERT(child_pid >= 0);

        if (child_pid == 0)
        {
            // Child closes the read end and only keeps the write end long enough to signal readiness.
            if (ready_pipe[0] != -1)
                ::close(ready_pipe[0]);

#  if defined(__linux__)
            if (options.kill_child_on_parent_death)
            {
                if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0)
                    _exit(126);
                if (::getppid() == 1)
                    _exit(127);
                // Send a single byte to tell the parent the death-signal protection is now armed.
                uint8_t ready = 1;
                if (::write(ready_pipe[1], &ready, sizeof(ready)) != sizeof(ready))
                    _exit(125);
            }
#  endif

            // Close the pipe before exec so the launched process does not inherit this handshake fd.
            if (ready_pipe[1] != -1)
                ::close(ready_pipe[1]);

            auto argv0 = path_basename(options.process_executable);
            std::vector<std::string> arguments;
            if (options.process_kind == child_process_kind::host_dll)
                arguments = child_host_bootstrap(
                    options.dll_path, state->mapped_file, options.dll_zone, options.child_scheduler_thread_count)
                                .make_command_line();
            else
                arguments
                    = child_process_bootstrap(state->mapped_file, options.dll_zone, options.child_scheduler_thread_count)
                          .make_command_line();
            std::vector<char*> argv;
            argv.reserve(arguments.size() + 2);
            argv.push_back(argv0.data());
            for (auto& argument : arguments)
                argv.push_back(argument.data());
            argv.push_back(nullptr);

            ::execve(options.process_executable.c_str(), argv.data(), environ);
            _exit(127);
        }

        // Parent closes its write end immediately, then waits for the child's single-byte ready signal.
        if (ready_pipe[1] != -1)
            ::close(ready_pipe[1]);
        if (options.kill_child_on_parent_death)
        {
            uint8_t ready = 0;
            auto bytes_read = ::read(ready_pipe[0], &ready, sizeof(ready));
            ::close(ready_pipe[0]);
            RPC_ASSERT(bytes_read == sizeof(ready));
            RPC_ASSERT(ready == 1);
        }

        state->child_pid = child_pid;
    }

    void transport::reap_child(const std::shared_ptr<state>& state)
    {
        if (!state || state->child_reaped.exchange(true))
            return;

        if (state->child_pid > 0)
        {
            int status = 0;
            bool child_exited = false;
            auto wait_result = ::waitpid(state->child_pid, &status, WNOHANG);
            if (wait_result == state->child_pid)
                child_exited = true;
            if (wait_result == 0 && process_is_alive(state->child_pid))
            {
                if (!wait_for_child_exit(state->child_pid, status, std::chrono::seconds(5)))
                {
                    RPC_WARNING("ipc_transport: child pid={} did not exit cleanly, sending SIGTERM", state->child_pid);
                    ::kill(state->child_pid, SIGTERM);
                    if (!wait_for_child_exit(state->child_pid, status, std::chrono::seconds(1)))
                    {
                        RPC_WARNING(
                            "ipc_transport: child pid={} still alive after SIGTERM, sending SIGKILL", state->child_pid);
                        ::kill(state->child_pid, SIGKILL);
                        child_exited = wait_for_child_exit(state->child_pid, status, std::chrono::seconds(1));
                    }
                    else
                        child_exited = true;
                }
                else
                    child_exited = true;
            }

            if (child_exited)
                RPC_INFO("ipc_transport: child pid={} exited with status={}", state->child_pid, status);
            state->child_pid = -1;
        }

        if (state->queues)
        {
            ::munmap(state->queues, sizeof(rpc::libcoro_spsc_dynamic_dll::queue_pair));
            state->queues = nullptr;
        }

        if (!state->mapped_file.empty())
        {
            ::unlink(state->mapped_file.c_str());
            state->mapped_file.clear();
        }
    }

    transport::transport(
        std::string name,
        const std::shared_ptr<rpc::service>& service,
        options options)
        : transport(
              std::move(name),
              service,
              std::move(options),
              create_bundle(
                  service,
                  options))
    {
    }

    transport::transport(
        std::string name,
        const std::shared_ptr<rpc::service>& service,
        options options,
        construction_bundle bundle)
        : rpc::stream_transport::transport(
              std::move(name),
              service,
              std::move(bundle.stream),
              nullptr)
        , state_(std::move(bundle.state))
    {
        spawn_child(state_, options);
        child_started_ = true;
    }

    transport::~transport()
    {
        if (!state_)
            return;

        if (!state_->child_reaped.load() && state_->child_pid > 0 && process_is_alive(state_->child_pid))
        {
            RPC_INFO("ipc_transport: waiting for child pid={} during transport destruction", state_->child_pid);
        }

        reap_child(state_);
    }

    void transport::initialise()
    {
        initialise_after_construction();
    }

#  ifdef CANOPY_BUILD_TEST
    int transport::child_pid_for_test() const
    {
        if (!state_)
            return -1;
        return state_->child_pid;
    }
#  endif

    void transport::set_status(rpc::transport_status new_status)
    {
        auto old_status = get_status();
        rpc::stream_transport::transport::set_status(new_status);

        if (old_status < rpc::transport_status::DISCONNECTED && new_status == rpc::transport_status::DISCONNECTED && state_)
            reap_child(state_);
    }

    std::shared_ptr<transport> make_client(
        std::string name,
        const std::shared_ptr<rpc::service>& service,
        options options)
    {
        auto result = std::shared_ptr<transport>(new transport(std::move(name), service, std::move(options)));
        result->initialise();
        return result;
    }

} // namespace rpc::ipc_transport

#endif // CANOPY_BUILD_COROUTINE
