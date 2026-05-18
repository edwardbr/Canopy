/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/service.h>

#include "kdf.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

#include <openssl/rand.h>

namespace canopy::security::attestation
{
    namespace
    {
        auto identity_sort_key(const identity& value) -> std::string
        {
            return value.enclave_id + "/" + value.zone_id;
        }

        auto direction_name(protected_rpc_direction direction) -> const char*
        {
            switch (direction)
            {
            case protected_rpc_direction::caller_to_destination:
                return "caller_to_destination";
            case protected_rpc_direction::destination_to_caller:
                return "destination_to_caller";
            }
            return "caller_to_destination";
        }

        auto make_scope_counter_key(const protected_key_scope& scope) -> std::string
        {
            return scope.session_id + "|" + identity_sort_key(scope.caller_identity) + "|"
                   + identity_sort_key(scope.destination_identity) + "|" + direction_name(scope.direction);
        }

        auto identity_has_enclave(const identity& value) -> bool
        {
            return !value.enclave_id.empty();
        }

        auto scope_matches_session(
            const protected_key_scope& scope,
            const security_context& context) -> bool
        {
            if (!context.established || scope.session_id != context.session_id)
                return false;
            if (!identity_has_enclave(scope.caller_identity) || !identity_has_enclave(scope.destination_identity))
                return false;

            const auto& local = context.local_identity.enclave_id;
            const auto& peer = context.peer_identity.enclave_id;
            const auto caller_matches
                = scope.caller_identity.enclave_id == local || scope.caller_identity.enclave_id == peer;
            const auto destination_matches
                = scope.destination_identity.enclave_id == local || scope.destination_identity.enclave_id == peer;
            return caller_matches && destination_matches;
        }

        auto rejected_counter(std::string reason) -> monotonic_counter_result
        {
            monotonic_counter_result result;
            result.accepted = false;
            result.reason = std::move(reason);
            return result;
        }

        auto accepted_counter(
            std::string reason,
            uint64_t counter) -> monotonic_counter_result
        {
            monotonic_counter_result result;
            result.accepted = true;
            result.reason = std::move(reason);
            result.counter = counter;
            return result;
        }
    } // namespace

    auto make_attestation_nonce() -> std::optional<std::vector<uint8_t>>
    {
        static_assert(attestation_nonce_size <= static_cast<size_t>(std::numeric_limits<int>::max()));

        std::vector<uint8_t> nonce(attestation_nonce_size);
        if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
            return std::nullopt;
        return nonce;
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

    attestation_service::attestation_service(attestation_service_options options)
        : options_(std::move(options))
    {
    }

    auto attestation_service::local_identity() const -> const identity&
    {
        return options_.local_identity;
    }

    auto attestation_service::policy() const -> const attestation_policy&
    {
        return options_.policy;
    }

    auto attestation_service::backend_id() const -> std::string
    {
        return options_.backend ? options_.backend->backend_id() : std::string{};
    }

    auto attestation_service::backend_level() const -> security_level
    {
        return options_.backend ? options_.backend->level() : security_level::none;
    }

    auto attestation_service::should_send_local_evidence() const -> bool
    {
        return options_.policy.send_local_evidence;
    }

    auto attestation_service::requires_peer_evidence() const -> bool
    {
        return options_.policy.require_peer_evidence;
    }

    auto attestation_service::allows_unattested_peer() const -> bool
    {
        return options_.policy.allow_unattested_peer;
    }

    auto attestation_service::produce_evidence(
        uint64_t transcript_id,
        std::vector<uint8_t> nonce) const -> evidence_result
    {
        evidence_result result;
        if (!options_.policy.send_local_evidence)
        {
            result.accepted = true;
            result.reason = "local evidence not required by policy";
            return result;
        }

        if (!options_.backend)
        {
            result.reason = "no attestation backend configured";
            return result;
        }

        if (options_.backend->level() == security_level::none)
        {
            result.reason = "configured backend cannot produce attestation evidence";
            return result;
        }

        evidence_binding binding;
        binding.subject = options_.local_identity;
        binding.transcript_id = transcript_id;
        binding.nonce = std::move(nonce);
        result.evidence = options_.backend->produce_evidence(binding);
        result.accepted = true;
        result.reason = "local evidence produced";
        return result;
    }

    auto attestation_service::verify_peer_evidence(
        const cmw& evidence,
        evidence_binding expected_binding) const -> attestation_verdict
    {
        if (!options_.backend)
        {
            attestation_verdict verdict;
            verdict.accepted = false;
            verdict.reason = "no attestation backend configured";
            return verdict;
        }

        return options_.backend->verify_evidence(evidence, expected_binding, options_.policy);
    }

    auto attestation_service::establish_session(const establish_session_params& params) -> security_context
    {
        security_context context;
        context.local_evidence_sent = params.local_evidence_sent;
        context.peer_attested = params.peer_attested;
        context.local_identity = options_.local_identity;
        context.peer_identity = params.peer_identity;
        context.transcript_id = params.transcript_id;
        context.session_epoch = params.session_epoch;
        context.session_id = make_session_id(context.local_identity, context.peer_identity, context.transcript_id);

        if (params.peer_attested)
        {
            context.backend_id = params.verified_backend_id;
            context.level = params.verified_level;
        }
        else
        {
            context.backend_id = backend_id();
            context.level = backend_level();
        }

        const auto development_secret = params.shared_secret.empty()
                                            ? detail::derive_development_shared_secret(context)
                                            : std::optional<std::vector<uint8_t>>{params.shared_secret};
        if (!development_secret.has_value())
            return context;

        auto root_secret = detail::derive_session_root_secret(context, development_secret.value());
        if (!root_secret.has_value())
            return context;

        context.established = true;

        session_state state;
        state.context = context;
        state.root_secret = std::move(root_secret.value());

        {
            std::scoped_lock lock(sessions_mutex_);
            sessions_[context.session_id] = std::move(state);
        }
        return context;
    }

    auto attestation_service::find_session(const std::string& session_id) const -> std::optional<security_context>
    {
        std::scoped_lock lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end())
            return std::nullopt;
        return it->second.context;
    }

    auto attestation_service::session_count() const -> size_t
    {
        std::scoped_lock lock(sessions_mutex_);
        return sessions_.size();
    }

    auto attestation_service::derive_aead_key(const protected_key_scope& scope) const -> std::optional<aead_key_material>
    {
        std::scoped_lock lock(sessions_mutex_);
        auto it = sessions_.find(scope.session_id);
        if (it == sessions_.end())
            return std::nullopt;
        if (!scope_matches_session(scope, it->second.context))
            return std::nullopt;

        const auto output_size = aead_key_size + aead_nonce_prefix_size;
        auto output
            = detail::derive_protected_rpc_key_material(it->second.root_secret, scope, it->second.context, output_size);
        if (!output.has_value() || output->size() != output_size)
            return std::nullopt;

        aead_key_material material;
        std::copy(output->begin(), output->begin() + static_cast<std::ptrdiff_t>(aead_key_size), material.key.begin());
        std::copy(
            output->begin() + static_cast<std::ptrdiff_t>(aead_key_size), output->end(), material.nonce_prefix.begin());
        material.scope = scope;
        material.session_epoch = it->second.context.session_epoch;
        return material;
    }

    auto attestation_service::next_send_counter(const protected_key_scope& scope) -> monotonic_counter_result
    {
        std::scoped_lock lock(sessions_mutex_);
        auto it = sessions_.find(scope.session_id);
        if (it == sessions_.end())
            return rejected_counter("attestation session not found");
        if (!scope_matches_session(scope, it->second.context))
            return rejected_counter("protected key scope does not match attestation session");

        auto& state = it->second.counters[make_scope_counter_key(scope)];
        if (state.send_exhausted || state.next_send == 0 || state.next_send > options_.max_counter_value)
            return rejected_counter("monotonic send counter exhausted");

        const auto value = state.next_send;
        if (state.next_send == options_.max_counter_value)
            state.send_exhausted = true;
        else
            ++state.next_send;

        return accepted_counter("monotonic send counter allocated", value);
    }

    auto attestation_service::accept_receive_counter(
        const protected_key_scope& scope,
        uint64_t counter) -> monotonic_counter_result
    {
        std::scoped_lock lock(sessions_mutex_);
        auto it = sessions_.find(scope.session_id);
        if (it == sessions_.end())
            return rejected_counter("attestation session not found");
        if (!scope_matches_session(scope, it->second.context))
            return rejected_counter("protected key scope does not match attestation session");
        if (counter == 0)
            return rejected_counter("monotonic receive counter must be non-zero");
        if (counter > options_.max_counter_value)
            return rejected_counter("monotonic receive counter exceeds configured limit");

        auto& state = it->second.counters[make_scope_counter_key(scope)];
        if (counter <= state.highest_received)
            return rejected_counter("monotonic receive counter replayed or out of order");

        state.highest_received = counter;
        return accepted_counter("monotonic receive counter accepted", counter);
    }

    auto attestation_service::make_aead_nonce(
        const aead_key_material& key,
        uint64_t counter) const
        -> std::array<
            uint8_t,
            aead_nonce_size>
    {
        static_assert(aead_nonce_prefix_size + sizeof(uint64_t) == aead_nonce_size);

        std::array<uint8_t, aead_nonce_size> nonce{};
        std::copy(key.nonce_prefix.begin(), key.nonce_prefix.end(), nonce.begin());
        for (size_t byte = 0; byte < sizeof(uint64_t); ++byte)
        {
            const auto shift = static_cast<unsigned>((sizeof(uint64_t) - byte - 1) * 8);
            nonce[aead_nonce_prefix_size + byte] = static_cast<uint8_t>((counter >> shift) & 0xffU);
        }
        return nonce;
    }
} // namespace canopy::security::attestation
