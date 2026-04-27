/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/internal/service.h>
#include <rpc/internal/service_proxy.h>

namespace rpc
{
    template<class PtrType> [[nodiscard]] bool is_bound_pointer_null(const PtrType& iface)
    {
        if constexpr (__rpc_pointer_traits::is_optimistic_v<PtrType>)
        {
            return iface.is_null();
        }
        else
        {
            return !iface;
        }
    }

    template<class PtrType> [[nodiscard]] bool is_bound_pointer_gone(const PtrType& iface)
    {
        if constexpr (__rpc_pointer_traits::is_optimistic_v<PtrType>)
        {
            return !iface && !iface.is_null();
        }
        else
        {
            return false;
        }
    }

    template<
        class T,
        template<class> class PtrType>
    CORO_TASK(remote_object_bind_result)
    proxy_bind_in_param(
        std::shared_ptr<rpc::object_proxy> object_p,
        uint64_t protocol_version,
        PtrType<T> iface)
    {
        static_assert(
            __rpc_pointer_traits::is_supported_v<PtrType<T>>,
            "proxy_bind_in_param only supports rpc::shared_ptr and rpc::optimistic_ptr");

        remote_object_bind_result result{error::OK(), nullptr, {}};

        if (is_bound_pointer_gone(iface))
        {
            result.error_code = error::OBJECT_GONE();
            CO_RETURN result;
        }

        if (is_bound_pointer_null(iface))
        {
            CO_RETURN result;
        }

        RPC_ASSERT(object_p);
        if (!object_p)
        {
            result.error_code = error::INVALID_DATA();
            CO_RETURN result;
        }
        auto sp = object_p->get_service_proxy();
        auto operating_service = sp->get_operating_zone_service();

        // this is to check that an interface is belonging to another zone and not the operating zone
        if (!iface->__rpc_is_local() && casting_interface::get_destination_zone(*iface) != operating_service->get_zone_id())
        {
            result.descriptor = casting_interface::get_remote_object(*iface);
            CO_RETURN result;
        }

        // else encapsulate away
        CO_RETURN CO_AWAIT operating_service->bind_in_proxy(protocol_version, iface, sp->get_destination_zone_id());
    }

    template<
        class T,
        template<class> class PtrType>
    CORO_TASK(remote_object_bind_result)
    stub_bind_out_param(
        std::shared_ptr<rpc::service> zone,
        uint64_t protocol_version,
        caller_zone caller_zone_id,
        uint64_t request_id,
        PtrType<T> iface)
    {
        RPC_ASSERT(caller_zone_id.is_set());
        static_assert(
            __rpc_pointer_traits::is_supported_v<PtrType<T>>,
            "stub_bind_out_param only supports rpc::shared_ptr and rpc::optimistic_ptr");

        remote_object_bind_result result{rpc::error::OK(), nullptr, {}};

        if (is_bound_pointer_gone(iface))
        {
            result.error_code = error::OBJECT_GONE();
            CO_RETURN result;
        }

        if (is_bound_pointer_null(iface))
        {
            CO_RETURN result;
        }

        if (iface->__rpc_is_local())
        {
            auto add_ref_result = CO_AWAIT zone->stub_add_ref(protocol_version, caller_zone_id, request_id, iface);
            result.error_code = add_ref_result.error_code;
            result.descriptor = std::move(add_ref_result.descriptor);
            CO_RETURN result;
        }
        else
        {
            auto add_ref_result = CO_AWAIT zone->remote_add_ref(protocol_version, caller_zone_id, request_id, iface);
            result.error_code = add_ref_result.error_code;
            result.descriptor = std::move(add_ref_result.descriptor);
            CO_RETURN result;
        }
    }

    // do not use directly it is for the interface generator use rpc::create_interface_proxy if you want to get a proxied pointer to a remote implementation
    template<
        class T,
        template<class> class PtrType>
    CORO_TASK(interface_bind_result<PtrType<T>>)
    stub_bind_in_param(
        uint64_t protocol_version,
        std::shared_ptr<rpc::service> serv,
        caller_zone caller_zone_id,
        rpc::remote_object encap)
    {
        static_assert(
            __rpc_pointer_traits::is_supported_v<PtrType<T>>,
            "stub_bind_in_param only supports rpc::shared_ptr and rpc::optimistic_ptr");
        interface_bind_result<PtrType<T>> result{rpc::error::OK(), {}};

        // if we have a null object id then return a null ptr
        if (encap == rpc::remote_object())
        {
            CO_RETURN result;
        }
        // if it is local to this service then just get the relevant stub
        else if (serv->get_zone_id() == encap.as_zone())
        {
            auto os = serv->get_object(encap.get_object_id()).lock();
            if (!os)
            {
                RPC_ERROR("Object not found in zone {}", serv->get_zone_id().get_subnet());
                result.error_code = rpc::error::OBJECT_NOT_FOUND();
                CO_RETURN result;
            }

            auto shared_iface = rpc::static_pointer_cast<T>(os->get_castable_interface(T::get_id(protocol_version)));
            if (!shared_iface)
            {
                RPC_ERROR("interface not implemented by this object");
                result.error_code = rpc::error::INVALID_INTERFACE_ID();
                CO_RETURN result;
            }

            if constexpr (__rpc_pointer_traits::is_optimistic_v<PtrType<T>>)
            {
                auto optimistic_result = CO_AWAIT rpc::make_optimistic(shared_iface);
                result.error_code = std::get<0>(optimistic_result);
                result.iface = std::get<1>(std::move(optimistic_result));
                CO_RETURN result;
            }
            else
            {
                result.iface = shared_iface;
                CO_RETURN result;
            }
        }
        else
        {
            // get the right  service proxy
            // if the zone is different lookup or clone the right proxy
            bool new_proxy_added = false;
            auto service_proxy = serv->get_zone_proxy(caller_zone_id, encap.as_zone(), new_proxy_added);
            if (!service_proxy)
            {
                RPC_ERROR("Object not found - service proxy is null");
                result.error_code = rpc::error::OBJECT_NOT_FOUND();
                CO_RETURN result;
            }

            auto proxy_result = CO_AWAIT service_proxy->get_or_create_object_proxy(
                encap.get_object_id(),
                service_proxy::object_proxy_creation_rule::ADD_REF_IF_NEW,
                new_proxy_added,
                caller_zone_id,
                false);
            if (proxy_result.error_code != error::OK())
            {
                RPC_ERROR("get_or_create_object_proxy failed");
                result.error_code = proxy_result.error_code;
                CO_RETURN result;
            }
            auto op = std::move(proxy_result.proxy);
            RPC_ASSERT(op != nullptr);
            if (!op)
            {
                RPC_ERROR("Object not found - object proxy is null");
                result.error_code = rpc::error::OBJECT_NOT_FOUND();
                CO_RETURN result;
            }
            auto query_result = CO_AWAIT op->template query_interface<T, PtrType>(false);
            result.error_code = query_result.error_code;
            result.iface = std::move(query_result.iface);
            CO_RETURN result;
        }
    }

    // do not use directly it is for the interface generator use rpc::create_interface_proxy if you want to get a proxied pointer to a remote implementation
    template<
        class T,
        template<class> class PtrType>
    CORO_TASK(interface_bind_result<PtrType<T>>)
    proxy_bind_out_param(
        std::shared_ptr<rpc::service_proxy> sp,
        uint64_t request_id,
        rpc::remote_object encap)
    {
        static_assert(
            __rpc_pointer_traits::is_supported_v<PtrType<T>>,
            "proxy_bind_out_param only supports rpc::shared_ptr and rpc::optimistic_ptr");
        interface_bind_result<PtrType<T>> result{rpc::error::OK(), {}};

        // if we have a null object id then return a null ptr
        if (!encap.get_object_id().is_set() || !encap.is_set())
            CO_RETURN result;

        auto serv = sp->get_operating_zone_service();

        // if it is local to this service then just get the relevant stub
        if (encap.as_zone() == serv->get_zone_id())
        {
            auto stub = serv->get_object(encap.get_object_id()).lock();
            if (!stub)
            {
                RPC_ERROR("Object not found - object is null in release");
                result.error_code = rpc::error::OBJECT_NOT_FOUND();
                CO_RETURN result;
            }

            auto count = CO_AWAIT serv->release_local_stub(
                stub, __rpc_pointer_traits::is_optimistic_v<PtrType<T>>, encap.as_zone());
            RPC_ASSERT(count);
            if (!count || count == std::numeric_limits<uint64_t>::max())
            {
                RPC_ERROR("Reference count error in release");
                result.error_code = rpc::error::REFERENCE_COUNT_ERROR();
                CO_RETURN result;
            }

            auto castable = stub->get_castable_interface(T::get_id(rpc::get_version()));
            if (!castable)
            {
                RPC_ERROR("Invalid interface ID in proxy release");
                result.error_code = rpc::error::INVALID_INTERFACE_ID();
                CO_RETURN result;
            }

            auto typed_iface = rpc::static_pointer_cast<T>(castable);
            if constexpr (__rpc_pointer_traits::is_optimistic_v<PtrType<T>>)
            {
                auto optimistic_result = CO_AWAIT rpc::make_optimistic(typed_iface);
                result.error_code = std::get<0>(optimistic_result);
                result.iface = std::get<1>(std::move(optimistic_result));
                CO_RETURN result;
            }
            else
            {
                result.iface = typed_iface;
                CO_RETURN result;
            }
        }

        if (request_id != 0)
        {
            int pending_error = rpc::error::OK();
            if constexpr (__rpc_pointer_traits::is_optimistic_v<PtrType<T>>)
            {
                auto pending_iface = serv->find_pending_out_param_optimistic(request_id, encap, pending_error);
                RPC_DEBUG(
                    "proxy_bind_out_param pending optimistic request_id={} remote_object={} pending_error={} "
                    "has_pending={}",
                    request_id,
                    std::to_string(encap),
                    pending_error,
                    static_cast<bool>(pending_iface));
                if (pending_error != rpc::error::OK())
                {
                    result.error_code = pending_error;
                    CO_RETURN result;
                }
                if (pending_iface)
                {
                    auto ob = pending_iface->__rpc_get_object_proxy();
                    auto res = CO_AWAIT ob->template query_interface<T, rpc::optimistic_ptr>(false);
                    if (error::is_error(res.error_code))
                    {
                        result.error_code = res.error_code;
                        CO_RETURN result;
                    }
                    result.iface = res.iface;
                    RPC_DEBUG(
                        "proxy_bind_out_param pending optimistic request_id={} remote_object={} cast_result={}",
                        request_id,
                        std::to_string(encap),
                        static_cast<bool>(result.iface));
                    result.error_code = result.iface ? rpc::error::OK() : rpc::error::INVALID_INTERFACE_ID();
                    CO_RETURN result;
                }
            }
            else
            {
                auto pending_iface = serv->find_pending_out_param_shared(request_id, encap, pending_error);
                RPC_DEBUG(
                    "proxy_bind_out_param pending shared request_id={} remote_object={} pending_error={} "
                    "has_pending={}",
                    request_id,
                    std::to_string(encap),
                    pending_error,
                    static_cast<bool>(pending_iface));
                if (pending_error != rpc::error::OK())
                {
                    result.error_code = pending_error;
                    CO_RETURN result;
                }
                if (pending_iface)
                {
                    auto ob = pending_iface->__rpc_get_object_proxy();
                    auto res = CO_AWAIT ob->template query_interface<T>(false);
                    if (error::is_error(res.error_code))
                    {
                        result.error_code = res.error_code;
                        CO_RETURN result;
                    }
                    result.iface = res.iface;
                    RPC_DEBUG(
                        "proxy_bind_out_param pending shared request_id={} remote_object={} cast_result={}",
                        request_id,
                        std::to_string(encap),
                        static_cast<bool>(result.iface));
                    result.error_code = result.iface ? rpc::error::OK() : rpc::error::INVALID_INTERFACE_ID();
                    CO_RETURN result;
                }
            }
        }

        // get the right  service proxy
        bool new_proxy_added = false;
        auto service_proxy = sp;

        if (sp->get_destination_zone_id() != encap.as_zone())
        {
            // If the returned object belongs to a different zone than the current
            // service proxy, use the callee zone as the caller context. That lets
            // get_zone_proxy() recreate a route via the current call path when a
            // temporary direct transport entry has already been cleaned up.
            // the service proxy is where the object came from so it should be used as the new caller channel for this returned object
            service_proxy = serv->get_zone_proxy(sp->get_destination_zone_id(), {encap.as_zone()}, new_proxy_added);
            if (!service_proxy)
            {
                RPC_ERROR("Object not found - service proxy is null in proxy_bind_out_param");
                result.error_code = rpc::error::ZONE_NOT_FOUND();
                CO_RETURN result;
            }
        }

        auto proxy_result = CO_AWAIT service_proxy->get_or_create_object_proxy(
            encap.get_object_id(),
            service_proxy::object_proxy_creation_rule::RELEASE_IF_NOT_NEW,
            new_proxy_added,
            {},
            __rpc_pointer_traits::is_optimistic_v<PtrType<T>>);
        if (proxy_result.error_code != error::OK())
        {
            RPC_ERROR("get_or_create_object_proxy failed");
            result.error_code = proxy_result.error_code;
            CO_RETURN result;
        }
        auto op = std::move(proxy_result.proxy);
        if (!op)
        {
            RPC_ERROR("Object not found in proxy_bind_out_param");
            result.error_code = rpc::error::OBJECT_NOT_FOUND();
            CO_RETURN result;
        }
        RPC_ASSERT(op != nullptr);
        auto query_result = CO_AWAIT op->template query_interface<T, PtrType>(false);
        result.error_code = query_result.error_code;
        result.iface = std::move(query_result.iface);
        CO_RETURN result;
    }

    template<
        class T,
        template<class> class PtrType>
    CORO_TASK(interface_bind_result<PtrType<T>>)
    demarshall_interface_proxy(
        uint64_t protocol_version,
        std::shared_ptr<rpc::service_proxy> sp,
        rpc::remote_object encap)
    {
        static_assert(
            __rpc_pointer_traits::is_supported_v<PtrType<T>>,
            "demarshall_interface_proxy only supports rpc::shared_ptr and rpc::optimistic_ptr");
        interface_bind_result<PtrType<T>> result{rpc::error::OK(), {}};

        if (protocol_version > rpc::get_version())
        {
            RPC_ERROR("Incompatible service in demarshall_interface_proxy");
            result.error_code = rpc::error::INCOMPATIBLE_SERVICE();
            CO_RETURN result;
        }

        // if we have a null object id then return a null ptr
        if (encap.get_object_id() == 0 || !encap.is_set())
            CO_RETURN result;

        if (encap.as_zone() != sp->get_destination_zone_id())
        {
            CO_RETURN CO_AWAIT rpc::proxy_bind_out_param<T, PtrType>(sp, 0, encap);
        }

        const auto& service_proxy = sp;
        auto serv = service_proxy->get_operating_zone_service();

        // if it is local to this service then just get the relevant stub
        if (serv->get_zone_id() == encap.as_zone())
        {
            // if we get here then we need to invent a test for this
            RPC_ASSERT(false);
            RPC_ERROR("Invalid data in demarshall_interface_proxy");
            result.error_code = rpc::error::INVALID_DATA();
            CO_RETURN result;
        }

        // get the right  service proxy
        // bool new_proxy_added = false;
        if (service_proxy->get_destination_zone_id() != encap.as_zone())
        {
            // if we get here then we need to invent a test for this
            RPC_ASSERT(false);
            RPC_ERROR("Invalid data in demarshall_interface_proxy");
            result.error_code = rpc::error::INVALID_DATA();
            CO_RETURN result;
        }

        // if (serv->get_parent_zone_id() == service_proxy->get_destination_zone_id())
        //     service_proxy->add_external_ref();

        auto proxy_result = CO_AWAIT service_proxy->get_or_create_object_proxy(
            encap.get_object_id(), service_proxy::object_proxy_creation_rule::DO_NOTHING, false, {}, false);
        if (proxy_result.error_code != error::OK())
        {
            RPC_ERROR("get_or_create_object_proxy failed");
            result.error_code = proxy_result.error_code;
            CO_RETURN result;
        }
        auto op = std::move(proxy_result.proxy);
        if (!op)
        {
            RPC_ERROR("Object not found in demarshall_interface_proxy");
            result.error_code = rpc::error::OBJECT_NOT_FOUND();
            CO_RETURN result;
        }
#if defined(CANOPY_USE_TELEMETRY) && !defined(FOR_SGX)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            auto encap_remote_r = encap.with_object(encap.get_object_id());
            RPC_ASSERT(encap_remote_r.has_value());
            telemetry_service->on_service_proxy_add_ref(
                service_proxy->get_zone_id(),
                *encap_remote_r,
                service_proxy->get_zone_id(),
                rpc::requesting_zone(),
                rpc::add_ref_options::normal);
        }
#endif

        RPC_ASSERT(op != nullptr);
        CO_RETURN CO_AWAIT op->template query_interface<T, PtrType>(false);
    }

    template<class T>
    CORO_TASK(remote_object_bind_result)
    create_interface_stub(
        rpc::service* serv,
        rpc::shared_ptr<T> iface,
        caller_zone caller_zone_id)
    {
        // caller_zone caller_zone_id = serv.get_zone_id();
        remote_object_bind_result result{error::OK(), nullptr, {}};

        if (!iface)
        {
            RPC_ASSERT(false);
            result.error_code = error::INVALID_DATA();
            CO_RETURN result;
        }
        auto iface_cast = rpc::static_pointer_cast<rpc::casting_interface>(iface);
        CO_RETURN CO_AWAIT serv->get_descriptor_from_interface_stub(caller_zone_id, std::move(iface_cast), false);
    }
}
