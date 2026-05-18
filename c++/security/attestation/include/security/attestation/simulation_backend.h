/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <security/attestation/fake_backend.h>

#include <utility>

namespace canopy::security::attestation
{
    inline constexpr const char* simulation_backend_id = "sgx-sim";
    inline constexpr const char* simulation_evidence_media_type = "application/canopy-sim-evidence";
    inline constexpr const char* simulation_evidence_content_format = "canopy.sgx-sim.v1";

    /*
     * Development SGX-simulation profile.
     *
     * This backend deliberately does not claim hardware attestation. The current
     * implementation reuses fake_backend's signed development evidence format,
     * but gives it a distinct backend id and security_level::simulation so tests
     * and policy can distinguish "plain fake" from "SGX-sim build/profile".
     *
     * The next SGX-specific backend should replace the payload machinery with
     * SGX SDK simulation report mechanics where available. Even then, simulation
     * evidence must remain non-production evidence: it can prove the Canopy SGX
     * code path and enclave ABI are being exercised, but not hardware isolation
     * or a production platform TCB.
     */
    class simulation_backend final : public fake_backend
    {
    public:
        explicit simulation_backend(std::string development_key = fake_default_development_key)
            : fake_backend(
                  std::move(development_key),
                  fake_backend_profile{simulation_backend_id,
                      simulation_evidence_media_type,
                      simulation_evidence_content_format,
                      security_level::simulation})
        {
        }
    };
} // namespace canopy::security::attestation
