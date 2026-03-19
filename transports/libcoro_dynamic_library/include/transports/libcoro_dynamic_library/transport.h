/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// Host-side libcoro_dynamic_library transport.  Coroutine-build only.
//
// child_transport lives in the host zone and manages the lifetime of a loaded
// shared object.  It resolves a single extern "C" symbol
// (canopy_libcoro_dll_create) via dlsym, calls it synchronously to obtain the
// DLL-side parent_transport pointer and a set of coroutine function pointers,
// then CO_AWAITs the returned init_fn to set up the child zone.
//
// All cross-boundary calls use coroutine function pointers so no sync_wait is
// needed on either side.  The host's static inbound trampolines (host_inbound_*)
// are passed as host_coro_*_fn callbacks to the DLL during create.

#ifdef CANOPY_BUILD_COROUTINE

#include <string>
#include <memory>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <rpc/rpc.h>
#include <transports/libcoro_dynamic_library/dll_abi.h>

namespace rpc::libcoro_dynamic_library
{
    class child_transport : public rpc::transport
    {
        std::shared_ptr<rpc::transport> keep_alive_;

        // DLL coroutine function pointers (host calls these to reach the DLL)
        dll_coro_send_fn dll_coro_send_ = nullptr;
        dll_coro_post_fn dll_coro_post_ = nullptr;
        dll_coro_try_cast_fn dll_coro_try_cast_ = nullptr;
        dll_coro_add_ref_fn dll_coro_add_ref_ = nullptr;
        dll_coro_release_fn dll_coro_release_ = nullptr;
        dll_coro_object_released_fn dll_coro_object_released_ = nullptr;
        dll_coro_transport_down_fn dll_coro_transport_down_ = nullptr;

        // Opaque handle to the DLL-side parent_transport
        void* dll_ctx_ = nullptr;

        // Native handle to the loaded shared object
        void* lib_handle_ = nullptr;

        // Path to the shared object
        std::string library_path_;

        // -----------------------------------------------------------------------
        // Static host-inbound trampolines
        // These are passed as host_coro_*_fn callbacks to canopy_libcoro_dll_create.
        // The DLL's parent_transport CO_AWAITs them to call into the host.
        // -----------------------------------------------------------------------
        static coro::task<send_result> host_inbound_send(void* ctx, send_params params)
        {
            return static_cast<child_transport*>(ctx)->inbound_send(std::move(params));
        }

        static coro::task<void> host_inbound_post(void* ctx, post_params params)
        {
            return static_cast<child_transport*>(ctx)->inbound_post(std::move(params));
        }

        static coro::task<standard_result> host_inbound_try_cast(void* ctx, try_cast_params params)
        {
            return static_cast<child_transport*>(ctx)->inbound_try_cast(std::move(params));
        }

        static coro::task<standard_result> host_inbound_add_ref(void* ctx, add_ref_params params)
        {
            return static_cast<child_transport*>(ctx)->inbound_add_ref(std::move(params));
        }

        static coro::task<standard_result> host_inbound_release(void* ctx, release_params params)
        {
            return static_cast<child_transport*>(ctx)->inbound_release(std::move(params));
        }

        static coro::task<void> host_inbound_object_released(void* ctx, object_released_params params)
        {
            return static_cast<child_transport*>(ctx)->inbound_object_released(std::move(params));
        }

        static coro::task<void> host_inbound_transport_down(void* ctx, transport_down_params params)
        {
            return static_cast<child_transport*>(ctx)->inbound_transport_down(std::move(params));
        }

        static coro::task<new_zone_id_result> host_inbound_get_new_zone_id(void* ctx, get_new_zone_id_params params)
        {
            auto* t = static_cast<child_transport*>(ctx);
            auto svc = t->get_service();
            if (!svc)
                co_return new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
            co_return co_await svc->get_new_zone_id(std::move(params));
        }

        static void host_coro_release_parent(void* ctx) { return static_cast<child_transport*>(ctx)->release_parent(); }

        // Load the shared object and resolve canopy_libcoro_dll_create
        int load_library();

        // Close the shared object (idempotent)
        void unload_library();

        // Resolve a symbol by name; returns nullptr on failure
        void* resolve_symbol(const char* name) const;

        void release_parent() { keep_alive_.reset(); }

    protected:
        void on_destination_count_zero() override;

    public:
        child_transport(std::string name, std::shared_ptr<rpc::service> service, std::string library_path);

        ~child_transport() override;

        void set_status(rpc::transport_status status) override;

        CORO_TASK(rpc::connect_result)
        inner_connect(std::shared_ptr<rpc::object_stub> stub, connection_settings input_descr) override;

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        // Outbound — host → DLL (CO_AWAITs the DLL coroutine function pointer)
        CORO_TASK(send_result) outbound_send(send_params params) override;
        CORO_TASK(void) outbound_post(post_params params) override;
        CORO_TASK(standard_result) outbound_try_cast(try_cast_params params) override;
        CORO_TASK(standard_result) outbound_add_ref(add_ref_params params) override;
        CORO_TASK(standard_result) outbound_release(release_params params) override;
        CORO_TASK(void) outbound_object_released(object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(transport_down_params params) override;
    };

} // namespace rpc::libcoro_dynamic_library

#endif // CANOPY_BUILD_COROUTINE
