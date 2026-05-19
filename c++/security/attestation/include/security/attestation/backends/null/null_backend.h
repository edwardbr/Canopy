/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <security/attestation/types.h>

namespace canopy::security::attestation
{
    inline constexpr const char* null_backend_id = "null";
    inline constexpr const char* null_evidence_media_type = "application/canopy-null-evidence";

    class null_backend final : public attestation_backend
    {
    public:
        [[nodiscard]] auto backend_id() const -> std::string override;
        [[nodiscard]] auto level() const -> security_level override;
        [[nodiscard]] auto produce_evidence(const evidence_binding& binding) const -> cmw override;
        [[nodiscard]] auto verify_evidence(
            const cmw& evidence,
            const evidence_binding& expected_binding,
            const attestation_policy& policy) const -> attestation_verdict override;
    };
} // namespace canopy::security::attestation
