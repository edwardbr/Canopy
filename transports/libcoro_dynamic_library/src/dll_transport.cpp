/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// DLL-side (parent_transport) implementation.  Coroutine-build only.
//
// Compiled into transport_libcoro_dynamic_library_dll, which the DLL links.
// The DLL author only needs to provide canopy_libcoro_dll_init.

#ifdef CANOPY_BUILD_COROUTINE

#  include <transports/libcoro_dynamic_library/dll_transport.h>
#  include <rpc/rpc.h>

namespace rpc::libcoro_dynamic_library
{
    // -------------------------------------------------------------------------
    // parent_transport
    // -------------------------------------------------------------------------
    parent_transport::parent_transport(std::string name,
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
        host_coro_release_parent_fn host_coro_release_parent)
        : rpc::transport(name, dll_zone)
        , host_ctx_(host_ctx)
        , host_send_(send)
        , host_post_(post)
        , host_try_cast_(try_cast)
        , host_add_ref_(add_ref)
        , host_release_(release)
        , host_object_released_(object_released)
        , host_transport_down_(transport_down)
        , host_get_new_zone_id_(get_new_zone_id)
        , host_coro_release_parent_(host_coro_release_parent)
    {
        set_adjacent_zone_id(host_zone);
        set_status(rpc::transport_status::CONNECTED);
    }

    parent_transport::~parent_transport()
    {
        host_coro_release_parent_(host_ctx_);
    }

    // -------------------------------------------------------------------------
    // Outbound — DLL → Host (CO_AWAIT host coroutine function pointer)
    // -------------------------------------------------------------------------
    CORO_TASK(send_result)
    parent_transport::outbound_send(send_params params)
    {
        if (!host_send_ || !host_ctx_)
            CO_RETURN send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        CO_RETURN CO_AWAIT host_send_(host_ctx_, std::move(params));
    }

    CORO_TASK(void)
    parent_transport::outbound_post(post_params params)
    {
        if (!host_post_ || !host_ctx_)
            CO_RETURN;
        CO_AWAIT host_post_(host_ctx_, std::move(params));
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_try_cast(try_cast_params params)
    {
        if (!host_try_cast_ || !host_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        CO_RETURN CO_AWAIT host_try_cast_(host_ctx_, std::move(params));
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_add_ref(add_ref_params params)
    {
        if (!host_add_ref_ || !host_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        CO_RETURN CO_AWAIT host_add_ref_(host_ctx_, std::move(params));
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_release(release_params params)
    {
        if (!host_release_ || !host_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
        CO_RETURN CO_AWAIT host_release_(host_ctx_, std::move(params));
    }

    CORO_TASK(void)
    parent_transport::outbound_object_released(object_released_params params)
    {
        if (!host_object_released_ || !host_ctx_)
            CO_RETURN;
        CO_AWAIT host_object_released_(host_ctx_, std::move(params));
    }

    CORO_TASK(void)
    parent_transport::outbound_transport_down(transport_down_params params)
    {
        if (!host_transport_down_ || !host_ctx_)
            CO_RETURN;
        CO_AWAIT host_transport_down_(host_ctx_, std::move(params));
    }

    CORO_TASK(new_zone_id_result)
    parent_transport::outbound_get_new_zone_id(get_new_zone_id_params params)
    {
        if (!host_get_new_zone_id_ || !host_ctx_)
            CO_RETURN new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        CO_RETURN CO_AWAIT host_get_new_zone_id_(host_ctx_, std::move(params));
    }

} // namespace rpc::libcoro_dynamic_library

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
    result->init_fn = &canopy_libcoro_dll_init;
    result->send_fn = &parent_transport::static_inbound_send;
    result->post_fn = &parent_transport::static_inbound_post;
    result->try_cast_fn = &parent_transport::static_inbound_try_cast;
    result->add_ref_fn = &parent_transport::static_inbound_add_ref;
    result->release_fn = &parent_transport::static_inbound_release;
    result->object_released_fn = &parent_transport::static_inbound_object_released;
    result->transport_down_fn = &parent_transport::static_inbound_transport_down;
}

#endif // CANOPY_BUILD_COROUTINE
