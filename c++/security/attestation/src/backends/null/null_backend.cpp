/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backends/null/null_backend.h>

namespace canopy::security::attestation
{
    auto null_backend::backend_id() const -> std::string
    {
        return null_backend_id;
    }

    auto null_backend::level() const -> security_level
    {
        return security_level::none;
    }

    auto null_backend::produce_evidence(const evidence_binding&) const -> cmw
    {
        cmw result;
        result.media_type = null_evidence_media_type;
        result.content_format = "canopy.null.v1";
        return result;
    }

    auto null_backend::verify_evidence(
        const cmw&,
        const evidence_binding&,
        const attestation_policy&) const -> attestation_verdict
    {
        attestation_verdict verdict;
        verdict.accepted = false;
        verdict.reason = "null backend cannot verify attestation evidence";
        verdict.backend_id = null_backend_id;
        verdict.level = security_level::none;
        return verdict;
    }
} // namespace canopy::security::attestation
