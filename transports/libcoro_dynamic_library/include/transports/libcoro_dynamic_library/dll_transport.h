/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// DLL-side libcoro_dynamic_library transport.  Coroutine-build only.
//
// This header is included by shared objects that participate in the
// Canopy libcoro_dynamic_library transport.  It provides:
//
//   1. parent_transport — rpc::transport subclass that lives inside the DLL
//      and calls into the host via stored coroutine function pointers.
//      Static inbound trampolines are returned to the host as dll_coro_*_fn
//      pointers via dll_create_result.
//
//   2. init_child_zone<P, C>() — CORO_TASK template the DLL author
//      calls from their concrete do_init static function.  It adopts the raw
//      parent_transport pointer, runs create_child_zone, and releases all
//      temporaries before returning.  After it returns, only objects owned by
//      the parent_transport (or by stubs/proxies) remain live.
//
// Usage pattern (DLL author — canopy_libcoro_dll_init.cpp):
//
//   namespace rpc::libcoro_dynamic_library
//   {
//       coro::task<rpc::connect_result> canopy_libcoro_dll_init(
//           void* ctx, const rpc::connection_settings* s,
//           std::shared_ptr<coro::scheduler>* sched)
//       {
//           return init_child_zone<i_host, i_example>(
//               ctx, s, sched,
//               [](rpc::shared_ptr<i_host> h, std::shared_ptr<rpc::child_service> svc)
//                   -> CORO_TASK(rpc::service_connect_result<i_example>)
//               { CO_RETURN {rpc::error::OK(), rpc::make_shared<MyImpl>(svc, h)}; });
//       }
//   }

#ifdef CANOPY_BUILD_COROUTINE

#  include <functional>
#  include <memory>

#  include <rpc/rpc.h>
#  include <transports/libcoro_dynamic_library/dll_abi.h>

namespace rpc::libcoro_dynamic_library
{
    // Concrete init coroutine supplied by the shared object that links this
    // transport library. canopy_libcoro_dll_create is provided by the
    // transport library and binds init_fn to this symbol.
    coro::task<rpc::connect_result> canopy_libcoro_dll_init(
        void* transport_ctx, const rpc::connection_settings* settings, std::shared_ptr<coro::scheduler>* scheduler);

    // -------------------------------------------------------------------------
    // parent_transport
    //
    // Lives inside the DLL.  Calls back into the host via coroutine function
    // pointers captured during canopy_libcoro_dll_create.
    // -------------------------------------------------------------------------
    class parent_transport : public rpc::transport
    {
        void* host_ctx_;

        host_coro_send_fn host_send_;
        host_coro_post_fn host_post_;
        host_coro_try_cast_fn host_try_cast_;
        host_coro_add_ref_fn host_add_ref_;
        host_coro_release_fn host_release_;
        host_coro_object_released_fn host_object_released_;
        host_coro_transport_down_fn host_transport_down_;
        host_coro_get_new_zone_id_fn host_get_new_zone_id_;
        host_coro_release_parent_fn host_coro_release_parent_;

    public:
        parent_transport(std::string name,
            rpc::zone dll_zone,
            rpc::zone host_zone,
            void* host_ctx,
            host_coro_send_fn send,
            host_coro_post_fn post,
            host_coro_try_cast_fn try_cast,
            host_coro_add_ref_fn add_ref,
            host_coro_release_fn release,
            host_coro_object_released_fn object_released,
            host_coro_transport_down_fn transport_down,
            host_coro_get_new_zone_id_fn get_new_zone_id,
            host_coro_release_parent_fn host_coro_release_parent);

        ~parent_transport() override;

        // inner_connect / inner_accept are unused; the transport is wired
        // directly by init_child_zone.
        CORO_TASK(rpc::connect_result)
        inner_connect(std::shared_ptr<rpc::object_stub>, connection_settings) override
        {
            CO_RETURN rpc::connect_result{rpc::error::ZONE_NOT_SUPPORTED(), {}};
        }

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        // ------------------------------------------------------------------
        // Outbound — DLL → Host
        // CO_AWAITs the corresponding host coroutine function pointer.
        // ------------------------------------------------------------------
        CORO_TASK(send_result) outbound_send(send_params params) override;
        CORO_TASK(void) outbound_post(post_params params) override;
        CORO_TASK(standard_result) outbound_try_cast(try_cast_params params) override;
        CORO_TASK(standard_result) outbound_add_ref(add_ref_params params) override;
        CORO_TASK(standard_result) outbound_release(release_params params) override;
        CORO_TASK(void) outbound_object_released(object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(transport_down_params params) override;
        CORO_TASK(new_zone_id_result) outbound_get_new_zone_id(get_new_zone_id_params params) override;

        // ------------------------------------------------------------------
        // Static inbound trampolines — Host → DLL
        //
        // These are the dll_coro_*_fn pointers placed in dll_create_result.
        // The host CO_AWAITs them directly; no sync_wait needed.
        // ------------------------------------------------------------------
        static coro::task<send_result> static_inbound_send(void* ctx, send_params params)
        {
            return static_cast<parent_transport*>(ctx)->inbound_send(std::move(params));
        }

        static coro::task<void> static_inbound_post(void* ctx, post_params params)
        {
            return static_cast<parent_transport*>(ctx)->inbound_post(std::move(params));
        }

        static coro::task<standard_result> static_inbound_try_cast(void* ctx, try_cast_params params)
        {
            return static_cast<parent_transport*>(ctx)->inbound_try_cast(std::move(params));
        }

        static coro::task<standard_result> static_inbound_add_ref(void* ctx, add_ref_params params)
        {
            return static_cast<parent_transport*>(ctx)->inbound_add_ref(std::move(params));
        }

        static coro::task<standard_result> static_inbound_release(void* ctx, release_params params)
        {
            return static_cast<parent_transport*>(ctx)->inbound_release(std::move(params));
        }

        static coro::task<void> static_inbound_object_released(void* ctx, object_released_params params)
        {
            return static_cast<parent_transport*>(ctx)->inbound_object_released(std::move(params));
        }

        static coro::task<void> static_inbound_transport_down(void* ctx, transport_down_params params)
        {
            return static_cast<parent_transport*>(ctx)->inbound_transport_down(std::move(params));
        }
    };

    // -------------------------------------------------------------------------
    // init_child_zone<PARENT_INTERFACE, CHILD_INTERFACE>
    //
    // Called from the DLL author's concrete do_init static function (which has
    // the right signature for dll_coro_init_fn).  Adopts transport_ctx into a
    // shared_ptr, runs create_child_zone, then lets all temporaries go.
    //
    // After this coroutine returns, the service is the sole owner of the
    // parent_transport.  No shared_ptr to the service or transport is retained
    // by the DLL init machinery.
    // -------------------------------------------------------------------------
    template<class PARENT_INTERFACE, class CHILD_INTERFACE>
    coro::task<rpc::connect_result> init_child_zone(void* transport_ctx,
        const rpc::connection_settings* settings,
        std::shared_ptr<coro::scheduler>* scheduler,
        std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
            rpc::shared_ptr<PARENT_INTERFACE>, std::shared_ptr<rpc::child_service>)> factory)
    {
        // Adopt the raw pointer.  Any failure path that throws or returns early
        // will still delete it correctly through the shared_ptr destructor.
        auto pt = std::shared_ptr<parent_transport>(static_cast<parent_transport*>(transport_ctx));

        auto create_result = co_await rpc::child_service::create_child_zone<PARENT_INTERFACE, CHILD_INTERFACE>(
            pt->get_name().c_str(), pt, *settings, std::move(factory), *scheduler);

        if (create_result.error_code != rpc::error::OK())
            co_return rpc::connect_result{create_result.error_code, {}};

        // pt and all other temporaries go out of scope here.
        // The service is now the sole owner of the parent_transport.
        co_return rpc::connect_result{rpc::error::OK(), create_result.descriptor};
    }

} // namespace rpc::libcoro_dynamic_library

#endif // CANOPY_BUILD_COROUTINE
