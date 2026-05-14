/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/service.h>

#include <utility>

namespace canopy::security::attestation
{
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
        context.established = true;
        context.local_evidence_sent = params.local_evidence_sent;
        context.peer_attested = params.peer_attested;
        context.local_identity = options_.local_identity;
        context.peer_identity = params.peer_identity;
        context.transcript_id = params.transcript_id;
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

        {
            std::scoped_lock lock(sessions_mutex_);
            sessions_[context.session_id] = context;
        }
        return context;
    }

    auto attestation_service::find_session(const std::string& session_id) const -> std::optional<security_context>
    {
        std::scoped_lock lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end())
            return std::nullopt;
        return it->second;
    }

    auto attestation_service::session_count() const -> size_t
    {
        std::scoped_lock lock(sessions_mutex_);
        return sessions_.size();
    }
} // namespace canopy::security::attestation
