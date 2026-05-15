/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/enclave/service.h>

#include <security/attestation/context_source.h>
#include <streaming/stream.h>

#include <utility>

namespace rpc
{
    enclave_service::~enclave_service()
    {
        if (auto controller = get_io_uring_controller())
            controller->request_shutdown();
    }

    void enclave_service::set_security_context(
        rpc::destination_zone adjacent_zone_id,
        canopy::security::attestation::security_context context)
    {
        RPC_ASSERT(adjacent_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        security_contexts_[adjacent_zone_id] = std::move(context);
    }

    bool enclave_service::publish_security_context_from_stream(
        rpc::destination_zone adjacent_zone_id,
        const std::shared_ptr<streaming::stream>& stream)
    {
        if (!adjacent_zone_id.is_set() || !stream)
            return false;

        auto source = std::dynamic_pointer_cast<canopy::security::attestation::security_context_source>(stream);
        if (!source)
            return false;

        auto context = source->security_context();
        if (!context.established)
            return false;

        set_security_context(adjacent_zone_id, std::move(context));
        return true;
    }

    void enclave_service::remove_security_context(rpc::destination_zone adjacent_zone_id)
    {
        RPC_ASSERT(adjacent_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        security_contexts_.erase(adjacent_zone_id);
    }

    auto enclave_service::get_security_context(rpc::destination_zone adjacent_zone_id) const
        -> std::optional<canopy::security::attestation::security_context>
    {
        RPC_ASSERT(adjacent_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = security_contexts_.find(adjacent_zone_id);
        if (item == security_contexts_.end())
            return std::nullopt;
        return item->second;
    }
}
