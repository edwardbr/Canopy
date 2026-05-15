/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <rpc/rpc.h>
#include <security/attestation/service.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace canopy::security::attestation
{
    inline constexpr uint64_t protected_rpc_envelope_tag = 0x43414e4f50595052ULL; // "CANOPYPR"
    inline constexpr uint32_t protected_rpc_envelope_version = 1;
    inline constexpr uint64_t protected_rpc_outer_method_id = 0;
    inline constexpr uint64_t protected_rpc_unset_transport_request_id = 0;
    inline constexpr uint64_t protected_rpc_unset_service_request_id = 0;
    inline constexpr uint64_t protected_rpc_invalid_counter = 0;

    enum class protected_rpc_kind : uint8_t
    {
        send = 1,
        post = 2,
        response = 3
    };

    struct protected_rpc_error
    {
        int error_code{rpc::error::OK()};
        std::string reason;
    };

    template<typename T> struct protected_rpc_result
    {
        bool accepted{false};
        protected_rpc_error error;
        T value{};
    };

    struct protected_send_request
    {
        rpc::send_params params;
        security_context context;
        uint64_t request_counter{protected_rpc_invalid_counter};
    };

    struct protected_post_request
    {
        rpc::post_params params;
        security_context context;
        uint64_t request_counter{protected_rpc_invalid_counter};
    };

    [[nodiscard]] auto encrypted_payload_interface_id(uint64_t protocol_version) -> rpc::interface_ordinal;
    [[nodiscard]] auto is_protected_rpc_envelope(
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        uint64_t protocol_version) -> bool;

    [[nodiscard]] auto protect_send_request(
        attestation_service& service,
        const security_context& context,
        rpc::send_params params) -> protected_rpc_result<protected_send_request>;

    [[nodiscard]] auto unprotect_send_request(
        attestation_service& service,
        const rpc::send_params& outer) -> protected_rpc_result<protected_send_request>;

    [[nodiscard]] auto protect_send_response(
        attestation_service& service,
        const security_context& context,
        const rpc::send_params& outer_request,
        uint64_t request_counter,
        rpc::send_result response) -> protected_rpc_result<rpc::send_result>;

    [[nodiscard]] auto unprotect_send_response(
        attestation_service& service,
        const security_context& context,
        const rpc::send_params& outer_request,
        uint64_t request_counter,
        rpc::send_result outer_response) -> protected_rpc_result<rpc::send_result>;

    [[nodiscard]] auto protect_post_request(
        attestation_service& service,
        const security_context& context,
        rpc::post_params params) -> protected_rpc_result<protected_post_request>;

    [[nodiscard]] auto unprotect_post_request(
        attestation_service& service,
        const rpc::post_params& outer) -> protected_rpc_result<protected_post_request>;
} // namespace canopy::security::attestation
