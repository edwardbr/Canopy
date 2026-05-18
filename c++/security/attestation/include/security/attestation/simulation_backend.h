/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <security/attestation/fake_backend.h>

namespace canopy::security::attestation
{
    inline constexpr const char* simulation_backend_id = "sgx-sim";
    inline constexpr const char* simulation_evidence_media_type = "application/canopy-sim-evidence";
    inline constexpr const char* simulation_evidence_content_format = "canopy.sgx-sim.v1";
    inline constexpr const char* simulation_report_evidence_content_format = "canopy.sgx-sim-report.v1";

    /*
     * Development SGX-simulation profile.
     *
     * This backend deliberately does not claim hardware attestation. Host-only
     * builds still use fake_backend's signed development evidence format with a
     * distinct backend id and security_level::simulation.
     *
     * Intel SGX simulation enclave builds produce an additional IDL-defined
     * report payload that exercises sgx_self_target(), sgx_create_report(), and
     * sgx_verify_report() where the SDK exposes them. The carried report is
     * still simulation evidence. It can prove the Canopy SGX code path and
     * enclave ABI are being exercised, but not hardware isolation or a
     * production platform TCB.
     */
    class simulation_backend final : public attestation_backend
    {
    public:
        explicit simulation_backend(std::string development_key = fake_default_development_key);

        [[nodiscard]] auto backend_id() const -> std::string override;
        [[nodiscard]] auto level() const -> security_level override;
        [[nodiscard]] auto produce_evidence(const evidence_binding& binding) const -> cmw override;
        [[nodiscard]] auto verify_evidence(
            const cmw& evidence,
            const evidence_binding& expected_binding,
            const attestation_policy& policy) const -> attestation_verdict override;

    private:
        std::string development_key_;
        fake_backend fallback_;
    };
} // namespace canopy::security::attestation
