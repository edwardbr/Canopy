/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// Shared C ABI types for the dynamic_library transport.
// Included by both the host side (transport_dynamic_library) and
// the DLL side (transport_dynamic_library_dll).
//
// All boundary crossings use void* for context handles plus pointers to
// rpc:: param/result structs.  Both sides compile against the same rpc
// headers and share the same C++ runtime, so passing C++ objects by
// pointer across the in-process boundary is safe.

#ifndef CANOPY_BUILD_COROUTINE

#  include <rpc/rpc.h>

// ---------------------------------------------------------------------------
// Visibility macros for DLL-exported entry points
// ---------------------------------------------------------------------------
#  if defined(_WIN32)
#    if defined(CANOPY_DLL_BUILDING)
#      define CANOPY_DLL_EXPORT __declspec(dllexport)
#    else
#      define CANOPY_DLL_EXPORT __declspec(dllimport)
#    endif
#  else
#    define CANOPY_DLL_EXPORT __attribute__((visibility("default")))
#  endif

namespace rpc::dynamic_library
{
    // ---------------------------------------------------------------------------
    // Host -> DLL callback types
    // Provided by host child_transport, called by DLL parent_transport
    // ---------------------------------------------------------------------------
    using host_send_fn = int (*)(void* host_ctx, rpc::send_params* params, rpc::send_result* result);
    using host_post_fn = void (*)(void* host_ctx, rpc::post_params* params);
    using host_try_cast_fn = int (*)(void* host_ctx, rpc::try_cast_params* params, rpc::standard_result* result);
    using host_add_ref_fn = int (*)(void* host_ctx, rpc::add_ref_params* params, rpc::standard_result* result);
    using host_release_fn = int (*)(void* host_ctx, rpc::release_params* params, rpc::standard_result* result);
    using host_object_released_fn = void (*)(void* host_ctx, rpc::object_released_params* params);
    using host_transport_down_fn = void (*)(void* host_ctx, rpc::transport_down_params* params);
    using host_get_new_zone_id_fn
        = int (*)(void* host_ctx, rpc::get_new_zone_id_params* params, rpc::new_zone_id_result* result);

    // ---------------------------------------------------------------------------
    // Parameters passed to canopy_dll_init
    // ---------------------------------------------------------------------------
    struct dll_init_params
    {
        // Inputs
        const char* name;
        rpc::zone host_zone;
        rpc::zone dll_zone;
        const rpc::connection_settings* input_descr;
        void* host_ctx;

        // Host callback function pointers
        host_send_fn host_send;
        host_post_fn host_post;
        host_try_cast_fn host_try_cast;
        host_add_ref_fn host_add_ref;
        host_release_fn host_release;
        host_object_released_fn host_object_released;
        host_transport_down_fn host_transport_down;
        host_get_new_zone_id_fn host_get_new_zone_id;

        // Outputs: filled in by canopy_dll_init
        void* dll_ctx;                 // opaque handle to dll_context allocated by DLL
        rpc::remote_object output_obj; // DLL's root object descriptor
    };

    // ---------------------------------------------------------------------------
    // DLL-exported entry point types (resolved by host via dlsym)
    // ---------------------------------------------------------------------------
    using dll_init_fn = int (*)(dll_init_params* params);
    using dll_destroy_fn = void (*)(void* dll_ctx);
    using dll_send_fn = int (*)(void* dll_ctx, rpc::send_params* params, rpc::send_result* result);
    using dll_post_fn = void (*)(void* dll_ctx, rpc::post_params* params);
    using dll_try_cast_fn = int (*)(void* dll_ctx, rpc::try_cast_params* params, rpc::standard_result* result);
    using dll_add_ref_fn = int (*)(void* dll_ctx, rpc::add_ref_params* params, rpc::standard_result* result);
    using dll_release_fn = int (*)(void* dll_ctx, rpc::release_params* params, rpc::standard_result* result);
    using dll_object_released_fn = void (*)(void* dll_ctx, rpc::object_released_params* params);
    using dll_transport_down_fn = void (*)(void* dll_ctx, rpc::transport_down_params* params);
    using dll_get_new_zone_id_fn
        = int (*)(void* dll_ctx, rpc::get_new_zone_id_params* params, rpc::new_zone_id_result* result);

} // namespace rpc::dynamic_library

#endif // !CANOPY_BUILD_COROUTINE
