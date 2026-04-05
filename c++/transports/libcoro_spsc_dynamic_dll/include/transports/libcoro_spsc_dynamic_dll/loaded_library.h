/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <condition_variable>
#  include <memory>
#  include <mutex>
#  include <string>

#  include <rpc/rpc.h>
#  include <transports/libcoro_spsc_dynamic_dll/dll_abi.h>

namespace rpc::libcoro_spsc_dynamic_dll
{
    class loaded_library
    {
        void* lib_handle_ = nullptr;
        void* runtime_ctx_ = nullptr;
        dll_stop_fn stop_fn_ = nullptr;
        std::shared_ptr<loaded_library> keep_alive_;

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        bool expired_ = false;

        static void static_on_parent_expired(void* ctx);
        void on_parent_expired();
        void release_parent() { keep_alive_.reset(); }

    public:
        loaded_library() = default;
        loaded_library(const loaded_library&) = delete;
        loaded_library& operator=(const loaded_library&) = delete;
        ~loaded_library();

        static std::shared_ptr<loaded_library> load(
            const std::string& library_path,
            const std::string& name,
            rpc::zone dll_zone,
            rpc::zone host_zone,
            streaming::spsc_queue::queue_type* send_queue,
            streaming::spsc_queue::queue_type* recv_queue,
            size_t scheduler_thread_count = 1);

        bool wait_until_expired(std::chrono::milliseconds timeout);
        bool is_expired() const;
        void stop();
    };

} // namespace rpc::libcoro_spsc_dynamic_dll

#endif // CANOPY_BUILD_COROUTINE
