/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace canopy::security::attestation
{
    inline constexpr size_t aead_key_size = 32;
    inline constexpr size_t aead_nonce_size = 12;
    inline constexpr size_t aead_nonce_prefix_size = 4;

    enum class security_level
    {
        none,
        development,
        simulation,
        hardware_legacy,
        hardware
    };

    struct cmw
    {
        std::string media_type;
        std::string content_format;
        std::vector<uint8_t> payload;
    };

    struct identity
    {
        std::string enclave_id;
        std::string zone_id;
    };

    struct evidence_binding
    {
        identity subject;
        uint64_t transcript_id{0};
        std::vector<uint8_t> nonce;
    };

    struct attestation_policy
    {
        bool send_local_evidence{true};
        bool require_peer_evidence{true};
        bool allow_development_evidence{true};
        security_level minimum_security_level{security_level::development};
        std::string required_backend_id;
    };

    struct attestation_verdict
    {
        bool accepted{false};
        std::string reason;
        std::string backend_id;
        security_level level{security_level::none};
        identity peer_identity;
    };

    struct security_context
    {
        bool established{false};
        bool local_evidence_sent{false};
        bool peer_attested{false};
        identity local_identity;
        identity peer_identity;
        std::string backend_id;
        security_level level{security_level::none};
        std::string session_id;
        uint64_t transcript_id{0};
        uint64_t session_epoch{1};
    };

    enum class route_attestation_status
    {
        unknown,
        handshaking,
        attested,
        unattested_allowed,
        failed
    };

    enum class route_attestation_action
    {
        allow,
        start_handshake,
        wait_for_handshake,
        reject
    };

    struct route_attestation_state
    {
        route_attestation_status status{route_attestation_status::unknown};
        std::optional<security_context> context;
        std::string failure_reason;
        uint64_t failure_epoch{0};
    };

    [[nodiscard]] auto evaluate_route_attestation_state(const route_attestation_state& state) noexcept
        -> route_attestation_action;

    enum class protected_rpc_direction
    {
        caller_to_destination,
        destination_to_caller
    };

    struct protected_key_scope
    {
        std::string session_id;
        identity caller_identity;
        identity destination_identity;
        protected_rpc_direction direction{protected_rpc_direction::caller_to_destination};
    };

    struct aead_key_material
    {
        std::array<uint8_t, aead_key_size> key{};
        std::array<uint8_t, aead_nonce_prefix_size> nonce_prefix{};
        protected_key_scope scope;
        uint64_t session_epoch{0};
    };

    struct monotonic_counter_result
    {
        bool accepted{false};
        std::string reason;
        uint64_t counter{0};
    };

    class attestation_backend
    {
    public:
        virtual ~attestation_backend() = default;

        [[nodiscard]] virtual auto backend_id() const -> std::string = 0;
        [[nodiscard]] virtual auto level() const -> security_level = 0;
        [[nodiscard]] virtual auto produce_evidence(const evidence_binding& binding) const -> cmw = 0;
        [[nodiscard]] virtual auto verify_evidence(
            const cmw& evidence,
            const evidence_binding& expected_binding,
            const attestation_policy& policy) const -> attestation_verdict = 0;
    };

    [[nodiscard]] auto security_level_rank(security_level level) noexcept -> int;
    [[nodiscard]] auto security_level_name(security_level level) noexcept -> const char*;
    [[nodiscard]] auto make_session_id(
        const identity& local,
        const identity& peer,
        uint64_t transcript_id) -> std::string;
} // namespace canopy::security::attestation
