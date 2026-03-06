/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/internal/service.h>
#include <rpc/internal/service_proxy.h>

namespace rpc
{
    template<class T, template<class> class PtrType>
    CORO_TASK(int)
    proxy_bind_in_param(std::shared_ptr<rpc::object_proxy> object_p,
        uint64_t protocol_version,
        const PtrType<T>& iface,
        std::shared_ptr<rpc::object_stub>& stub,
        interface_descriptor& descriptor)
    {
        static_assert(__rpc_pointer_traits::is_supported_v<PtrType<T>>,
            "proxy_bind_in_param only supports rpc::shared_ptr and rpc::optimistic_ptr");

        if (iface.use_count() == 0)
        {
            descriptor = interface_descriptor();
            CO_RETURN error::OK();
        }

        RPC_ASSERT(object_p);
        if (!object_p)
            CO_RETURN error::INVALID_DATA();
        auto sp = object_p->get_service_proxy();
        auto operating_service = sp->get_operating_zone_service();

        // this is to check that an interface is belonging to another zone and not the operating zone
        if (!iface->__rpc_is_local() && casting_interface::get_destination_zone(*iface) != operating_service->get_zone_id())
        {
            descriptor = {casting_interface::get_object_id(*iface), casting_interface::get_destination_zone(*iface)};
            CO_RETURN error::OK();
        }

        // else encapsulate away
        CO_RETURN CO_AWAIT operating_service->bind_in_proxy(
            protocol_version, iface, stub, sp->get_destination_zone_id(), descriptor);
    }

    template<class T, template<class> class PtrType>
    CORO_TASK(int)
    stub_bind_out_param(const std::shared_ptr<rpc::service>& zone,
        uint64_t protocol_version,
        caller_zone caller_zone_id,
        const PtrType<T>& iface,
        interface_descriptor& descriptor)
    {
        RPC_ASSERT(caller_zone_id.is_set());
        static_assert(__rpc_pointer_traits::is_supported_v<PtrType<T>>,
            "stub_bind_out_param only supports rpc::shared_ptr and rpc::optimistic_ptr");

        if (iface.use_count() == 0)
        {
            descriptor = interface_descriptor();
            CO_RETURN rpc::error::OK();
        }

        if (iface->__rpc_is_local())
        {
            auto ret = CO_AWAIT zone->stub_add_ref(protocol_version, caller_zone_id, iface, descriptor);
            CO_RETURN ret;
        }
        else
        {
            auto ret = CO_AWAIT zone->remote_add_ref(protocol_version, caller_zone_id, iface, descriptor);
            CO_RETURN ret;
        }
    }

    // do not use directly it is for the interface generator use rpc::create_interface_proxy if you want to get a proxied pointer to a remote implementation
    template<class T, template<class> class PtrType>
    CORO_TASK(int)
    stub_bind_in_param(uint64_t protocol_version,
        const std::shared_ptr<rpc::service>& serv,
        caller_zone caller_zone_id,
        const rpc::interface_descriptor& encap,
        PtrType<T>& iface)
    {
        static_assert(__rpc_pointer_traits::is_supported_v<PtrType<T>>,
            "stub_bind_in_param only supports rpc::shared_ptr and rpc::optimistic_ptr");

        // if we have a null object id then return a null ptr
        if (encap == rpc::interface_descriptor())
        {
            CO_RETURN rpc::error::OK();
        }
        // if it is local to this service then just get the relevant stub
        else if (serv->get_zone_id() == encap.destination_zone_id.as_zone())
        {
            auto os = serv->get_object(encap.get_object_id()).lock();
            if (!os)
            {
                RPC_ERROR("Object not found in zone {}", serv->get_zone_id().get_subnet());
                CO_RETURN rpc::error::OBJECT_NOT_FOUND();
            }

            auto shared_iface = rpc::static_pointer_cast<T>(os->get_castable_interface(T::get_id(protocol_version)));
            if (!shared_iface)
            {
                RPC_ERROR("interface not implemented by this object");
                CO_RETURN rpc::error::INVALID_INTERFACE_ID();
            }

            if constexpr (__rpc_pointer_traits::is_optimistic_v<PtrType<T>>)
            {
                CO_RETURN CO_AWAIT rpc::make_optimistic(shared_iface, iface);
            }
            else
            {
                iface = shared_iface;
                CO_RETURN rpc::error::OK();
            }
        }
        else
        {
            // get the right  service proxy
            // if the zone is different lookup or clone the right proxy
            bool new_proxy_added = false;
            auto service_proxy
                = serv->get_zone_proxy(caller_zone_id, encap.destination_zone_id.as_zone(), new_proxy_added);
            if (!service_proxy)
            {
                RPC_ERROR("Object not found - service proxy is null");
                CO_RETURN rpc::error::OBJECT_NOT_FOUND();
            }

            std::shared_ptr<rpc::object_proxy> op;
            auto err = CO_AWAIT service_proxy->get_or_create_object_proxy(encap.get_object_id(),
                service_proxy::object_proxy_creation_rule::ADD_REF_IF_NEW,
                new_proxy_added,
                caller_zone_id,
                false,
                op);
            if (err != error::OK())
            {
                RPC_ERROR("get_or_create_object_proxy failed");
                CO_RETURN err;
            }
            RPC_ASSERT(op != nullptr);
            if (!op)
            {
                RPC_ERROR("Object not found - object proxy is null");
                CO_RETURN rpc::error::OBJECT_NOT_FOUND();
            }
            auto ret = CO_AWAIT op->query_interface(iface, false);
            CO_RETURN ret;
        }
    }

    // do not use directly it is for the interface generator use rpc::create_interface_proxy if you want to get a proxied pointer to a remote implementation
    template<class T, template<class> class PtrType>
    CORO_TASK(int)
    proxy_bind_out_param(
        const std::shared_ptr<rpc::service_proxy>& sp, const rpc::interface_descriptor& encap, PtrType<T>& val)
    {
        static_assert(__rpc_pointer_traits::is_supported_v<PtrType<T>>,
            "proxy_bind_out_param only supports rpc::shared_ptr and rpc::optimistic_ptr");

        // if we have a null object id then return a null ptr
        if (!encap.get_object_id().is_set() || !encap.destination_zone_id.is_set())
            CO_RETURN rpc::error::OK();

        auto serv = sp->get_operating_zone_service();

        // if it is local to this service then just get the relevant stub
        if (encap.destination_zone_id.as_zone() == serv->get_zone_id())
        {
            auto stub = serv->get_object(encap.get_object_id()).lock();
            if (!stub)
            {
                RPC_ERROR("Object not found - object is null in release");
                CO_RETURN rpc::error::OBJECT_NOT_FOUND();
            }

            auto count = CO_AWAIT serv->release_local_stub(
                stub, __rpc_pointer_traits::is_optimistic_v<PtrType<T>>, encap.destination_zone_id.as_zone());
            RPC_ASSERT(count);
            if (!count || count == std::numeric_limits<uint64_t>::max())
            {
                RPC_ERROR("Reference count error in release");
                CO_RETURN rpc::error::REFERENCE_COUNT_ERROR();
            }

            auto castable = stub->get_castable_interface(T::get_id(rpc::get_version()));
            if (!castable)
            {
                RPC_ERROR("Invalid interface ID in proxy release");
                CO_RETURN rpc::error::INVALID_INTERFACE_ID();
            }

            auto typed_iface = rpc::static_pointer_cast<T>(castable);
            if constexpr (__rpc_pointer_traits::is_optimistic_v<PtrType<T>>)
            {
                CO_RETURN CO_AWAIT rpc::make_optimistic(typed_iface, val);
            }
            else
            {
                val = typed_iface;
                CO_RETURN rpc::error::OK();
            }
        }

        // get the right  service proxy
        bool new_proxy_added = false;
        auto service_proxy = sp;

        if (sp->get_destination_zone_id() != encap.destination_zone_id)
        {
            // if the zone is different lookup or clone the right proxy
            // the service proxy is where the object came from so it should be used as the new caller channel for this returned object
            service_proxy
                = serv->get_zone_proxy(rpc::caller_zone(), {encap.destination_zone_id.as_zone()}, new_proxy_added);
            if (!service_proxy)
            {
                RPC_ERROR("Object not found - service proxy is null in proxy_bind_out_param");
                CO_RETURN rpc::error::ZONE_NOT_FOUND();
            }
        }

        std::shared_ptr<rpc::object_proxy> op;
        auto err = CO_AWAIT service_proxy->get_or_create_object_proxy(encap.get_object_id(),
            service_proxy::object_proxy_creation_rule::RELEASE_IF_NOT_NEW,
            new_proxy_added,
            {},
            __rpc_pointer_traits::is_optimistic_v<PtrType<T>>,
            op);
        if (err != error::OK())
        {
            RPC_ERROR("get_or_create_object_proxy failed");
            CO_RETURN err;
        }
        if (!op)
        {
            RPC_ERROR("Object not found in proxy_bind_out_param");
            CO_RETURN rpc::error::OBJECT_NOT_FOUND();
        }
        RPC_ASSERT(op != nullptr);
        CO_RETURN CO_AWAIT op->query_interface(val, false);
    }

    template<class T, template<class> class PtrType>
    CORO_TASK(int)
    demarshall_interface_proxy(uint64_t protocol_version,
        const std::shared_ptr<rpc::service_proxy>& sp,
        const rpc::interface_descriptor& encap,
        PtrType<T>& val)
    {
        static_assert(__rpc_pointer_traits::is_supported_v<PtrType<T>>,
            "demarshall_interface_proxy only supports rpc::shared_ptr and rpc::optimistic_ptr");

        if (protocol_version > rpc::get_version())
        {
            RPC_ERROR("Incompatible service in demarshall_interface_proxy");
            CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
        }

        // if we have a null object id then return a null ptr
        if (encap.get_object_id() == 0 || !encap.destination_zone_id.is_set())
            CO_RETURN rpc::error::OK();

        if (encap.destination_zone_id.as_zone() != sp->get_destination_zone_id())
        {
            CO_RETURN CO_AWAIT rpc::proxy_bind_out_param(sp, encap, val);
        }

        auto service_proxy = sp;
        auto serv = service_proxy->get_operating_zone_service();

        // if it is local to this service then just get the relevant stub
        if (serv->get_zone_id() == encap.destination_zone_id.as_zone())
        {
            // if we get here then we need to invent a test for this
            RPC_ASSERT(false);
            RPC_ERROR("Invalid data in demarshall_interface_proxy");
            CO_RETURN rpc::error::INVALID_DATA();
        }

        // get the right  service proxy
        // bool new_proxy_added = false;
        if (service_proxy->get_destination_zone_id() != encap.destination_zone_id.as_zone())
        {
            // if we get here then we need to invent a test for this
            RPC_ASSERT(false);
            RPC_ERROR("Invalid data in demarshall_interface_proxy");
            CO_RETURN rpc::error::INVALID_DATA();
        }

        // if (serv->get_parent_zone_id() == service_proxy->get_destination_zone_id())
        //     service_proxy->add_external_ref();

        std::shared_ptr<rpc::object_proxy> op;
        auto err = CO_AWAIT service_proxy->get_or_create_object_proxy(
            encap.get_object_id(), service_proxy::object_proxy_creation_rule::DO_NOTHING, false, {}, false, op);
        if (err != error::OK())
        {
            RPC_ERROR("get_or_create_object_proxy failed");
            CO_RETURN err;
        }
        if (!op)
        {
            RPC_ERROR("Object not found in demarshall_interface_proxy");
            CO_RETURN rpc::error::OBJECT_NOT_FOUND();
        }
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_proxy_add_ref(service_proxy->get_zone_id(),
                encap.destination_zone_id.with_object(encap.get_object_id()),
                service_proxy->get_zone_id(),
                rpc::requesting_zone(),
                rpc::add_ref_options::normal);
        }
#endif

        RPC_ASSERT(op != nullptr);
        CO_RETURN CO_AWAIT op->query_interface(val, false);
    }

    template<class T>
    CORO_TASK(int)
    create_interface_stub(
        rpc::service& serv, const rpc::shared_ptr<T>& iface, caller_zone caller_zone_id, rpc::interface_descriptor& descriptor)
    {
        // caller_zone caller_zone_id = serv.get_zone_id();

        if (!iface)
        {
            RPC_ASSERT(false);
            CO_RETURN error::INVALID_DATA();
        }
        std::shared_ptr<object_stub> stub;
        auto iface_cast = rpc::static_pointer_cast<rpc::casting_interface>(iface);
        CO_RETURN CO_AWAIT serv.get_descriptor_from_interface_stub(caller_zone_id, iface_cast, stub, descriptor, false);
    }
}
