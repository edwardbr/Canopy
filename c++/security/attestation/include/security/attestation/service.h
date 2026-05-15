/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <security/attestation/types.h>

namespace canopy::security::attestation
{
    struct attestation_service_options
    {
        identity local_identity;
        attestation_policy policy;
        std::shared_ptr<attestation_backend> backend;
        uint64_t max_counter_value{std::numeric_limits<uint64_t>::max()};
    };

    struct evidence_result
    {
        bool accepted{false};
        std::string reason;
        cmw evidence;
    };

    struct establish_session_params
    {
        identity peer_identity;
        uint64_t transcript_id{0};
        bool local_evidence_sent{false};
        bool peer_attested{false};
        std::string verified_backend_id;
        security_level verified_level{security_level::none};
        uint64_t session_epoch{1};
        std::vector<uint8_t> shared_secret;
    };

    class attestation_service final
    {
    public:
        explicit attestation_service(attestation_service_options options);

        [[nodiscard]] auto local_identity() const -> const identity&;
        [[nodiscard]] auto policy() const -> const attestation_policy&;
        [[nodiscard]] auto backend_id() const -> std::string;
        [[nodiscard]] auto backend_level() const -> security_level;
        [[nodiscard]] auto should_send_local_evidence() const -> bool;
        [[nodiscard]] auto requires_peer_evidence() const -> bool;

        [[nodiscard]] auto produce_evidence(
            uint64_t transcript_id,
            std::vector<uint8_t> nonce) const -> evidence_result;

        [[nodiscard]] auto verify_peer_evidence(
            const cmw& evidence,
            evidence_binding expected_binding) const -> attestation_verdict;

        [[nodiscard]] auto establish_session(const establish_session_params& params) -> security_context;
        [[nodiscard]] auto find_session(const std::string& session_id) const -> std::optional<security_context>;
        [[nodiscard]] auto session_count() const -> size_t;
        [[nodiscard]] auto derive_aead_key(const protected_key_scope& scope) const -> std::optional<aead_key_material>;
        [[nodiscard]] auto next_send_counter(const protected_key_scope& scope) -> monotonic_counter_result;
        [[nodiscard]] auto accept_receive_counter(
            const protected_key_scope& scope,
            uint64_t counter) -> monotonic_counter_result;
        [[nodiscard]] auto make_aead_nonce(
            const aead_key_material& key,
            uint64_t counter) const
            -> std::array<
                uint8_t,
                aead_nonce_size>;

    private:
        struct key_counter_state
        {
            uint64_t next_send{1};
            uint64_t highest_received{0};
            bool send_exhausted{false};
        };

        struct session_state
        {
            security_context context;
            std::vector<uint8_t> root_secret;
            std::unordered_map<std::string, key_counter_state> counters;
        };

        attestation_service_options options_;
        mutable std::mutex sessions_mutex_;
        std::unordered_map<std::string, session_state> sessions_;
    };
} // namespace canopy::security::attestation
