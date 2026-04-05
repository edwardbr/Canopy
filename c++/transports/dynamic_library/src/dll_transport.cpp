/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// DLL-side (parent_transport) implementation, plus the canopy_dll_* entry
// points that the host resolves at load time.
//
// This file is compiled into transport_dynamic_library_dll, which is linked
// by the user's shared object.  The user provides canopy_dll_init themselves
// (using the init_child_zone<> helper).  All other entry points are here.

#include <transports/dynamic_library/dll_transport.h>
#include <rpc/rpc.h>

#ifndef CANOPY_BUILD_COROUTINE

namespace rpc::dynamic_library
{
    // -------------------------------------------------------------------------
    // parent_transport — lives inside the DLL, calls back to host
    // -------------------------------------------------------------------------
    parent_transport::parent_transport(
        std::string name,
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
        host_get_new_zone_id_fn get_new_zone_id)
        : rpc::transport(
              name,
              dll_zone)
        , host_ctx_(host_ctx)
        , host_send_(send)
        , host_post_(post)
        , host_try_cast_(try_cast)
        , host_add_ref_(add_ref)
        , host_release_(release)
        , host_object_released_(object_released)
        , host_transport_down_(transport_down)
        , host_get_new_zone_id_(get_new_zone_id)
    {
        set_adjacent_zone_id(host_zone);
        set_status(rpc::transport_status::CONNECTED);
    }

    void parent_transport::set_status(rpc::transport_status status)
    {
        rpc::transport::set_status(status);

        if (status == rpc::transport_status::DISCONNECTED)
        {
            // Notify the host service that this zone is going down.
            // notify_all_destinations_of_disconnect() broadcasts transport_down
            // to all registered destinations without requiring us to build the
            // zone-typed params manually.
            notify_all_destinations_of_disconnect();

            // Sever the host connection so no further callbacks fire
            host_ctx_ = nullptr;
        }
    }

    // -------------------------------------------------------------------------
    // Outbound methods — DLL sends to host
    // -------------------------------------------------------------------------
    CORO_TASK(send_result)
    parent_transport::outbound_send(send_params params)
    {
        if (!host_send_ || !host_ctx_)
            CO_RETURN send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};

        send_result result;
        host_send_(host_ctx_, &params, &result);
        CO_RETURN result;
    }

    CORO_TASK(void)
    parent_transport::outbound_post(post_params params)
    {
        if (!host_post_ || !host_ctx_)
            CO_RETURN;

        host_post_(host_ctx_, &params);
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_try_cast(try_cast_params params)
    {
        if (!host_try_cast_ || !host_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        standard_result result;
        host_try_cast_(host_ctx_, &params, &result);
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_add_ref(add_ref_params params)
    {
        if (!host_add_ref_ || !host_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        standard_result result;
        host_add_ref_(host_ctx_, &params, &result);
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    parent_transport::outbound_release(release_params params)
    {
        if (!host_release_ || !host_ctx_)
            CO_RETURN standard_result{rpc::error::ZONE_NOT_FOUND(), {}};

        standard_result result;
        host_release_(host_ctx_, &params, &result);
        CO_RETURN result;
    }

    CORO_TASK(void)
    parent_transport::outbound_object_released(object_released_params params)
    {
        if (!host_object_released_ || !host_ctx_)
            CO_RETURN;

        host_object_released_(host_ctx_, &params);
    }

    CORO_TASK(void)
    parent_transport::outbound_transport_down(transport_down_params params)
    {
        if (!host_transport_down_ || !host_ctx_)
            CO_RETURN;

        host_transport_down_(host_ctx_, &params);
    }

    CORO_TASK(new_zone_id_result)
    parent_transport::outbound_get_new_zone_id(get_new_zone_id_params params)
    {
        if (!host_get_new_zone_id_ || !host_ctx_)
            CO_RETURN new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};

        new_zone_id_result result;
        host_get_new_zone_id_(host_ctx_, &params, &result);
        CO_RETURN result;
    }

} // namespace rpc::dynamic_library

// -----------------------------------------------------------------------------
// canopy_dll_* entry points
//
// These are the symbols the host resolves with dlsym.  All are plain C linkage
// (no name mangling) and invisible from the OS symbol table except through the
// canopy_dll_init symbol which the host looks up explicitly.
//
// The CANOPY_DLL_EXPORT macro sets visibility("default") on Linux so that the
// host can actually find them with dlsym even when the DSO was compiled with
// -fvisibility=hidden (which is the recommended default for DLL payloads to
// keep all other symbols private).
// -----------------------------------------------------------------------------
extern "C"
{
    CANOPY_DLL_EXPORT void canopy_dll_destroy(void* dll_ctx)
    {
        if (!dll_ctx)
            return;

        auto* ctx = static_cast<rpc::dynamic_library::dll_context*>(dll_ctx);

        // Guard against double-destroy
        if (ctx->destroyed.exchange(true))
            return;

        // Tear down the child service and transport
        ctx->service.reset();
        if (ctx->transport)
            ctx->transport->set_status(rpc::transport_status::DISCONNECTED);
        ctx->transport.reset();

        delete ctx;
    }

    CANOPY_DLL_EXPORT int canopy_dll_send(
        void* dll_ctx,
        rpc::send_params* params,
        rpc::send_result* result)
    {
        auto* ctx = static_cast<rpc::dynamic_library::dll_context*>(dll_ctx);
        if (!ctx || !ctx->transport)
        {
            *result = rpc::send_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
            return result->error_code;
        }
        *result = ctx->transport->inbound_send(std::move(*params));
        return result->error_code;
    }

    CANOPY_DLL_EXPORT void canopy_dll_post(
        void* dll_ctx,
        rpc::post_params* params)
    {
        auto* ctx = static_cast<rpc::dynamic_library::dll_context*>(dll_ctx);
        if (!ctx || !ctx->transport)
            return;
        ctx->transport->inbound_post(std::move(*params));
    }

    CANOPY_DLL_EXPORT int canopy_dll_try_cast(
        void* dll_ctx,
        rpc::try_cast_params* params,
        rpc::standard_result* result)
    {
        auto* ctx = static_cast<rpc::dynamic_library::dll_context*>(dll_ctx);
        if (!ctx || !ctx->transport)
        {
            *result = rpc::standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
            return result->error_code;
        }
        *result = ctx->transport->inbound_try_cast(std::move(*params));
        return result->error_code;
    }

    CANOPY_DLL_EXPORT int canopy_dll_add_ref(
        void* dll_ctx,
        rpc::add_ref_params* params,
        rpc::standard_result* result)
    {
        auto* ctx = static_cast<rpc::dynamic_library::dll_context*>(dll_ctx);
        if (!ctx || !ctx->transport)
        {
            *result = rpc::standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
            return result->error_code;
        }
        *result = ctx->transport->inbound_add_ref(std::move(*params));
        return result->error_code;
    }

    CANOPY_DLL_EXPORT int canopy_dll_release(
        void* dll_ctx,
        rpc::release_params* params,
        rpc::standard_result* result)
    {
        auto* ctx = static_cast<rpc::dynamic_library::dll_context*>(dll_ctx);
        if (!ctx || !ctx->transport)
        {
            *result = rpc::standard_result{rpc::error::ZONE_NOT_FOUND(), {}};
            return result->error_code;
        }
        *result = ctx->transport->inbound_release(std::move(*params));
        return result->error_code;
    }

    CANOPY_DLL_EXPORT void canopy_dll_object_released(
        void* dll_ctx,
        rpc::object_released_params* params)
    {
        auto* ctx = static_cast<rpc::dynamic_library::dll_context*>(dll_ctx);
        if (!ctx || !ctx->transport)
            return;
        ctx->transport->inbound_object_released(std::move(*params));
    }

    CANOPY_DLL_EXPORT void canopy_dll_transport_down(
        void* dll_ctx,
        rpc::transport_down_params* params)
    {
        auto* ctx = static_cast<rpc::dynamic_library::dll_context*>(dll_ctx);
        if (!ctx || !ctx->transport)
            return;
        ctx->transport->inbound_transport_down(std::move(*params));
    }

    CANOPY_DLL_EXPORT int canopy_dll_get_new_zone_id(
        void* dll_ctx,
        rpc::get_new_zone_id_params* params,
        rpc::new_zone_id_result* result)
    {
        auto* ctx = static_cast<rpc::dynamic_library::dll_context*>(dll_ctx);
        if (!ctx || !ctx->transport)
        {
            *result = rpc::new_zone_id_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
            return result->error_code;
        }
        // Forward to the DLL's outbound_get_new_zone_id which calls back to host
        *result = ctx->transport->outbound_get_new_zone_id(std::move(*params));
        return result->error_code;
    }

} // extern "C"
#endif
