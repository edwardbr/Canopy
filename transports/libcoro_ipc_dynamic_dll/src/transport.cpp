/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#include <transports/libcoro_ipc_dynamic_dll/transport.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <streaming/spsc_queue/stream.h>

namespace rpc::libcoro_ipc_dynamic_dll
{
    namespace
    {
        void* load_symbol(void* lib_handle, const char* symbol)
        {
#if defined(_WIN32)
            return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib_handle), symbol));
#else
            return dlsym(lib_handle, symbol);
#endif
        }
    }

    void loaded_library::static_on_parent_expired(void* ctx)
    {
        static_cast<loaded_library*>(ctx)->on_parent_expired();
    }

    void loaded_library::on_parent_expired()
    {
        {
            std::lock_guard lock(mutex_);
            expired_ = true;
        }
        release_parent();
        cv_.notify_all();
    }

    loaded_library::~loaded_library()
    {
        stop();
    }

    std::shared_ptr<loaded_library> loaded_library::load(const std::string& library_path,
        const std::string& name,
        rpc::zone dll_zone,
        rpc::zone host_zone,
        streaming::spsc_queue::queue_type* send_queue,
        streaming::spsc_queue::queue_type* recv_queue)
    {
        auto result = std::shared_ptr<loaded_library>(new loaded_library());
        result->keep_alive_ = result;

#if defined(_WIN32)
        result->lib_handle_ = LoadLibraryA(library_path.c_str());
#else
        result->lib_handle_ = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
#endif
        if (!result->lib_handle_)
        {
            RPC_ERROR("[libcoro_ipc_dynamic_dll] failed to load {}", library_path);
            return nullptr;
        }

        auto start_fn = reinterpret_cast<dll_start_fn>(load_symbol(result->lib_handle_, "canopy_libcoro_ipc_dll_start"));
        if (!start_fn)
        {
            RPC_ERROR("[libcoro_ipc_dynamic_dll] canopy_libcoro_ipc_dll_start not found in {}", library_path);
            result->stop();
            return nullptr;
        }

        dll_start_params params{};
        params.name = name.c_str();
        params.dll_zone = dll_zone;
        params.host_zone = host_zone;
        params.send_queue = send_queue;
        params.recv_queue = recv_queue;
        params.callback_ctx = result.get();
        params.on_parent_expired = &loaded_library::static_on_parent_expired;

        dll_start_result start_result{};
        start_fn(&params, &start_result);
        if (start_result.error_code != rpc::error::OK() || !start_result.runtime_ctx || !start_result.stop_fn)
        {
            RPC_ERROR("[libcoro_ipc_dynamic_dll] failed to start {}", library_path);
            result->stop();
            return nullptr;
        }

        result->runtime_ctx_ = start_result.runtime_ctx;
        result->stop_fn_ = start_result.stop_fn;
        return result;
    }

    bool loaded_library::wait_until_expired(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return expired_; });
    }

    bool loaded_library::is_expired() const
    {
        std::lock_guard lock(mutex_);
        return expired_;
    }

    void loaded_library::stop()
    {
        if (stop_fn_ && runtime_ctx_)
        {
            stop_fn_(runtime_ctx_);
            stop_fn_ = nullptr;
            runtime_ctx_ = nullptr;
        }

        if (lib_handle_)
        {
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(lib_handle_));
#else
            dlclose(lib_handle_);
#endif
            lib_handle_ = nullptr;
        }
    }

    std::shared_ptr<rpc::stream_transport::transport> make_client(
        std::string name, const std::shared_ptr<rpc::service>& service, queue_pair* queues)
    {
        auto stream = std::make_shared<streaming::spsc_queue::stream>(
            &queues->host_to_dll, &queues->dll_to_host, service->get_scheduler());
        return rpc::stream_transport::make_client(std::move(name), service, std::move(stream));
    }

} // namespace rpc::libcoro_ipc_dynamic_dll

#endif // CANOPY_BUILD_COROUTINE
