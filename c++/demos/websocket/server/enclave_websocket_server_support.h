// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <exception>
#include <memory>
#include <new>
#include <utility>

#include <rpc/rpc.h>

namespace websocket_demo::v1::enclave_support
{
    // Enclave demo code deliberately terminates on allocation failure.
    // Recovering from bad_alloc inside the enclave would need a larger policy:
    // cancelling accepted clients, draining scheduler work, and reporting health
    // to the host. For this reference server, logging and terminating is clearer
    // than pretending the listener can continue safely.
    inline void log_bad_alloc(const char* component_name)
    {
        RPC_ERROR("bad_alloc while creating {}", component_name);
    }

    template<
        typename Type,
        typename... Args>
    auto make_std_shared_or_terminate(
        const char* component_name,
        Args&&... args) -> std::shared_ptr<Type>
    {
        try
        {
            return std::make_shared<Type>(std::forward<Args>(args)...);
        }
        catch (const std::bad_alloc&)
        {
            log_bad_alloc(component_name);
            std::terminate();
        }
        return {};
    }

    template<
        typename Interface,
        typename... Args>
    auto make_rpc_shared_or_terminate(
        const char* component_name,
        Args&&... args) -> rpc::shared_ptr<Interface>
    {
        try
        {
            return rpc::make_shared<Interface>(std::forward<Args>(args)...);
        }
        catch (const std::bad_alloc&)
        {
            log_bad_alloc(component_name);
            std::terminate();
        }
        return {};
    }
} // namespace websocket_demo::v1::enclave_support
