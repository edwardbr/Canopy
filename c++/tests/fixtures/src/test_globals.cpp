/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "test_globals.h"
#include <array>
#include <filesystem>
#include <string>

#ifndef _WIN32
#  include <unistd.h>
#endif

namespace
{
    std::string resolve_enclave_path(const std::string& filename)
    {
        namespace fs = std::filesystem;

        const fs::path direct_path(filename);
        if (fs::exists(direct_path))
        {
            return fs::absolute(direct_path).string();
        }

#ifndef _WIN32
        std::array<char, 4096> exe_path_buffer{};
        auto exe_path_length = readlink("/proc/self/exe", exe_path_buffer.data(), exe_path_buffer.size() - 1);
        if (exe_path_length > 0)
        {
            exe_path_buffer[exe_path_length] = '\0';
            auto candidate = fs::path(exe_path_buffer.data()).parent_path() / direct_path.filename();
            if (fs::exists(candidate))
            {
                return candidate.string();
            }
        }
#endif

        auto cwd_candidate = fs::current_path() / direct_path.filename();
        if (fs::exists(cwd_candidate))
        {
            return cwd_candidate.string();
        }

        return filename;
    }
}

// Global variable definitions
std::weak_ptr<rpc::service> current_host_service; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

#ifdef _WIN32 // windows
std::string enclave_path = resolve_enclave_path(
    "./marshal_test_enclave.signed.dll"); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::string coroutine_enclave_path = resolve_enclave_path(
    "./marshal_test_coroutine_enclave.signed.dll"); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::string sgx_coroutine_test_enclave_path = resolve_enclave_path(
    "./sgx_coroutine_test_enclave.signed.dll"); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
#else                                           // Linux
std::string enclave_path = resolve_enclave_path(
    "./libmarshal_test_enclave.signed.so"); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::string coroutine_enclave_path = resolve_enclave_path(
    "./libmarshal_test_coroutine_enclave.signed.so"); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::string sgx_coroutine_test_enclave_path = resolve_enclave_path(
    "./libsgx_coroutine_test_enclave.signed.so"); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
#endif
