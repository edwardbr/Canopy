/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <args.hxx>
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <unistd.h>

#  include <transports/ipc_transport/bootstrap.h>

namespace rpc::ipc_transport
{
    namespace
    {
        constexpr auto CHILD_HOST_DLL_PATH_ARG = "dll-path";
        constexpr auto CHILD_HOST_MAPPED_FILE_ARG = "mapped-file";
        constexpr auto CHILD_HOST_DLL_SUBNET_ARG = "dll-subnet";
        constexpr auto CHILD_HOST_SCHEDULER_THREAD_COUNT_ARG = "scheduler-thread-count";
        constexpr auto CHILD_PROCESS_MAPPED_FILE_ARG = "mapped-file";
        constexpr auto CHILD_PROCESS_SUBNET_ARG = "child-subnet";
        constexpr auto CHILD_PROCESS_SCHEDULER_THREAD_COUNT_ARG = "scheduler-thread-count";
    }

    queue_pair_bootstrap::queue_pair_bootstrap(std::string mapped_file, rpc::zone child_zone, size_t scheduler_thread_count)
        : mapped_file_(std::move(mapped_file))
        , child_zone_(child_zone)
        , scheduler_thread_count_(scheduler_thread_count)
    {
    }

    const std::string& queue_pair_bootstrap::mapped_file() const
    {
        return mapped_file_;
    }

    rpc::zone queue_pair_bootstrap::child_zone() const
    {
        return child_zone_;
    }

    size_t queue_pair_bootstrap::scheduler_thread_count() const
    {
        return scheduler_thread_count_;
    }

    rpc::libcoro_spsc_dynamic_dll::queue_pair* queue_pair_bootstrap::map_queue_pair() const
    {
        int fd = ::open(mapped_file_.c_str(), O_RDWR, 0600);
        if (fd < 0)
            return nullptr;

        auto* queues = static_cast<rpc::libcoro_spsc_dynamic_dll::queue_pair*>(::mmap(
            nullptr, sizeof(rpc::libcoro_spsc_dynamic_dll::queue_pair), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        ::close(fd);

        if (queues == MAP_FAILED)
            return nullptr;

        return queues;
    }

    void queue_pair_bootstrap::unmap_queue_pair(rpc::libcoro_spsc_dynamic_dll::queue_pair* queues)
    {
        if (!queues)
            return;

        ::munmap(queues, sizeof(rpc::libcoro_spsc_dynamic_dll::queue_pair));
    }

    child_host_bootstrap::child_host_bootstrap(
        std::string dll_path, std::string mapped_file, rpc::zone dll_zone, size_t scheduler_thread_count)
        : queue_pair_bootstrap(std::move(mapped_file), dll_zone, scheduler_thread_count)
        , dll_path_(std::move(dll_path))
    {
    }

    const char* child_host_bootstrap::dll_path_arg_name()
    {
        return CHILD_HOST_DLL_PATH_ARG;
    }

    const char* child_host_bootstrap::mapped_file_arg_name()
    {
        return CHILD_HOST_MAPPED_FILE_ARG;
    }

    const char* child_host_bootstrap::dll_subnet_arg_name()
    {
        return CHILD_HOST_DLL_SUBNET_ARG;
    }

    const char* child_host_bootstrap::scheduler_thread_count_arg_name()
    {
        return CHILD_HOST_SCHEDULER_THREAD_COUNT_ARG;
    }

    std::shared_ptr<child_host_bootstrap> child_host_bootstrap::from_command_line(int argc, char** argv)
    {
        args::ArgumentParser parser("Canopy IPC child host process");
        args::HelpFlag help(parser, "help", "Display this help menu", {'h', "help"});
        args::ValueFlag<std::string> dll_path_arg(
            parser, "path", "Path to the dynamic library to load", {dll_path_arg_name()}, args::Options::Required);
        args::ValueFlag<std::string> mapped_file_arg(
            parser, "path", "Path to the mapped SPSC queue file", {mapped_file_arg_name()}, args::Options::Required);
        args::ValueFlag<uint64_t> dll_subnet_arg(
            parser, "subnet", "Subnet for the DLL zone", {dll_subnet_arg_name()}, args::Options::Required);
        args::ValueFlag<size_t> scheduler_thread_count_arg(parser,
            "thread-count",
            "Scheduler thread count for the child runtime",
            {scheduler_thread_count_arg_name()},
            args::Options::Required);

        try
        {
            parser.ParseCLI(argc, argv);
        }
        catch (const args::Help&)
        {
            return {};
        }
        catch (const args::ParseError&)
        {
            return {};
        }

        auto dll_zone = rpc::DEFAULT_PREFIX;
        if (!dll_zone.set_subnet(args::get(dll_subnet_arg)))
            return {};

        return std::make_shared<child_host_bootstrap>(
            args::get(dll_path_arg), args::get(mapped_file_arg), dll_zone, args::get(scheduler_thread_count_arg));
    }

    const std::string& child_host_bootstrap::dll_path() const
    {
        return dll_path_;
    }

    rpc::zone child_host_bootstrap::dll_zone() const
    {
        return child_zone();
    }

    std::vector<std::string> child_host_bootstrap::make_command_line() const
    {
        return {
            std::string("--") + dll_path_arg_name(),
            dll_path_,
            std::string("--") + mapped_file_arg_name(),
            mapped_file(),
            std::string("--") + dll_subnet_arg_name(),
            std::to_string(dll_zone().get_subnet()),
            std::string("--") + scheduler_thread_count_arg_name(),
            std::to_string(scheduler_thread_count()),
        };
    }

    child_process_bootstrap::child_process_bootstrap(
        std::string mapped_file, rpc::zone child_zone, size_t scheduler_thread_count)
        : queue_pair_bootstrap(std::move(mapped_file), child_zone, scheduler_thread_count)
    {
    }

    const char* child_process_bootstrap::mapped_file_arg_name()
    {
        return CHILD_PROCESS_MAPPED_FILE_ARG;
    }

    const char* child_process_bootstrap::child_subnet_arg_name()
    {
        return CHILD_PROCESS_SUBNET_ARG;
    }

    const char* child_process_bootstrap::scheduler_thread_count_arg_name()
    {
        return CHILD_PROCESS_SCHEDULER_THREAD_COUNT_ARG;
    }

    std::shared_ptr<child_process_bootstrap> child_process_bootstrap::from_command_line(int argc, char** argv)
    {
        args::ArgumentParser parser("Canopy IPC child process");
        args::HelpFlag help(parser, "help", "Display this help menu", {'h', "help"});
        args::ValueFlag<std::string> mapped_file_arg(
            parser, "path", "Path to the mapped SPSC queue file", {mapped_file_arg_name()}, args::Options::Required);
        args::ValueFlag<uint64_t> child_subnet_arg(
            parser, "subnet", "Subnet for the child zone", {child_subnet_arg_name()}, args::Options::Required);
        args::ValueFlag<size_t> scheduler_thread_count_arg(parser,
            "thread-count",
            "Scheduler thread count for the child runtime",
            {scheduler_thread_count_arg_name()},
            args::Options::Required);

        try
        {
            parser.ParseCLI(argc, argv);
        }
        catch (const args::Help&)
        {
            return nullptr;
        }
        catch (const args::ParseError&)
        {
            return nullptr;
        }

        auto child_zone = rpc::DEFAULT_PREFIX;
        if (!child_zone.set_subnet(args::get(child_subnet_arg)))
            return nullptr;

        return std::make_shared<child_process_bootstrap>(
            args::get(mapped_file_arg), child_zone, args::get(scheduler_thread_count_arg));
    }

    std::vector<std::string> child_process_bootstrap::make_command_line() const
    {
        return {
            std::string("--") + mapped_file_arg_name(),
            mapped_file(),
            std::string("--") + child_subnet_arg_name(),
            std::to_string(child_zone().get_subnet()),
            std::string("--") + scheduler_thread_count_arg_name(),
            std::to_string(scheduler_thread_count()),
        };
    }

} // namespace rpc::ipc_transport

#endif // CANOPY_BUILD_COROUTINE
