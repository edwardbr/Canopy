/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/enclave/service.h>

#include <security/attestation/context_source.h>
#include <security/attestation/protected_rpc.h>
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
        rpc::destination_zone attested_zone_id,
        canopy::security::attestation::security_context context)
    {
        RPC_ASSERT(attested_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        security_contexts_[attested_zone_id] = std::move(context);
        canopy::security::attestation::route_attestation_state state;
        state.status = canopy::security::attestation::route_attestation_status::attested;
        state.context = security_contexts_[attested_zone_id];
        attestation_route_states_[attested_zone_id] = std::move(state);
    }

    bool enclave_service::publish_security_context_from_stream(
        rpc::destination_zone attested_zone_id,
        const std::shared_ptr<streaming::stream>& stream)
    {
        if (!attested_zone_id.is_set() || !stream)
            return false;

        auto source = std::dynamic_pointer_cast<canopy::security::attestation::security_context_source>(stream);
        if (!source)
            return false;

        auto context = source->security_context();
        if (!context.established)
            return false;

        set_security_context(attested_zone_id, std::move(context));
        return true;
    }

    void enclave_service::remove_security_context(rpc::destination_zone attested_zone_id)
    {
        RPC_ASSERT(attested_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        security_contexts_.erase(attested_zone_id);
        attestation_route_states_.erase(attested_zone_id);
    }

    auto enclave_service::get_security_context(rpc::destination_zone attested_zone_id) const
        -> std::optional<canopy::security::attestation::security_context>
    {
        RPC_ASSERT(attested_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = security_contexts_.find(attested_zone_id);
        if (item == security_contexts_.end())
            return std::nullopt;
        return item->second;
    }

    void enclave_service::set_attestation_route_state(
        rpc::destination_zone attested_zone_id,
        canopy::security::attestation::route_attestation_state state)
    {
        RPC_ASSERT(attested_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        if (state.status == canopy::security::attestation::route_attestation_status::attested && state.context.established)
            security_contexts_[attested_zone_id] = state.context;
        attestation_route_states_[attested_zone_id] = std::move(state);
    }

    auto enclave_service::get_attestation_route_state(rpc::destination_zone attested_zone_id) const
        -> canopy::security::attestation::route_attestation_state
    {
        if (!attested_zone_id.is_set())
            return {};

        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(attested_zone_id);
        if (item == attestation_route_states_.end())
            return {};
        return item->second;
    }

    void enclave_service::set_attestation_service(std::shared_ptr<canopy::security::attestation::attestation_service> service)
    {
        std::lock_guard<std::mutex> lock(attestation_service_mutex_);
        attestation_service_ = std::move(service);
    }

    auto enclave_service::get_attestation_service() const
        -> std::shared_ptr<canopy::security::attestation::attestation_service>
    {
        std::lock_guard<std::mutex> lock(attestation_service_mutex_);
        return attestation_service_;
    }

    void enclave_service::set_protected_rpc_enabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(attestation_service_mutex_);
        protected_rpc_enabled_ = enabled;
    }

    bool enclave_service::protected_rpc_enabled() const
    {
        std::lock_guard<std::mutex> lock(attestation_service_mutex_);
        return protected_rpc_enabled_;
    }

    auto enclave_service::find_security_context_for_protected_call(
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id) const -> std::optional<canopy::security::attestation::security_context>
    {
        if (caller_zone_id != get_zone_id())
            return std::nullopt;
        if (!destination_zone_id.is_set())
            return std::nullopt;
        return get_security_context(destination_zone_id);
    }

    CORO_TASK(send_result)
    enclave_service::send(send_params params)
    {
        if (!canopy::security::attestation::is_protected_rpc_envelope(
                params.interface_id, params.method_id, params.protocol_version))
        {
            CO_RETURN CO_AWAIT child_service::send(std::move(params));
        }

        auto service = get_attestation_service();
        if (!protected_rpc_enabled() || !service)
        {
            CO_RETURN send_result{rpc::error::ZONE_NOT_SUPPORTED(), {}, {}};
        }

        const auto outer = params;
        auto request = canopy::security::attestation::unprotect_send_request(*service, outer);
        if (!request.accepted)
        {
            CO_RETURN send_result{request.error.error_code, {}, {}};
        }

        auto response = CO_AWAIT child_service::send(std::move(request.value.params));
        auto protected_response = canopy::security::attestation::protect_send_response(
            *service, request.value.context, outer, request.value.request_counter, std::move(response));
        if (!protected_response.accepted)
        {
            CO_RETURN send_result{protected_response.error.error_code, {}, {}};
        }

        CO_RETURN std::move(protected_response.value);
    }

    CORO_TASK(void)
    enclave_service::post(post_params params)
    {
        if (!canopy::security::attestation::is_protected_rpc_envelope(
                params.interface_id, params.method_id, params.protocol_version))
        {
            CO_AWAIT child_service::post(std::move(params));
            CO_RETURN;
        }

        auto service = get_attestation_service();
        if (!protected_rpc_enabled() || !service)
            CO_RETURN;

        auto request = canopy::security::attestation::unprotect_post_request(*service, params);
        if (!request.accepted)
            CO_RETURN;

        CO_AWAIT child_service::post(std::move(request.value.params));
        CO_RETURN;
    }

    CORO_TASK(send_result)
    enclave_service::outbound_send(
        send_params params,
        std::shared_ptr<transport> transport)
    {
        auto service = get_attestation_service();
        if (!protected_rpc_enabled() || !service)
        {
            CO_RETURN CO_AWAIT child_service::outbound_send(std::move(params), std::move(transport));
        }
        if (!transport)
        {
            CO_RETURN send_result{rpc::error::TRANSPORT_ERROR(), {}, {}};
        }

        auto context = find_security_context_for_protected_call(params.caller_zone_id, params.remote_object_id.as_zone());
        if (!context)
        {
            CO_RETURN send_result{rpc::error::ZONE_NOT_SUPPORTED(), {}, {}};
        }

        auto request = canopy::security::attestation::protect_send_request(*service, *context, std::move(params));
        if (!request.accepted)
        {
            CO_RETURN send_result{request.error.error_code, {}, {}};
        }

        auto outer_request = request.value.params;
        auto outer_response = CO_AWAIT child_service::outbound_send(std::move(request.value.params), std::move(transport));
        auto response = canopy::security::attestation::unprotect_send_response(
            *service, request.value.context, outer_request, request.value.request_counter, std::move(outer_response));
        if (!response.accepted)
        {
            CO_RETURN send_result{response.error.error_code, {}, {}};
        }

        CO_RETURN std::move(response.value);
    }

    CORO_TASK(void)
    enclave_service::outbound_post(
        post_params params,
        std::shared_ptr<transport> transport)
    {
        auto service = get_attestation_service();
        if (!protected_rpc_enabled() || !service)
        {
            CO_AWAIT child_service::outbound_post(std::move(params), std::move(transport));
            CO_RETURN;
        }
        if (!transport)
            CO_RETURN;

        auto context = find_security_context_for_protected_call(params.caller_zone_id, params.remote_object_id.as_zone());
        if (!context)
            CO_RETURN;

        auto request = canopy::security::attestation::protect_post_request(*service, *context, std::move(params));
        if (!request.accepted)
            CO_RETURN;

        CO_AWAIT child_service::outbound_post(std::move(request.value.params), std::move(transport));
        CO_RETURN;
    }
}
