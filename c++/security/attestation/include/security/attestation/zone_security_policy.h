/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <security/attestation/types.h>

namespace canopy::security::attestation
{
    struct zone_security_policy_options
    {
        // This is the per-enclave-service route gate. A web-facing service can
        // leave it disabled or allow specific unattested routes, while a
        // treasury/control service can require every remote reference route to
        // be attested before add_ref creates local reference state.
        bool reference_routes_require_attestation{false};

        // This policy governs whether a peer may omit Evidence during the route
        // handshake. Backend appraisal still happens in attestation_service,
        // because it owns the verifier backend and cryptographic facts.
        attestation_policy peer_attestation_policy;
    };

    // One policy object should be associated with each enclave_service instance.
    // The attestation_service remains enclave-wide and cryptographic; this
    // object is where service-flavour policy starts to live.
    class zone_security_policy
    {
    public:
        explicit zone_security_policy(zone_security_policy_options options = {});
        virtual ~zone_security_policy() = default;

        void set_reference_routes_require_attestation(bool required) noexcept;
        [[nodiscard]] virtual auto reference_routes_require_attestation() const noexcept -> bool;

        void set_peer_attestation_policy(attestation_policy policy);
        [[nodiscard]] virtual auto peer_attestation_policy() const noexcept -> const attestation_policy&;

        [[nodiscard]] virtual auto evaluate_reference_route(reference_route_policy_input input) const
            -> route_policy_decision;
        [[nodiscard]] virtual auto evaluate_missing_peer_evidence() const -> peer_evidence_policy_decision;

    private:
        zone_security_policy_options options_;
    };

    // This helper is intentionally narrow: it answers only whether a route that
    // lacks peer Evidence may be accepted. Destination-zone authorization is a
    // separate policy layer built on top of this cryptographic route status.
    [[nodiscard]] auto policy_allows_unattested_peer(const attestation_policy& policy) noexcept -> bool;

    [[nodiscard]] auto evaluate_route_attestation_state(const route_attestation_state& state) noexcept
        -> route_attestation_action;
    [[nodiscard]] auto evaluate_reference_route_policy(const reference_route_policy_input& input) -> route_policy_decision;
    [[nodiscard]] auto evaluate_missing_peer_evidence_policy(const attestation_policy& policy)
        -> peer_evidence_policy_decision;
} // namespace canopy::security::attestation
