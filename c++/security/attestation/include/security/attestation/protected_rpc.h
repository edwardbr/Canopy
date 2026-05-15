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
        response = 3,
        add_ref = 4,
        release = 5,
        try_cast = 6,
        object_released = 7,
        transport_down = 8
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

    struct protected_add_ref_request
    {
        rpc::add_ref_params params;
        security_context context;
        uint64_t request_counter{protected_rpc_invalid_counter};
    };

    struct protected_release_request
    {
        rpc::release_params params;
        security_context context;
        uint64_t request_counter{protected_rpc_invalid_counter};
    };

    struct protected_try_cast_request
    {
        rpc::try_cast_params params;
        security_context context;
        uint64_t request_counter{protected_rpc_invalid_counter};
    };

    struct protected_object_released_request
    {
        rpc::object_released_params params;
        security_context context;
        uint64_t request_counter{protected_rpc_invalid_counter};
    };

    struct protected_transport_down_request
    {
        rpc::transport_down_params params;
        security_context context;
        uint64_t request_counter{protected_rpc_invalid_counter};
    };

    // Type fingerprint used when encrypted_payload is carried in payload_type_id fields.
    [[nodiscard]] auto encrypted_payload_type_id(uint64_t protocol_version) -> uint64_t;

    // Same fingerprint wrapped as an interface ordinal for protected send/post carriers.
    [[nodiscard]] auto encrypted_payload_interface_id(uint64_t protocol_version) -> rpc::interface_ordinal;

    // True when a reference/control payload field carries encrypted_payload bytes.
    [[nodiscard]] auto is_protected_rpc_payload(
        uint64_t payload_type_id,
        uint64_t protocol_version) -> bool;

    // True when a send/post-style interface+method pair is the protected envelope carrier.
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

    [[nodiscard]] auto protect_add_ref_request(
        attestation_service& service,
        const security_context& context,
        rpc::add_ref_params params) -> protected_rpc_result<protected_add_ref_request>;

    [[nodiscard]] auto unprotect_add_ref_request(
        attestation_service& service,
        const rpc::add_ref_params& outer) -> protected_rpc_result<protected_add_ref_request>;

    [[nodiscard]] auto protect_release_request(
        attestation_service& service,
        const security_context& context,
        rpc::release_params params) -> protected_rpc_result<protected_release_request>;

    [[nodiscard]] auto unprotect_release_request(
        attestation_service& service,
        const rpc::release_params& outer) -> protected_rpc_result<protected_release_request>;

    [[nodiscard]] auto protect_try_cast_request(
        attestation_service& service,
        const security_context& context,
        rpc::try_cast_params params) -> protected_rpc_result<protected_try_cast_request>;

    [[nodiscard]] auto unprotect_try_cast_request(
        attestation_service& service,
        const rpc::try_cast_params& outer) -> protected_rpc_result<protected_try_cast_request>;

    [[nodiscard]] auto protect_object_released_request(
        attestation_service& service,
        const security_context& context,
        rpc::object_released_params params) -> protected_rpc_result<protected_object_released_request>;

    [[nodiscard]] auto unprotect_object_released_request(
        attestation_service& service,
        const rpc::object_released_params& outer) -> protected_rpc_result<protected_object_released_request>;

    [[nodiscard]] auto protect_transport_down_request(
        attestation_service& service,
        const security_context& context,
        rpc::transport_down_params params) -> protected_rpc_result<protected_transport_down_request>;

    [[nodiscard]] auto unprotect_transport_down_request(
        attestation_service& service,
        const rpc::transport_down_params& outer) -> protected_rpc_result<protected_transport_down_request>;
} // namespace canopy::security::attestation
