/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/service.h>
#include <security/attestation/zone_security_policy.h>

#include "kdf.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
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

        void secure_clear(std::vector<uint8_t>& bytes) noexcept
        {
            if (!bytes.empty())
                OPENSSL_cleanse(bytes.data(), bytes.size());
            bytes.clear();
        }

        auto may_use_development_secret(security_level level) noexcept -> bool
        {
            return level == security_level::development || level == security_level::simulation;
        }

        auto constant_time_equal(
            const std::vector<uint8_t>& left,
            const std::vector<uint8_t>& right) noexcept -> bool
        {
            if (left.size() != right.size() || left.empty())
                return false;

            uint8_t difference = 0;
            for (size_t index = 0; index < left.size(); ++index)
                difference = static_cast<uint8_t>(difference | (left[index] ^ right[index]));
            return difference == 0;
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

    attestation_service::attestation_service(attestation_service_options options)
        : options_(std::move(options))
    {
    }

    attestation_service::session_state::~session_state()
    {
        secure_clear(root_secret);
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
        return policy_allows_unattested_peer(options_.policy);
    }

    auto attestation_service::supports_verifier_challenge() const -> bool
    {
        return options_.backend && options_.backend->supports_verifier_challenge();
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

    auto attestation_service::make_verifier_challenge(
        uint64_t transcript_id,
        std::vector<uint8_t> nonce) const -> evidence_result
    {
        evidence_result result;
        if (!options_.backend || !options_.backend->supports_verifier_challenge())
        {
            result.reason = "configured backend cannot produce a verifier challenge";
            return result;
        }

        evidence_binding binding;
        binding.subject = options_.local_identity;
        binding.transcript_id = transcript_id;
        binding.nonce = std::move(nonce);

        auto challenge = options_.backend->make_verifier_challenge(binding);
        if (!challenge.has_value())
        {
            result.reason = "failed to produce verifier challenge";
            return result;
        }

        result.accepted = true;
        result.reason = "verifier challenge produced";
        result.evidence = std::move(challenge.value());
        return result;
    }

    auto attestation_service::produce_evidence_for_challenge(
        const cmw& verifier_challenge,
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

        evidence_binding binding;
        binding.subject = options_.local_identity;
        binding.transcript_id = transcript_id;
        binding.nonce = std::move(nonce);

        auto evidence = options_.backend->produce_evidence_for_challenge(verifier_challenge, binding);
        if (!evidence.has_value())
        {
            result.reason = "configured backend cannot produce evidence for verifier challenge";
            return result;
        }

        result.accepted = true;
        result.reason = "challenge-bound local evidence produced";
        result.evidence = std::move(evidence.value());
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

    auto attestation_service::verify_peer_evidence_for_challenge(
        const cmw& evidence,
        const cmw& verifier_challenge,
        evidence_binding expected_binding) const -> attestation_verdict
    {
        if (!options_.backend)
        {
            attestation_verdict verdict;
            verdict.accepted = false;
            verdict.reason = "no attestation backend configured";
            return verdict;
        }

        return options_.backend->verify_evidence_for_challenge(
            evidence, verifier_challenge, expected_binding, options_.policy);
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

        auto session_secret = params.shared_secret.empty() && may_use_development_secret(context.level)
                                  ? detail::derive_development_shared_secret(context)
                                  : std::optional<std::vector<uint8_t>>{params.shared_secret};
        if (!session_secret.has_value() || session_secret->empty())
            return context;

        auto root_secret = detail::derive_session_root_secret(context, session_secret.value());
        secure_clear(session_secret.value());
        if (!root_secret.has_value())
            return context;

        context.established = true;

        {
            std::scoped_lock lock(sessions_mutex_);
            auto existing = sessions_.find(context.session_id);
            if (existing != sessions_.end())
            {
                if (context.session_epoch < existing->second.context.session_epoch)
                {
                    context.established = false;
                    secure_clear(root_secret.value());
                    return context;
                }
                if (context.session_epoch == existing->second.context.session_epoch)
                {
                    if (!constant_time_equal(root_secret.value(), existing->second.root_secret))
                        context.established = false;
                    secure_clear(root_secret.value());
                    return context;
                }

                // A higher epoch means a genuine re-key. Erase first so the old
                // session_state destructor scrubs root_secret before counters are
                // reset for the new key epoch.
                sessions_.erase(existing);
            }

            session_state state;
            state.context = context;
            state.root_secret = std::move(root_secret.value());
            sessions_.emplace(context.session_id, std::move(state));
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
        std::vector<uint8_t> root_secret;
        security_context context;
        {
            std::scoped_lock lock(sessions_mutex_);
            auto it = sessions_.find(scope.session_id);
            if (it == sessions_.end())
                return std::nullopt;
            if (!scope_matches_session(scope, it->second.context))
                return std::nullopt;

            // Copy the stable session inputs while holding the map lock, then
            // run HKDF after releasing it. Counter allocation remains locked
            // separately, but expensive key derivation no longer serialises all
            // sessions behind one process-wide mutex.
            root_secret = it->second.root_secret;
            context = it->second.context;
        }

        const auto output_size = aead_key_size + aead_nonce_prefix_size;
        auto output = detail::derive_protected_rpc_key_material(root_secret, scope, context, output_size);
        secure_clear(root_secret);
        if (!output.has_value() || output->size() != output_size)
            return std::nullopt;

        aead_key_material material;
        std::copy(output->begin(), output->begin() + static_cast<std::ptrdiff_t>(aead_key_size), material.key.begin());
        std::copy(
            output->begin() + static_cast<std::ptrdiff_t>(aead_key_size), output->end(), material.nonce_prefix.begin());
        secure_clear(output.value());
        material.scope = scope;
        material.session_epoch = context.session_epoch;
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

        // This is a deliberately strict replay model: messages for one
        // protected key scope must arrive in counter order. Ordered stream
        // transports satisfy that naturally. If a future transport can deliver
        // one scope out of order, replace this with a bounded replay window
        // before enabling pipelined protected traffic on that transport.
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
