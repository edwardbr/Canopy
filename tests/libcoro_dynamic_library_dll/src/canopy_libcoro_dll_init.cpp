/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Test DLL entry point for the libcoro_dynamic_library transport tests.

#ifdef CANOPY_BUILD_COROUTINE

#include <transports/libcoro_dynamic_library/dll_transport.h>
#include <rpc/rpc.h>
#include <common/tests.h>

namespace
{
    // Concrete init coroutine: signature matches dll_coro_init_fn.
    // Called by the host's inner_connect after canopy_libcoro_dll_create.
    coro::task<rpc::connect_result> do_init(
        void* transport_ctx, const rpc::connection_settings* settings, std::shared_ptr<coro::scheduler>* scheduler)
    {
        return rpc::libcoro_dynamic_library::init_child_zone_libcoro<yyy::i_host, yyy::i_example>(transport_ctx,
            settings,
            scheduler,
            [](rpc::shared_ptr<yyy::i_host> host,
                std::shared_ptr<rpc::child_service> svc) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
            {
                CO_RETURN rpc::service_connect_result<yyy::i_example>{
                    rpc::error::OK(), rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(svc, host))};
            });
    }
}

extern "C" CANOPY_LIBCORO_DLL_EXPORT void canopy_libcoro_dll_create(
    rpc::libcoro_dynamic_library::dll_create_params* params, rpc::libcoro_dynamic_library::dll_create_result* result)
{
    using namespace rpc::libcoro_dynamic_library;

    auto* pt = new parent_transport(params->name,
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
        params->host_get_new_zone_id,
        params->host_coro_release_parent);

    result->transport_ctx = pt;
    result->init_fn = &do_init;
    result->send_fn = &parent_transport::static_inbound_send;
    result->post_fn = &parent_transport::static_inbound_post;
    result->try_cast_fn = &parent_transport::static_inbound_try_cast;
    result->add_ref_fn = &parent_transport::static_inbound_add_ref;
    result->release_fn = &parent_transport::static_inbound_release;
    result->object_released_fn = &parent_transport::static_inbound_object_released;
    result->transport_down_fn = &parent_transport::static_inbound_transport_down;
}

#endif // CANOPY_BUILD_COROUTINE
