/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/internal/error_codes.h>
#include <rpc/internal/types.h>
#include <rpc/internal/version.h>
#include <rpc/internal/remote_pointer.h>
#include <rpc/internal/coroutine_support.h>

namespace rpc
{
    class object_stub;
    class service;
    class service_proxy;

    template<class Ptr> struct interface_bind_result
    {
        int error_code;
        Ptr iface;

        interface_bind_result() = default;
        interface_bind_result(
            int error_code,
            Ptr iface)
            : error_code(error_code)
            , iface(std::move(iface))
        {
        }
    };

    struct remote_object_bind_result
    {
        int error_code;
        std::shared_ptr<rpc::object_stub> stub;
        rpc::remote_object descriptor;

        remote_object_bind_result() = default;
        remote_object_bind_result(
            int error_code,
            std::shared_ptr<rpc::object_stub> stub,
            rpc::remote_object descriptor)
            : error_code(error_code)
            , stub(std::move(stub))
            , descriptor(descriptor)
        {
        }
    };

    template<
        class T,
        template<class> class PtrType>
    CORO_TASK(interface_bind_result<PtrType<T>>)
    demarshall_interface_proxy(
        uint64_t protocol_version,
        std::shared_ptr<rpc::service_proxy> sp,
        rpc::remote_object encap);

    template<class T>
    CORO_TASK(remote_object_bind_result)
    create_interface_stub(
        service* serv,
        rpc::shared_ptr<T> iface,
        caller_zone caller_zone_id);

    template<
        class T,
        template<class> class PtrType>
    CORO_TASK(remote_object_bind_result)
    stub_bind_out_param(
        std::shared_ptr<rpc::service> zone,
        uint64_t protocol_version,
        caller_zone caller_zone_id,
        PtrType<T> iface);

    template<
        class T,
        template<class> class PtrType>
    CORO_TASK(remote_object_bind_result)
    proxy_bind_in_param(
        std::shared_ptr<rpc::object_proxy> object_p,
        uint64_t protocol_version,
        PtrType<T> iface);

    // do not use directly it is for the interface generator use rpc::create_interface_proxy if you want to get a proxied pointer to a remote implementation
    template<
        class T,
        template<class> class PtrType>
    CORO_TASK(interface_bind_result<PtrType<T>>)
    stub_bind_in_param(
        uint64_t protocol_version,
        std::shared_ptr<rpc::service> serv,
        caller_zone caller_zone_id,
        rpc::remote_object encap);

    // do not use directly it is for the interface generator use rpc::create_interface_proxy if you want to get a proxied pointer to a remote implementation
    template<
        class T,
        template<class> class PtrType>
    CORO_TASK(interface_bind_result<PtrType<T>>)
    proxy_bind_out_param(
        std::shared_ptr<rpc::service_proxy> sp,
        rpc::remote_object encap);
}
