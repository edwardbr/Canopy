/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <memory>

#include <rpc/rpc.h>

namespace rpc::sgx::coro::enclave
{
    class local_route_transport
    {
    public:
        virtual ~local_route_transport() = default;
    };

    [[nodiscard]] inline bool is_local_route_transport(const std::shared_ptr<rpc::transport>& transport)
    {
        return static_cast<bool>(std::dynamic_pointer_cast<local_route_transport>(transport));
    }
}
