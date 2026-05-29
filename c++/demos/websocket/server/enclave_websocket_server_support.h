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
    inline void log_bad_alloc(const char* component_name)
    {
        RPC_ERROR("bad_alloc while creating {}", component_name);
    }

    template<typename Type> struct shared_create_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<Type> value;
    };

    template<
        typename Type,
        typename... Args>
    auto make_std_shared_result(
        const char* component_name,
        Args&&... args) -> shared_create_result<Type>
    {
        try
        {
            auto value = std::make_shared<Type>(std::forward<Args>(args)...);
            if (!value)
                return {rpc::error::OUT_OF_MEMORY(), {}};
            return {rpc::error::OK(), std::move(value)};
        }
        catch (const std::bad_alloc&)
        {
            log_bad_alloc(component_name);
            return {rpc::error::OUT_OF_MEMORY(), {}};
        }
        catch (const std::exception& error)
        {
            RPC_ERROR("exception while creating {}: {}", component_name, error.what());
            return {rpc::error::EXCEPTION(), {}};
        }
        catch (...)
        {
            RPC_ERROR("unknown exception while creating {}", component_name);
            return {rpc::error::EXCEPTION(), {}};
        }
    }
} // namespace websocket_demo::v1::enclave_support
