/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <security/attestation/types.h>

namespace canopy::security::attestation
{
    inline constexpr const char* fake_backend_id = "fake";
    inline constexpr const char* fake_evidence_media_type = "application/canopy-fake-evidence";
    inline constexpr const char* fake_evidence_content_format = "canopy.fake.v1";

    class fake_backend final : public attestation_backend
    {
    public:
        explicit fake_backend(std::string development_key = "canopy-fake-attestation-development-key");

        [[nodiscard]] auto backend_id() const -> std::string override;
        [[nodiscard]] auto level() const -> security_level override;
        [[nodiscard]] auto produce_evidence(const evidence_binding& binding) const -> cmw override;
        [[nodiscard]] auto verify_evidence(
            const cmw& evidence,
            const evidence_binding& expected_binding,
            const attestation_policy& policy) const -> attestation_verdict override;

    private:
        std::string development_key_;
    };
} // namespace canopy::security::attestation
