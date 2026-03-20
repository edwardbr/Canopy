/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// Host-side dynamic_library transport.
//
// child_transport lives in the parent/host zone and manages the lifetime of a
// loaded shared object.  It mirrors rpc::local::child_transport in structure:
//
//   parent zone                          child zone (inside DLL)
//   ──────────────────────               ────────────────────────────
//   child_transport  ──sends──>  canopy_dll_send / canopy_dll_* entry points
//                    <─sends──   host callback function pointers
//
// Symbol isolation: the DLL is opened with RTLD_NOW | RTLD_LOCAL so its
// symbols are invisible to the rest of the process (and vice-versa for the OS
// symbol table), except for the exported canopy_dll_* entry points and
// whatever the RPC library itself makes available.
//
// Lifetime: once all destination ref-counts on child_transport drop to zero
// (i.e. the child service is gone), canopy_dll_destroy is called and then the
// shared object is dlclose-d.

#ifndef CANOPY_BUILD_COROUTINE

#  include <functional>
#  include <string>
#  include <atomic>

#  if defined(_WIN32)
#    include <windows.h>
#  else
#    include <dlfcn.h>
#  endif

#  include <rpc/rpc.h>
#  include <transports/dynamic_library/dll_abi.h>

namespace rpc::dynamic_library
{
    class child_transport : public rpc::transport
    {
        // All resolved DLL entry points
        dll_init_fn dll_init_ = nullptr;
        dll_destroy_fn dll_destroy_ = nullptr;
        dll_send_fn dll_send_ = nullptr;
        dll_post_fn dll_post_ = nullptr;
        dll_try_cast_fn dll_try_cast_ = nullptr;
        dll_add_ref_fn dll_add_ref_ = nullptr;
        dll_release_fn dll_release_ = nullptr;
        dll_object_released_fn dll_object_released_ = nullptr;
        dll_transport_down_fn dll_transport_down_ = nullptr;
        dll_get_new_zone_id_fn dll_get_new_zone_id_ = nullptr;

        // Opaque handle to the DLL-side dll_context (owned by the DLL)
        void* dll_ctx_ = nullptr;

        // Native handle to the loaded shared object
        void* lib_handle_ = nullptr;

        // Path to the shared object
        std::string library_path_;

        // Called by parent_transport (DLL side) when it disconnects
        void on_dll_disconnected();

        // -----------------------------------------------------------------------
        // Static host-callback shims: passed as function pointers to canopy_dll_init
        // The DLL's parent_transport calls these to reach the host inbound handlers.
        // -----------------------------------------------------------------------
        static int cb_send(void* host_ctx, rpc::send_params* params, rpc::send_result* result);
        static void cb_post(void* host_ctx, rpc::post_params* params);
        static int cb_try_cast(void* host_ctx, rpc::try_cast_params* params, rpc::standard_result* result);
        static int cb_add_ref(void* host_ctx, rpc::add_ref_params* params, rpc::standard_result* result);
        static int cb_release(void* host_ctx, rpc::release_params* params, rpc::standard_result* result);
        static void cb_object_released(void* host_ctx, rpc::object_released_params* params);
        static void cb_transport_down(void* host_ctx, rpc::transport_down_params* params);
        static int cb_get_new_zone_id(void* host_ctx, rpc::get_new_zone_id_params* params, rpc::new_zone_id_result* result);

        // Load a symbol from the shared object; returns nullptr on failure
        void* resolve_symbol(const char* name) const;

        // Load and resolve all DLL entry points; returns error code
        int load_library();

        // Tear down DLL and close the shared object (idempotent)
        void unload_library();

    protected:
        // Called by base class when outbound destination count reaches zero
        void on_destination_count_zero() override;

    public:
        child_transport(std::string name, std::shared_ptr<rpc::service> service, std::string library_path);

        ~child_transport() override;

        // Override to propagate disconnect into the DLL
        void set_status(rpc::transport_status status) override;

        CORO_TASK(rpc::connect_result)
        inner_connect(std::shared_ptr<rpc::object_stub> stub, connection_settings input_descr) override;

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        // Outbound i_marshaller interface — sends from host to DLL
        CORO_TASK(send_result) outbound_send(send_params params) override;
        CORO_TASK(void) outbound_post(post_params params) override;
        CORO_TASK(standard_result) outbound_try_cast(try_cast_params params) override;
        CORO_TASK(standard_result) outbound_add_ref(add_ref_params params) override;
        CORO_TASK(standard_result) outbound_release(release_params params) override;
        CORO_TASK(void) outbound_object_released(object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(transport_down_params params) override;
    };

} // namespace rpc::dynamic_library

#endif // !CANOPY_BUILD_COROUTINE
