/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <rpc/rpc.h>
#  include <streaming/spsc_queue/stream.h>

#  if defined(_WIN32)
#    if defined(CANOPY_LIBCORO_SPSC_DLL_BUILDING)
#      define CANOPY_LIBCORO_SPSC_DLL_EXPORT __declspec(dllexport)
#    else
#      define CANOPY_LIBCORO_SPSC_DLL_EXPORT __declspec(dllimport)
#    endif
#  else
#    define CANOPY_LIBCORO_SPSC_DLL_EXPORT __attribute__((visibility("default")))
#  endif

namespace rpc::libcoro_spsc_dynamic_dll
{
    struct queue_pair
    {
        streaming::spsc_queue::queue_type host_to_dll;
        streaming::spsc_queue::queue_type dll_to_host;
    };

    using parent_expired_fn = void (*)(void* callback_ctx);
    using dll_stop_fn = void (*)(void* runtime_ctx);

    struct dll_start_params
    {
        const char* name = nullptr;
        rpc::zone dll_zone;
        rpc::zone host_zone;
        streaming::spsc_queue::queue_type* send_queue = nullptr;
        streaming::spsc_queue::queue_type* recv_queue = nullptr;
        void* callback_ctx = nullptr;
        parent_expired_fn on_parent_expired = nullptr;
    };

    struct dll_start_result
    {
        int error_code = rpc::error::OK();
        void* runtime_ctx = nullptr;
        dll_stop_fn stop_fn = nullptr;
    };

    using dll_start_fn = void (*)(dll_start_params* params, dll_start_result* result);

} // namespace rpc::libcoro_spsc_dynamic_dll

#endif // CANOPY_BUILD_COROUTINE
