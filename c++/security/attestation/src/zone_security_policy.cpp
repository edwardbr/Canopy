/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/zone_security_policy.h>

#include <utility>

namespace canopy::security::attestation
{
    zone_security_policy::zone_security_policy(zone_security_policy_options options)
        : options_(std::move(options))
    {
    }

    void zone_security_policy::set_reference_routes_require_attestation(bool required) noexcept
    {
        options_.reference_routes_require_attestation = required;
    }

    auto zone_security_policy::reference_routes_require_attestation() const noexcept -> bool
    {
        return options_.reference_routes_require_attestation;
    }

    void zone_security_policy::set_peer_attestation_policy(attestation_policy policy)
    {
        options_.peer_attestation_policy = std::move(policy);
    }

    auto zone_security_policy::peer_attestation_policy() const noexcept -> const attestation_policy&
    {
        return options_.peer_attestation_policy;
    }

    auto zone_security_policy::evaluate_reference_route(reference_route_policy_input input) const -> route_policy_decision
    {
        input.attestation_required = reference_routes_require_attestation();
        return evaluate_reference_route_policy(input);
    }

    auto zone_security_policy::evaluate_missing_peer_evidence() const -> peer_evidence_policy_decision
    {
        return evaluate_missing_peer_evidence_policy(peer_attestation_policy());
    }

    auto policy_allows_unattested_peer(const attestation_policy& policy) noexcept -> bool
    {
        return !policy.require_peer_evidence && policy.allow_unattested_peer;
    }

    auto evaluate_route_attestation_state(const route_attestation_state& state) noexcept -> route_attestation_action
    {
        switch (state.status)
        {
        case route_attestation_status::unknown:
            return route_attestation_action::start_handshake;
        case route_attestation_status::handshaking:
            return route_attestation_action::wait_for_handshake;
        case route_attestation_status::attested:
            return state.context && state.context->established ? route_attestation_action::allow
                                                               : route_attestation_action::start_handshake;
        case route_attestation_status::unattested_allowed:
            return route_attestation_action::allow;
        case route_attestation_status::failed:
            return route_attestation_action::reject;
        }
        return route_attestation_action::reject;
    }

    auto evaluate_reference_route_policy(const reference_route_policy_input& input) -> route_policy_decision
    {
        route_policy_decision decision;

        if (!input.attestation_required)
        {
            decision.action = route_attestation_action::allow;
            decision.reason = "route attestation is not required by policy";
            return decision;
        }

        if (input.route_is_local)
        {
            decision.action = route_attestation_action::allow;
            decision.reason = "route is local to this zone";
            return decision;
        }

        decision.action = evaluate_route_attestation_state(input.state);
        switch (decision.action)
        {
        case route_attestation_action::allow:
            decision.reason = input.state.status == route_attestation_status::unattested_allowed
                                  ? "route is explicitly allowed without attestation"
                                  : "route has an established attestation context";
            return decision;
        case route_attestation_action::start_handshake:
            if (!input.may_start_handshake)
            {
                // release, try_cast, and object_released are valid only after an
                // add_ref established or explicitly allowed the route. Starting a
                // new handshake from those operations would let a forged lifetime
                // message create trust state out of order.
                decision.action = route_attestation_action::reject;
                decision.reason = "route has no completed add_ref attestation";
                return decision;
            }
            decision.reason = "route requires attestation handshake";
            return decision;
        case route_attestation_action::wait_for_handshake:
            decision.reason = "route attestation handshake is already in progress";
            return decision;
        case route_attestation_action::reject:
            decision.reason = input.state.failure_reason.empty() ? "route attestation previously failed"
                                                                 : input.state.failure_reason;
            return decision;
        }

        decision.action = route_attestation_action::reject;
        decision.reason = "route attestation state is invalid";
        return decision;
    }

    auto evaluate_missing_peer_evidence_policy(const attestation_policy& policy) -> peer_evidence_policy_decision
    {
        peer_evidence_policy_decision decision;
        decision.accepted = policy_allows_unattested_peer(policy);
        decision.reason = decision.accepted
                              ? "unattested route accepted by explicit policy"
                              : "route attestation did not include peer evidence and policy requires evidence";
        return decision;
    }
} // namespace canopy::security::attestation
