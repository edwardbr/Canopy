/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include <transports/sgx_coroutine/sidecar/bootstrap.h>

#  include <chrono>
#  include <charconv>
#  include <cstring>
#  include <filesystem>
#  include <fcntl.h>
#  include <new>
#  include <random>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <utility>
#  include <unistd.h>

namespace rpc::sgx_coroutine_transport::sidecar
{
    namespace
    {
        constexpr auto enclave_path_arg = "enclave-path";
        constexpr auto shared_memory_file_arg = "shared-memory-file";
        constexpr auto worker_thread_count_arg = "worker-thread-count";

        [[nodiscard]] std::string make_temp_file_template()
        {
            auto temp_dir = std::filesystem::temp_directory_path();
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            auto random_value = std::random_device{}();
            auto unique_prefix
                = std::to_string(::getpid()) + "_" + std::to_string(now) + "_" + std::to_string(random_value);
            auto template_path = temp_dir / ("canopy_sgx_coroutine_sidecar_" + unique_prefix + "_XXXXXX");
            return template_path.string();
        }

        [[nodiscard]] bool parse_u32(
            const std::string& value,
            uint32_t& output) noexcept
        {
            uint32_t parsed = 0;
            const auto* begin = value.data();
            const auto* end = value.data() + value.size();
            auto [ptr, ec] = std::from_chars(begin, end, parsed);
            if (ec != std::errc{} || ptr != end)
                return false;
            output = parsed;
            return true;
        }

        [[nodiscard]] const char* option_value(
            int argc,
            char** argv,
            const char* name)
        {
            const std::string expected = std::string("--") + name;
            for (int index = 1; index + 1 < argc; ++index)
            {
                if (argv[index] && expected == argv[index])
                    return argv[index + 1];
            }
            return nullptr;
        }
    } // namespace

    bootstrap::bootstrap(
        std::string enclave_path,
        std::string shared_memory_file,
        uint32_t worker_thread_count)
        : enclave_path_(std::move(enclave_path))
        , shared_memory_file_(std::move(shared_memory_file))
        , worker_thread_count_(worker_thread_count)
    {
    }

    const char* bootstrap::enclave_path_arg_name()
    {
        return enclave_path_arg;
    }

    const char* bootstrap::shared_memory_file_arg_name()
    {
        return shared_memory_file_arg;
    }

    const char* bootstrap::worker_thread_count_arg_name()
    {
        return worker_thread_count_arg;
    }

    std::shared_ptr<bootstrap> bootstrap::from_command_line(
        int argc,
        char** argv)
    {
        auto* enclave_path = option_value(argc, argv, enclave_path_arg_name());
        auto* shared_memory_file = option_value(argc, argv, shared_memory_file_arg_name());
        auto* worker_thread_count_text = option_value(argc, argv, worker_thread_count_arg_name());
        if (!enclave_path || !shared_memory_file || !worker_thread_count_text)
            return {};

        uint32_t worker_thread_count = 0;
        if (!parse_u32(worker_thread_count_text, worker_thread_count))
            return {};

        return std::make_shared<bootstrap>(enclave_path, shared_memory_file, worker_thread_count);
    }

    std::vector<std::string> bootstrap::make_command_line() const
    {
        return {
            std::string("--") + enclave_path_arg_name(),
            enclave_path_,
            std::string("--") + shared_memory_file_arg_name(),
            shared_memory_file_,
            std::string("--") + worker_thread_count_arg_name(),
            std::to_string(worker_thread_count_),
        };
    }

    mapped_shared_memory create_shared_memory()
    {
        auto file_name_template = make_temp_file_template();
        std::vector<char> file_name(file_name_template.begin(), file_name_template.end());
        file_name.push_back('\0');

        int fd = ::mkstemp(file_name.data());
        if (fd < 0)
            return {};

        mapped_shared_memory result;
        result.path = file_name.data();
        result.owner = true;

        if (::ftruncate(fd, sizeof(shared_memory)) != 0)
        {
            ::close(fd);
            ::unlink(result.path.c_str());
            return {};
        }

        auto* memory = static_cast<shared_memory*>(
            ::mmap(nullptr, sizeof(shared_memory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        ::close(fd);

        if (memory == MAP_FAILED)
        {
            ::unlink(result.path.c_str());
            return {};
        }

        new (memory) shared_memory{};
        result.memory = memory;
        return result;
    }

    mapped_shared_memory map_shared_memory(const std::string& path)
    {
        int fd = ::open(path.c_str(), O_RDWR, 0600);
        if (fd < 0)
            return {};

        auto* memory = static_cast<shared_memory*>(
            ::mmap(nullptr, sizeof(shared_memory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        ::close(fd);

        if (memory == MAP_FAILED)
            return {};

        return mapped_shared_memory{path, memory, false};
    }

    void unmap_shared_memory(mapped_shared_memory& mapping) noexcept
    {
        if (mapping.memory)
        {
            ::munmap(mapping.memory, sizeof(shared_memory));
            mapping.memory = nullptr;
        }

        if (mapping.owner && !mapping.path.empty())
        {
            ::unlink(mapping.path.c_str());
            mapping.path.clear();
        }
    }

    bool valid_shared_memory(const shared_memory& memory) noexcept
    {
        return memory.magic == shared_memory_magic && memory.version == shared_memory_version
               && memory.request_size <= init_request_capacity;
    }
}

#endif
