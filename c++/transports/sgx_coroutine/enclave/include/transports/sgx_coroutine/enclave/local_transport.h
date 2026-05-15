/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <memory>
#include <string>
#include <utility>

#include <transports/local/transport.h>
#include <transports/sgx_coroutine/enclave/local_route_transport.h>

namespace rpc::sgx::coro::enclave
{
    class local_child_transport final : public rpc::local::child_transport, public local_route_transport
    {
    public:
        local_child_transport(
            std::string name,
            std::shared_ptr<rpc::service> service)
            : rpc::local::child_transport(
                  std::move(name),
                  std::move(service))
        {
        }
    };

    class local_parent_transport final : public rpc::local::parent_transport, public local_route_transport
    {
    public:
        local_parent_transport(
            std::string name,
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<local_child_transport> parent)
            : rpc::local::parent_transport(
                  std::move(name),
                  std::move(service),
                  std::move(parent))
        {
        }

        local_parent_transport(
            std::string name,
            std::shared_ptr<local_child_transport> parent)
            : rpc::local::parent_transport(
                  std::move(name),
                  std::move(parent))
        {
        }
    };
}
