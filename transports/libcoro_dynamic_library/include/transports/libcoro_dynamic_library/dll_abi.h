/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// Shared coroutine ABI types for the libcoro_dynamic_library transport.
// Included by both the host (transport_libcoro_dynamic_library) and the DLL
// (transport_libcoro_dynamic_library_dll).
//
// COROUTINE-BUILD ONLY.  The distinct canopy_libcoro_dll_* symbol names
// prevent a non-coroutine DLL from being accidentally loaded.
//
// Design
// ------
// There is a single extern "C" entry point resolved via dlsym:
//   canopy_libcoro_dll_create(dll_create_params*, dll_create_result*)
//
// It is a plain synchronous C function that:
//   1. Constructs the DLL-side parent_transport with the host's coroutine
//      callback pointers.
//   2. Fills dll_create_result with the raw transport pointer and a set of
//      coroutine function pointers the host will call directly (no sync_wait).
//
// The host's inner_connect (a CORO_TASK) then calls the returned init_fn
// coroutine to run create_child_zone.  All temporary shared_ptrs created
// during that coroutine are released before it returns; only objects owned
// by the parent_transport (or by stubs/proxies) remain live.
//
// All in-flight coroutine calls cross the host/DLL boundary purely through
// these coro::task<T>(*)(void*, ...) function pointers — no sync_wait on
// either side.

#ifdef CANOPY_BUILD_COROUTINE

#include <memory>
#include <rpc/rpc.h>
#include <coro/scheduler.hpp>

// ---------------------------------------------------------------------------
// Visibility macro for DLL-exported entry points
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#if defined(CANOPY_LIBCORO_DLL_BUILDING)
#define CANOPY_LIBCORO_DLL_EXPORT __declspec(dllexport)
#else
#define CANOPY_LIBCORO_DLL_EXPORT __declspec(dllimport)
#endif
#else
#define CANOPY_LIBCORO_DLL_EXPORT __attribute__((visibility("default")))
#endif

namespace rpc::libcoro_dynamic_library
{
    // -----------------------------------------------------------------------
    // DLL → Host coroutine callback types
    // Provided by child_transport, called by parent_transport's outbound_*.
    // The DLL CO_AWAITs these directly — no sync_wait wrapper.
    // -----------------------------------------------------------------------
    using host_coro_send_fn = coro::task<rpc::send_result> (*)(void* host_ctx, rpc::send_params params);
    using host_coro_post_fn = coro::task<void> (*)(void* host_ctx, rpc::post_params params);
    using host_coro_try_cast_fn = coro::task<rpc::standard_result> (*)(void* host_ctx, rpc::try_cast_params params);
    using host_coro_add_ref_fn = coro::task<rpc::standard_result> (*)(void* host_ctx, rpc::add_ref_params params);
    using host_coro_release_fn = coro::task<rpc::standard_result> (*)(void* host_ctx, rpc::release_params params);
    using host_coro_object_released_fn = coro::task<void> (*)(void* host_ctx, rpc::object_released_params params);
    using host_coro_transport_down_fn = coro::task<void> (*)(void* host_ctx, rpc::transport_down_params params);
    using host_coro_get_new_zone_id_fn
        = coro::task<rpc::new_zone_id_result> (*)(void* host_ctx, rpc::get_new_zone_id_params params);
    using host_coro_release_parent_fn = void (*)(void* host_ctx);

    // -----------------------------------------------------------------------
    // Host → DLL coroutine function pointer types
    // Returned via dll_create_result; stored and called by child_transport.
    // -----------------------------------------------------------------------
    using dll_coro_send_fn = coro::task<rpc::send_result> (*)(void* dll_ctx, rpc::send_params params);
    using dll_coro_post_fn = coro::task<void> (*)(void* dll_ctx, rpc::post_params params);
    using dll_coro_try_cast_fn = coro::task<rpc::standard_result> (*)(void* dll_ctx, rpc::try_cast_params params);
    using dll_coro_add_ref_fn = coro::task<rpc::standard_result> (*)(void* dll_ctx, rpc::add_ref_params params);
    using dll_coro_release_fn = coro::task<rpc::standard_result> (*)(void* dll_ctx, rpc::release_params params);
    using dll_coro_object_released_fn = coro::task<void> (*)(void* dll_ctx, rpc::object_released_params params);
    using dll_coro_transport_down_fn = coro::task<void> (*)(void* dll_ctx, rpc::transport_down_params params);

    // Coroutine that sets up the child zone.  Called by the host's
    // inner_connect after canopy_libcoro_dll_create.  Adopts transport_ctx,
    // runs create_child_zone, releases all temporaries before returning.
    using dll_coro_init_fn = coro::task<rpc::connect_result> (*)(
        void* transport_ctx, const rpc::connection_settings* settings, std::shared_ptr<coro::scheduler>* scheduler);

    // -----------------------------------------------------------------------
    // Parameters passed to canopy_libcoro_dll_create
    // -----------------------------------------------------------------------
    struct dll_create_params
    {
        const char* name;
        rpc::zone dll_zone; // assigned by the host via get_new_zone_id
        rpc::zone host_zone;
        void* host_ctx;

        // Host coroutine callbacks (DLL calls these to reach the host)
        host_coro_send_fn host_send;
        host_coro_post_fn host_post;
        host_coro_try_cast_fn host_try_cast;
        host_coro_add_ref_fn host_add_ref;
        host_coro_release_fn host_release;
        host_coro_object_released_fn host_object_released;
        host_coro_transport_down_fn host_transport_down;
        host_coro_get_new_zone_id_fn host_get_new_zone_id;
        host_coro_release_parent_fn host_coro_release_parent;
    };

    // -----------------------------------------------------------------------
    // Result filled in by canopy_libcoro_dll_create
    // -----------------------------------------------------------------------
    struct dll_create_result
    {
        // Raw parent_transport pointer.  Owned by the service after init_fn
        // completes; the host stores this as dll_ctx_ for routing.
        void* transport_ctx = nullptr;

        // Coroutine function pointers for the host to call into the DLL
        dll_coro_init_fn init_fn = nullptr;
        dll_coro_send_fn send_fn = nullptr;
        dll_coro_post_fn post_fn = nullptr;
        dll_coro_try_cast_fn try_cast_fn = nullptr;
        dll_coro_add_ref_fn add_ref_fn = nullptr;
        dll_coro_release_fn release_fn = nullptr;
        dll_coro_object_released_fn object_released_fn = nullptr;
        dll_coro_transport_down_fn transport_down_fn = nullptr;
    };

    // Type of the single extern "C" entry point resolved via dlsym
    using dll_create_fn = void (*)(dll_create_params* params, dll_create_result* result);

} // namespace rpc::libcoro_dynamic_library

#endif // CANOPY_BUILD_COROUTINE
