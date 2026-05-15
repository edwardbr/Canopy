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
        canopy::security::attestation::route_attestation_state state;
        state.status = canopy::security::attestation::route_attestation_status::attested;
        state.context = std::move(context);
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
        attestation_route_states_.erase(attested_zone_id);
    }

    auto enclave_service::get_security_context(rpc::destination_zone attested_zone_id) const
        -> std::optional<canopy::security::attestation::security_context>
    {
        RPC_ASSERT(attested_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(attested_zone_id);
        if (item == attestation_route_states_.end())
            return std::nullopt;
        if (item->second.status != canopy::security::attestation::route_attestation_status::attested)
            return std::nullopt;
        if (!item->second.context || !item->second.context->established)
            return std::nullopt;
        return item->second.context;
    }

    void enclave_service::set_attestation_route_state(
        rpc::destination_zone attested_zone_id,
        canopy::security::attestation::route_attestation_state state)
    {
        RPC_ASSERT(attested_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        if (state.status != canopy::security::attestation::route_attestation_status::attested)
        {
            state.context.reset();
        }
        else if (!state.context || !state.context->established)
        {
            state.status = canopy::security::attestation::route_attestation_status::unknown;
            state.context.reset();
        }
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

    void enclave_service::set_add_ref_attestation_required(bool required)
    {
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        add_ref_attestation_required_ = required;
    }

    bool enclave_service::add_ref_attestation_required() const
    {
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        return add_ref_attestation_required_;
    }

    void enclave_service::set_route_unattested_allowed(
        rpc::destination_zone route_zone_id,
        bool allowed)
    {
        if (!route_zone_id.is_set())
            return;

        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(route_zone_id);
        if (allowed)
        {
            canopy::security::attestation::route_attestation_state state;
            state.status = canopy::security::attestation::route_attestation_status::unattested_allowed;
            attestation_route_states_[route_zone_id] = std::move(state);
            return;
        }

        if (item != attestation_route_states_.end()
            && item->second.status == canopy::security::attestation::route_attestation_status::unattested_allowed)
        {
            attestation_route_states_.erase(item);
        }
    }

    CORO_TASK(standard_result)
    enclave_service::ensure_add_ref_route_allowed(
        rpc::destination_zone route_zone_id,
        const char* operation)
    {
        if (!add_ref_attestation_required())
            CO_RETURN standard_result{rpc::error::OK(), {}};
        if (!route_zone_id.is_set())
            CO_RETURN standard_result{rpc::error::INVALID_DATA(), {}};
        if (route_zone_id == get_zone_id())
            CO_RETURN standard_result{rpc::error::OK(), {}};

        auto state = get_attestation_route_state(route_zone_id);
        const auto action = canopy::security::attestation::evaluate_route_attestation_state(state);
        if (action == canopy::security::attestation::route_attestation_action::allow)
        {
            CO_RETURN standard_result{rpc::error::OK(), {}};
        }
        if (action == canopy::security::attestation::route_attestation_action::reject)
        {
            RPC_WARNING(
                "add_ref attestation rejected for route {} during {}: previous failure {}",
                route_zone_id.get_subnet(),
                operation,
                state.failure_reason);
            CO_RETURN standard_result{rpc::error::ZONE_NOT_SUPPORTED(), {}};
        }
        if (action == canopy::security::attestation::route_attestation_action::wait_for_handshake)
        {
            RPC_WARNING("add_ref attestation pending for route {} during {}", route_zone_id.get_subnet(), operation);
            CO_RETURN standard_result{rpc::error::ZONE_NOT_SUPPORTED(), {}};
        }

        canopy::security::attestation::route_attestation_state handshaking_state;
        handshaking_state.status = canopy::security::attestation::route_attestation_status::handshaking;
        set_attestation_route_state(route_zone_id, std::move(handshaking_state));

        handshake_params params;
        params.protocol_version = rpc::HIGHEST_SUPPORTED_VERSION;
        params.caller_zone_id = get_zone_id();
        params.destination_zone_id = route_zone_id;
        auto handshake = CO_AWAIT child_service::handshake(std::move(params));

        auto post_handshake_state = get_attestation_route_state(route_zone_id);
        if (handshake.error_code == rpc::error::OK()
            && post_handshake_state.status == canopy::security::attestation::route_attestation_status::attested
            && post_handshake_state.context && post_handshake_state.context->established)
        {
            CO_RETURN standard_result{rpc::error::OK(), std::move(handshake.out_back_channel)};
        }
        if (handshake.error_code == rpc::error::OK()
            && post_handshake_state.status == canopy::security::attestation::route_attestation_status::unattested_allowed)
        {
            CO_RETURN standard_result{rpc::error::OK(), std::move(handshake.out_back_channel)};
        }

        canopy::security::attestation::route_attestation_state failed_state;
        failed_state.status = canopy::security::attestation::route_attestation_status::failed;
        failed_state.failure_epoch = state.failure_epoch + 1;
        failed_state.failure_reason = "add_ref route attestation handshake did not establish an allowed route";
        set_attestation_route_state(route_zone_id, std::move(failed_state));

        RPC_WARNING(
            "add_ref attestation failed for route {} during {}, handshake error {}",
            route_zone_id.get_subnet(),
            operation,
            handshake.error_code);
        CO_RETURN standard_result{rpc::error::ZONE_NOT_SUPPORTED(), {}};
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

    CORO_TASK(standard_result)
    enclave_service::add_ref(add_ref_params params)
    {
        auto route_result
            = CO_AWAIT ensure_add_ref_route_allowed(params.remote_object_id.as_zone(), "inbound add_ref remote object");
        if (route_result.error_code != rpc::error::OK())
            CO_RETURN route_result;

        CO_RETURN CO_AWAIT child_service::add_ref(std::move(params));
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

    CORO_TASK(standard_result)
    enclave_service::outbound_add_ref(
        add_ref_params params,
        std::shared_ptr<transport> transport)
    {
        if (transport)
        {
            auto route_result = CO_AWAIT ensure_add_ref_route_allowed(
                transport->get_adjacent_zone_id(), "outbound add_ref adjacent peer");
            if (route_result.error_code != rpc::error::OK())
                CO_RETURN route_result;
        }

        CO_RETURN CO_AWAIT child_service::outbound_add_ref(std::move(params), std::move(transport));
    }
}
