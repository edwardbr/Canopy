/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/fake_sgx/runtime.h>
#include <sgx_trts.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <unistd.h>

namespace canopy::fake_sgx
{
    struct enclave_record
    {
        void* handle = nullptr;
        std::string path;
        bool remove_on_destroy = true;
    };

    struct memory_range
    {
        sgx_enclave_id_t enclave_id = 0;
        std::uint64_t call_id = 0;
        const std::byte* begin = nullptr;
        const std::byte* end = nullptr;
        memory_kind kind = memory_kind::outside;
    };

    std::mutex& registry_mutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    auto enclave_registry() -> std::unordered_map<
        sgx_enclave_id_t,
        enclave_record>&
    {
        static std::unordered_map<sgx_enclave_id_t, enclave_record> registry;
        return registry;
    }

    auto memory_registry() -> std::vector<memory_range>&
    {
        static std::vector<memory_range> registry;
        return registry;
    }

    auto next_enclave_id() -> sgx_enclave_id_t
    {
        static std::atomic<sgx_enclave_id_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    auto next_call_id() -> std::uint64_t
    {
        static std::atomic<std::uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    auto make_unique_enclave_image(
        const char* file_name,
        sgx_enclave_id_t enclave_id,
        std::filesystem::path& copied_path) -> bool
    {
        std::error_code error;
        auto source = std::filesystem::path(file_name);
        auto temp_dir = std::filesystem::temp_directory_path(error);
        if (error)
            return false;

        copied_path = temp_dir
                      / ("canopy_fake_sgx_" + std::to_string(static_cast<unsigned long long>(::getpid())) + "_"
                          + std::to_string(static_cast<unsigned long long>(enclave_id)) + source.extension().string());

        std::filesystem::copy_file(source, copied_path, std::filesystem::copy_options::overwrite_existing, error);
        return !error;
    }

    auto preserve_enclave_images_for_symbolizer() -> bool
    {
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
        return true;
#  endif
#endif
#if defined(__SANITIZE_THREAD__)
        return true;
#else
        return false;
#endif
    }

    thread_local sgx_enclave_id_t current_enclave_id = 0;

    auto range_contains(
        const memory_range& range,
        const void* address,
        std::size_t size) -> bool
    {
        if (size == 0)
            return true;
        if (!address)
            return false;

        const auto* begin = static_cast<const std::byte*>(address);
        const auto* end = begin + size;
        return begin >= range.begin && end <= range.end;
    }

    auto is_registered_range(
        const void* address,
        std::size_t size,
        memory_kind kind) -> bool
    {
        if (size == 0)
            return true;

        std::lock_guard lock(registry_mutex());
        for (const auto& range : memory_registry())
        {
            if (range.enclave_id == current_enclave_id && range.kind == kind && range_contains(range, address, size))
                return true;
        }
        return false;
    }

    auto add_memory_range(
        sgx_enclave_id_t enclave_id,
        std::uint64_t call_id,
        const void* address,
        std::size_t size,
        memory_kind kind) -> void
    {
        if (!address || size == 0 || enclave_id == 0 || call_id == 0)
            return;

        const auto* begin = static_cast<const std::byte*>(address);
        std::lock_guard lock(registry_mutex());
        memory_registry().push_back(memory_range{enclave_id, call_id, begin, begin + size, kind});
    }

    auto remove_call_ranges(
        sgx_enclave_id_t enclave_id,
        std::uint64_t call_id) -> void
    {
        std::lock_guard lock(registry_mutex());
        auto& ranges = memory_registry();
        ranges.erase(
            std::remove_if(
                ranges.begin(),
                ranges.end(),
                [enclave_id, call_id](const memory_range& range)
                { return range.enclave_id == enclave_id && range.call_id == call_id; }),
            ranges.end());
    }

    scoped_enclave_call::scoped_enclave_call(sgx_enclave_id_t enclave_id) noexcept
        : enclave_id_(enclave_id)
        , previous_enclave_id_(current_enclave_id)
        , call_id_(next_call_id())
    {
        std::lock_guard lock(registry_mutex());
        if (!enclave_registry().contains(enclave_id_))
        {
            status_ = SGX_ERROR_INVALID_ENCLAVE;
            return;
        }

        current_enclave_id = enclave_id_;
    }

    scoped_enclave_call::~scoped_enclave_call()
    {
        remove_call_ranges(enclave_id_, call_id_);
        current_enclave_id = previous_enclave_id_;
    }

    auto scoped_enclave_call::status() const noexcept -> sgx_status_t
    {
        return status_;
    }

    auto scoped_enclave_call::ok() const noexcept -> bool
    {
        return status_ == SGX_SUCCESS;
    }

    auto scoped_enclave_call::symbol(const char* name) const noexcept -> void*
    {
        if (!ok() || !name)
            return nullptr;

        std::lock_guard lock(registry_mutex());
        auto found = enclave_registry().find(enclave_id_);
        if (found == enclave_registry().end())
            return nullptr;
        return dlsym(found->second.handle, name);
    }

    auto scoped_enclave_call::add_inside(
        const void* address,
        std::size_t size) noexcept -> void
    {
        add_memory_range(enclave_id_, call_id_, address, size, memory_kind::inside);
    }

    auto scoped_enclave_call::add_outside(
        const void* address,
        std::size_t size) noexcept -> void
    {
        add_memory_range(enclave_id_, call_id_, address, size, memory_kind::outside);
    }
}

extern "C"
{
    void* __cxa_current_primary_exception() noexcept
    {
        return nullptr;
    }

    void __cxa_rethrow_primary_exception(void*) noexcept
    {
        std::terminate();
    }

    void __cxa_increment_exception_refcount(void*) noexcept { }

    void __cxa_decrement_exception_refcount(void*) noexcept { }

    sgx_status_t sgx_create_enclave(
        const char* file_name,
        int debug,
        sgx_launch_token_t* launch_token,
        int* launch_token_updated,
        sgx_enclave_id_t* enclave_id,
        void* misc_attr)
    {
        (void)debug;
        (void)launch_token;
        (void)misc_attr;

        if (launch_token_updated)
            *launch_token_updated = 0;
        if (!file_name || !enclave_id)
            return SGX_ERROR_INVALID_PARAMETER;

        auto id = canopy::fake_sgx::next_enclave_id();
        std::filesystem::path copied_path;
        if (!canopy::fake_sgx::make_unique_enclave_image(file_name, id, copied_path))
            return SGX_ERROR_UNEXPECTED;

        void* handle = dlopen(copied_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            std::error_code remove_error;
            std::filesystem::remove(copied_path, remove_error);
            return SGX_ERROR_UNEXPECTED;
        }

        {
            std::lock_guard lock(canopy::fake_sgx::registry_mutex());
            canopy::fake_sgx::enclave_registry().emplace(
                id,
                canopy::fake_sgx::enclave_record{
                    handle, copied_path.string(), !canopy::fake_sgx::preserve_enclave_images_for_symbolizer()});
        }

        *enclave_id = id;
        return SGX_SUCCESS;
    }

    sgx_status_t sgx_destroy_enclave(sgx_enclave_id_t enclave_id)
    {
        void* handle = nullptr;
        std::string path;
        bool remove_on_destroy = false;
        {
            std::lock_guard lock(canopy::fake_sgx::registry_mutex());
            auto& registry = canopy::fake_sgx::enclave_registry();
            auto found = registry.find(enclave_id);
            if (found == registry.end())
                return SGX_ERROR_INVALID_ENCLAVE;
            handle = found->second.handle;
            path = found->second.path;
            remove_on_destroy = found->second.remove_on_destroy;
            registry.erase(found);

            auto& ranges = canopy::fake_sgx::memory_registry();
            ranges.erase(
                std::remove_if(
                    ranges.begin(),
                    ranges.end(),
                    [enclave_id](const canopy::fake_sgx::memory_range& range) { return range.enclave_id == enclave_id; }),
                ranges.end());
        }

        auto status = SGX_SUCCESS;
        if (handle && dlclose(handle) != 0)
            status = SGX_ERROR_UNEXPECTED;
        if (remove_on_destroy && !path.empty())
        {
            std::error_code remove_error;
            std::filesystem::remove(path, remove_error);
        }
        return status;
    }

    int sgx_is_within_enclave(
        const void* address,
        std::size_t size)
    {
        return canopy::fake_sgx::is_registered_range(address, size, canopy::fake_sgx::memory_kind::inside) ? 1 : 0;
    }

    int sgx_is_outside_enclave(
        const void* address,
        std::size_t size)
    {
        return canopy::fake_sgx::is_registered_range(address, size, canopy::fake_sgx::memory_kind::outside) ? 1 : 0;
    }
}
