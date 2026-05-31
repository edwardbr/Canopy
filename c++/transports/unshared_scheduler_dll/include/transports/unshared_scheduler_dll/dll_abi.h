/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// Shared coroutine ABI types for the unshared_scheduler_dll transport.
// Included by both the host (transport_unshared_scheduler_dll) and the DLL
// (transport_unshared_scheduler_dll_runtime).
//
// COROUTINE-BUILD ONLY.  The distinct canopy_unshared_scheduler_dll_* symbol names
// prevent a non-coroutine DLL from being accidentally loaded.
//
// The ABI deliberately avoids exporting coro::task.  Each side owns its own
// scheduler and coroutine frames.  Cross-boundary calls are non-blocking
// begin_* functions plus completion callbacks owned by the caller side.

#ifdef CANOPY_BUILD_COROUTINE

#  include <rpc/rpc.h>

// ---------------------------------------------------------------------------
// Visibility macro for DLL-exported entry points
// ---------------------------------------------------------------------------
#  if defined(_WIN32)
#    if defined(CANOPY_UNSHARED_SCHEDULER_DLL_BUILDING)
#      define CANOPY_UNSHARED_SCHEDULER_DLL_EXPORT __declspec(dllexport)
#    else
#      define CANOPY_UNSHARED_SCHEDULER_DLL_EXPORT __declspec(dllimport)
#    endif
#  else
#    define CANOPY_UNSHARED_SCHEDULER_DLL_EXPORT __attribute__((visibility("default")))
#  endif

namespace rpc::unshared_scheduler_dll
{
    using complete_send_fn = void (*)(
        void* completion_ctx,
        rpc::send_result* result);
    using complete_standard_fn = void (*)(
        void* completion_ctx,
        rpc::standard_result* result);
    using complete_void_fn = void (*)(void* completion_ctx);
    using complete_new_zone_id_fn = void (*)(
        void* completion_ctx,
        rpc::new_zone_id_result* result);

    using host_begin_send_fn = int (*)(
        void* host_ctx,
        rpc::send_params params,
        void* completion_ctx,
        complete_send_fn complete);
    using host_begin_post_fn = int (*)(
        void* host_ctx,
        rpc::post_params params,
        void* completion_ctx,
        complete_void_fn complete);
    using host_begin_try_cast_fn = int (*)(
        void* host_ctx,
        rpc::try_cast_params params,
        void* completion_ctx,
        complete_standard_fn complete);
    using host_begin_add_ref_fn = int (*)(
        void* host_ctx,
        rpc::add_ref_params params,
        void* completion_ctx,
        complete_standard_fn complete);
    using host_begin_release_fn = int (*)(
        void* host_ctx,
        rpc::release_params params,
        void* completion_ctx,
        complete_standard_fn complete);
    using host_begin_object_released_fn = int (*)(
        void* host_ctx,
        rpc::object_released_params params,
        void* completion_ctx,
        complete_void_fn complete);
    using host_begin_transport_down_fn = int (*)(
        void* host_ctx,
        rpc::transport_down_params params,
        void* completion_ctx,
        complete_void_fn complete);
    using host_begin_get_new_zone_id_fn = int (*)(
        void* host_ctx,
        rpc::get_new_zone_id_params params,
        void* completion_ctx,
        complete_new_zone_id_fn complete);
    using host_coro_release_parent_fn = void (*)(void* host_ctx);

    using dll_begin_send_fn = int (*)(
        void* dll_ctx,
        rpc::send_params params,
        void* completion_ctx,
        complete_send_fn complete);
    using dll_begin_post_fn = int (*)(
        void* dll_ctx,
        rpc::post_params params,
        void* completion_ctx,
        complete_void_fn complete);
    using dll_begin_try_cast_fn = int (*)(
        void* dll_ctx,
        rpc::try_cast_params params,
        void* completion_ctx,
        complete_standard_fn complete);
    using dll_begin_add_ref_fn = int (*)(
        void* dll_ctx,
        rpc::add_ref_params params,
        void* completion_ctx,
        complete_standard_fn complete);
    using dll_begin_release_fn = int (*)(
        void* dll_ctx,
        rpc::release_params params,
        void* completion_ctx,
        complete_standard_fn complete);
    using dll_begin_object_released_fn = int (*)(
        void* dll_ctx,
        rpc::object_released_params params,
        void* completion_ctx,
        complete_void_fn complete);
    using dll_begin_transport_down_fn = int (*)(
        void* dll_ctx,
        rpc::transport_down_params params,
        void* completion_ctx,
        complete_void_fn complete);

    struct dll_begin_table
    {
        dll_begin_send_fn send = nullptr;
        dll_begin_post_fn post = nullptr;
        dll_begin_try_cast_fn try_cast = nullptr;
        dll_begin_add_ref_fn add_ref = nullptr;
        dll_begin_release_fn release = nullptr;
        dll_begin_object_released_fn object_released = nullptr;
        dll_begin_transport_down_fn transport_down = nullptr;
    };

    struct dll_start_result
    {
        void* runtime_ctx = nullptr;
        dll_begin_table begin;
        rpc::connect_result connect_result{rpc::error::TRANSPORT_ERROR(), {}};
    };

    using dll_ready_fn = void (*)(
        void* ready_ctx,
        dll_start_result* result);

    struct dll_start_params
    {
        const char* name = nullptr;
        rpc::zone dll_zone = {};
        rpc::zone host_zone = {};
        const rpc::connection_settings* settings = nullptr;
        void* host_ctx = nullptr;

        host_begin_send_fn host_send = nullptr;
        host_begin_post_fn host_post = nullptr;
        host_begin_try_cast_fn host_try_cast = nullptr;
        host_begin_add_ref_fn host_add_ref = nullptr;
        host_begin_release_fn host_release = nullptr;
        host_begin_object_released_fn host_object_released = nullptr;
        host_begin_transport_down_fn host_transport_down = nullptr;
        host_begin_get_new_zone_id_fn host_get_new_zone_id = nullptr;
        host_coro_release_parent_fn host_coro_release_parent = nullptr;

        void* ready_ctx = nullptr;
        dll_ready_fn ready = nullptr;
    };

    using dll_start_fn = void (*)(dll_start_params* params);

} // namespace rpc::unshared_scheduler_dll

#endif // CANOPY_BUILD_COROUTINE
