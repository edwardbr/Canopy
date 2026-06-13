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
#include <transports/sgx_coroutine/enclave/service.h>

namespace rpc::sgx_coroutine_transport::enclave
{
    class local_parent_transport;

    class local_child_transport final : public rpc::local::child_transport, public local_route_transport
    {
    protected:
        [[nodiscard]] std::shared_ptr<rpc::local::parent_transport> make_child_parent_transport(
            std::string name,
            std::shared_ptr<rpc::local::child_transport> parent) override;

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

        [[nodiscard]] std::shared_ptr<rpc::child_service> make_child_service(
            const char* name,
            rpc::zone zone_id,
            rpc::destination_zone parent_zone_id,
            const rpc::executor_ptr& executor) override;
    };

    inline std::shared_ptr<rpc::local::parent_transport> local_child_transport::make_child_parent_transport(
        std::string name,
        std::shared_ptr<rpc::local::child_transport> parent)
    {
        auto enclave_parent = std::dynamic_pointer_cast<local_child_transport>(std::move(parent));
        RPC_ASSERT(enclave_parent != nullptr);
        if (!enclave_parent)
            return nullptr;
        return std::make_shared<local_parent_transport>(std::move(name), std::move(enclave_parent));
    }

    inline std::shared_ptr<rpc::child_service> local_parent_transport::make_child_service(
        const char* name,
        rpc::zone zone_id,
        rpc::destination_zone parent_zone_id,
        const rpc::executor_ptr& executor)
    {
        return std::make_shared<rpc::enclave_service>(name, zone_id, parent_zone_id, executor);
    }
}
