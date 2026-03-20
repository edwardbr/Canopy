/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// DLL-side dynamic_library transport.
//
// This header is included by shared objects that want to participate in the
// Canopy dynamic-library transport.  It provides:
//
//   1.  parent_transport — an rpc::transport subclass that lives inside the
//       DLL and communicates back to the host via stored function pointers.
//
//   2.  dll_context — the heap-allocated struct that the DLL allocates during
//       canopy_dll_init and passes back as the opaque dll_ctx handle.  The
//       host passes it to every subsequent canopy_dll_* entry point.
//
//   3.  init_child_zone<PARENT_INTERFACE, CHILD_INTERFACE>() — a template
//       helper the DLL author calls from their canopy_dll_init implementation
//       to wire everything up without boilerplate.
//
// Usage pattern (DLL author):
//
//   #include <transports/dynamic_library/dll_transport.h>
//
//   extern "C" CANOPY_DLL_EXPORT
//   int canopy_dll_init(rpc::dynamic_library::dll_init_params* p)
//   {
//       return rpc::dynamic_library::init_child_zone<IHostService, IMyService>(
//           p,
//           [](rpc::shared_ptr<IHostService> host,
//              std::shared_ptr<rpc::child_service> svc)
//           {
//               return rpc::service_connect_result<IMyService>{
//                   rpc::error::OK(),
//                   rpc::make_shared<MyServiceImpl>(svc, host)};
//           });
//   }
//
// The remaining canopy_dll_* entry points (canopy_dll_send, etc.) are
// provided as compiled symbols by transport_dynamic_library_dll and do
// not need to be written by the DLL author.

#ifndef CANOPY_BUILD_COROUTINE

#  include <functional>
#  include <memory>
#  include <atomic>

#  include <rpc/rpc.h>
#  include <transports/dynamic_library/dll_abi.h>

namespace rpc::dynamic_library
{
    // -------------------------------------------------------------------------
    // parent_transport
    //
    // Lives inside the DLL.  Calls back into the host via the function pointers
    // captured during canopy_dll_init.  This is the DLL-side peer of
    // child_transport in the host.
    // -------------------------------------------------------------------------
    class parent_transport : public rpc::transport
    {
        void* host_ctx_;

        host_send_fn host_send_;
        host_post_fn host_post_;
        host_try_cast_fn host_try_cast_;
        host_add_ref_fn host_add_ref_;
        host_release_fn host_release_;
        host_object_released_fn host_object_released_;
        host_transport_down_fn host_transport_down_;
        host_get_new_zone_id_fn host_get_new_zone_id_;

    public:
        parent_transport(std::string name,
            rpc::zone dll_zone,
            rpc::zone host_zone,
            void* host_ctx,
            host_send_fn send,
            host_post_fn post,
            host_try_cast_fn try_cast,
            host_add_ref_fn add_ref,
            host_release_fn release,
            host_object_released_fn object_released,
            host_transport_down_fn transport_down,
            host_get_new_zone_id_fn get_new_zone_id);

        ~parent_transport() override CANOPY_DEFAULT_DESTRUCTOR;

        // Propagate disconnect back to host
        void set_status(rpc::transport_status status) override;

        CORO_TASK(rpc::connect_result)
        inner_connect(std::shared_ptr<rpc::object_stub> stub, connection_settings input_descr) override
        {
            std::ignore = stub;
            std::ignore = input_descr;
            // Already connected at construction time
            CO_RETURN rpc::connect_result{rpc::error::ZONE_NOT_SUPPORTED(), {}};
        }

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        // Outbound i_marshaller interface — sends from DLL to host
        CORO_TASK(send_result) outbound_send(send_params params) override;
        CORO_TASK(void) outbound_post(post_params params) override;
        CORO_TASK(standard_result) outbound_try_cast(try_cast_params params) override;
        CORO_TASK(standard_result) outbound_add_ref(add_ref_params params) override;
        CORO_TASK(standard_result) outbound_release(release_params params) override;
        CORO_TASK(void) outbound_object_released(object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(transport_down_params params) override;
        CORO_TASK(new_zone_id_result) outbound_get_new_zone_id(get_new_zone_id_params params) override;
    };

    // -------------------------------------------------------------------------
    // dll_context
    //
    // Heap-allocated by init_child_zone, returned as dll_ctx to the host.
    // The host passes it to every canopy_dll_* call.  Freed by canopy_dll_destroy.
    // -------------------------------------------------------------------------
    struct dll_context
    {
        std::shared_ptr<parent_transport> transport;
        std::shared_ptr<rpc::child_service> service;
        std::atomic<bool> destroyed{false};
    };

    // -------------------------------------------------------------------------
    // init_child_zone<PARENT_INTERFACE, CHILD_INTERFACE>
    //
    // Template helper called from canopy_dll_init.  Creates the parent_transport
    // and child_service, invokes the user factory, and writes outputs back into
    // *params.  Returns rpc::error::OK() on success.
    // -------------------------------------------------------------------------
    template<class PARENT_INTERFACE, class CHILD_INTERFACE>
    int init_child_zone(dll_init_params* params,
        std::function<rpc::service_connect_result<CHILD_INTERFACE>(
            rpc::shared_ptr<PARENT_INTERFACE>, std::shared_ptr<rpc::child_service>)> factory)
    {
        // Create the parent_transport for the DLL zone.
        // zone_id   = the DLL's own zone
        // adjacent  = the host zone
        auto pt = std::make_shared<parent_transport>(params->name,
            params->dll_zone,
            params->host_zone,
            params->host_ctx,
            params->host_send,
            params->host_post,
            params->host_try_cast,
            params->host_add_ref,
            params->host_release,
            params->host_object_released,
            params->host_transport_down,
            params->host_get_new_zone_id);

        // create_child_zone validates interface IDs, builds the child_service,
        // links it to the parent_transport, and calls the user factory.
        auto create_result = rpc::child_service::create_child_zone<PARENT_INTERFACE, CHILD_INTERFACE>(
            params->name, pt, *params->input_descr, std::move(factory));

        if (create_result.error_code != rpc::error::OK())
            return create_result.error_code;

        auto* ctx = new dll_context{};
        ctx->transport = pt;
        ctx->service = std::dynamic_pointer_cast<rpc::child_service>(pt->get_service());

        params->dll_ctx = ctx;
        params->output_obj = create_result.descriptor;
        return rpc::error::OK();
    }

} // namespace rpc::dynamic_library

// ---------------------------------------------------------------------------
// canopy_dll_* entry point declarations
//
// Defined in dll_transport.cpp (transport_dynamic_library_dll library).
// The DLL author links that library and gets these for free; they only need
// to provide canopy_dll_init.
// ---------------------------------------------------------------------------
extern "C"
{
    CANOPY_DLL_EXPORT void canopy_dll_destroy(void* dll_ctx);

    CANOPY_DLL_EXPORT int canopy_dll_send(void* dll_ctx, rpc::send_params* params, rpc::send_result* result);

    CANOPY_DLL_EXPORT void canopy_dll_post(void* dll_ctx, rpc::post_params* params);

    CANOPY_DLL_EXPORT int canopy_dll_try_cast(void* dll_ctx, rpc::try_cast_params* params, rpc::standard_result* result);

    CANOPY_DLL_EXPORT int canopy_dll_add_ref(void* dll_ctx, rpc::add_ref_params* params, rpc::standard_result* result);

    CANOPY_DLL_EXPORT int canopy_dll_release(void* dll_ctx, rpc::release_params* params, rpc::standard_result* result);

    CANOPY_DLL_EXPORT void canopy_dll_object_released(void* dll_ctx, rpc::object_released_params* params);

    CANOPY_DLL_EXPORT void canopy_dll_transport_down(void* dll_ctx, rpc::transport_down_params* params);

    CANOPY_DLL_EXPORT int canopy_dll_get_new_zone_id(
        void* dll_ctx, rpc::get_new_zone_id_params* params, rpc::new_zone_id_result* result);
}

#endif // !CANOPY_BUILD_COROUTINE
