/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <array>
#  include <cstddef>
#  include <cstdint>
#  include <memory>
#  include <string>
#  include <vector>

#  include <secure_coroutine_module/secure_coroutine_module.h>
#  include <streaming/spsc_queue/stream.h>

namespace rpc::sgx_coroutine_transport::sidecar
{
    inline constexpr uint64_t shared_memory_magic = 0x5347585349444543ULL; // "SGXSIDEC"
    inline constexpr uint32_t shared_memory_version = 1;
    inline constexpr std::size_t init_request_capacity = 1024U * 1024U;

    struct shared_memory
    {
        uint64_t magic{shared_memory_magic};
        uint32_t version{shared_memory_version};
        rpc::v4::secure_coroutine_module::startup_state startup_state{
            rpc::v4::secure_coroutine_module::startup_state::pending};
        ::error_code startup_error_code{0};
        uint64_t request_encoding{0};
        uint64_t request_type_id{0};
        uint64_t request_size{0};
        std::array<char, init_request_capacity> request{};
        ::streaming::spsc_queue::queue_type parent_to_runtime;
        ::streaming::spsc_queue::queue_type runtime_to_parent;
    };

    struct mapped_shared_memory
    {
        std::string path;
        shared_memory* memory{nullptr};
        bool owner{false};
    };

    class bootstrap
    {
        std::string enclave_path_;
        std::string shared_memory_file_;
        uint32_t worker_thread_count_{0};
        bool create_shared_memory_file_{false};

    public:
        bootstrap() = default;
        bootstrap(
            std::string enclave_path,
            std::string shared_memory_file,
            uint32_t worker_thread_count,
            bool create_shared_memory_file = false);

        [[nodiscard]] static const char* enclave_path_arg_name();
        [[nodiscard]] static const char* shared_memory_file_arg_name();
        [[nodiscard]] static const char* worker_thread_count_arg_name();
        [[nodiscard]] static const char* create_shared_memory_file_arg_name();

        [[nodiscard]] static std::shared_ptr<bootstrap> from_command_line(
            int argc,
            char** argv);

        [[nodiscard]] const std::string& enclave_path() const { return enclave_path_; }
        [[nodiscard]] const std::string& shared_memory_file() const { return shared_memory_file_; }
        [[nodiscard]] uint32_t worker_thread_count() const { return worker_thread_count_; }
        [[nodiscard]] bool create_shared_memory_file() const { return create_shared_memory_file_; }

        [[nodiscard]] std::vector<std::string> make_command_line() const;
    };

    [[nodiscard]] mapped_shared_memory create_shared_memory();
    [[nodiscard]] mapped_shared_memory create_shared_memory(const std::string& path);
    [[nodiscard]] mapped_shared_memory map_shared_memory(const std::string& path);
    void unmap_shared_memory(mapped_shared_memory& mapping) noexcept;

    [[nodiscard]] bool valid_shared_memory(const shared_memory& memory) noexcept;
}

#endif
