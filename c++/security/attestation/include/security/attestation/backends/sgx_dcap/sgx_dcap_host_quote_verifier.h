/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <security/attestation/backends/sgx_dcap/sgx_dcap_backend.h>

namespace canopy::security::attestation
{
    inline constexpr uint64_t sgx_dcap_qv_result_ok = 0;

    struct sgx_dcap_host_quote_verification
    {
        bool call_succeeded{false};
        sgx_dcap_verification_result_material result;
        std::string failure_reason;
    };

    struct sgx_dcap_delegated_verification_result_check
    {
        bool accepted{false};
        std::string failure_reason;
    };

    enum class sgx_dcap_delegated_verification_result_mode
    {
        // Default: producer-supplied verification_result material is rejected
        // unless a deployment has explicitly added and audited QvE delegation.
        reject_if_present,
        // The verifier callback appraises the raw quote and ignores any
        // producer-supplied verification_result material.
        ignore,
        // The producer-supplied result is permitted only after the configured
        // delegated-result checker verifies QvE report/signature, QvE
        // identity, collateral freshness, and local policy.
        require_verified
    };

    struct sgx_dcap_host_quote_verifier_options
    {
        // Intel's SGX_QL_QV_RESULT_OK value is zero. Additional QvL/QvE result
        // values such as CONFIG_NEEDED must be admitted deliberately by policy.
        std::vector<uint64_t> accepted_quote_verification_results{sgx_dcap_qv_result_ok};
        bool reject_expired_collateral{true};
        bool require_report_data_match{true};
        sgx_dcap_delegated_verification_result_mode delegated_result_mode{
            sgx_dcap_delegated_verification_result_mode::reject_if_present};

        // Defaults to Canopy's bounds-checked SGX quote3 parser. Override this
        // only for a different DCAP/TDX quote layout. This generic adapter then
        // checks that the first 32 bytes are Canopy's transcript hash and that
        // the remaining 32 bytes are zero. Keeping the binding check here
        // avoids a verifier callback accidentally accepting a valid Intel quote
        // that was not created for this Canopy handshake.
        std::function<std::optional<std::vector<uint8_t>>(const std::vector<uint8_t>& quote)> extract_report_data;

        // Production wiring should call sgx_qv_verify_quote or tee_verify_quote
        // here and return the QvL/QvE result material. If the wire evidence
        // carries verification_result, this callback must treat it as untrusted
        // unless it also verifies the QvE report/signature and accepted QvE
        // identity. This adapter performs Canopy policy mapping only after that
        // concrete verifier succeeds.
        std::function<sgx_dcap_host_quote_verification(const sgx_dcap_verifier_input& input, const attestation_policy& policy)> verify_quote;

        // Optional QvE delegated-result checker. This is deliberately separate
        // from verify_quote so that a deployment must make producer-supplied
        // verification_result semantics visible in configuration and tests.
        std::function<sgx_dcap_delegated_verification_result_check(
            const sgx_dcap_verifier_input& input, const attestation_policy& policy)>
            verify_delegated_verification_result;
    };

    class sgx_dcap_host_quote_verifier final : public sgx_dcap_quote_verifier
    {
    public:
        explicit sgx_dcap_host_quote_verifier(sgx_dcap_host_quote_verifier_options options);

        [[nodiscard]] auto verify_quote(
            const sgx_dcap_verifier_input& input,
            const attestation_policy& policy) const -> attestation_verdict override;

    private:
        sgx_dcap_host_quote_verifier_options options_;
    };
} // namespace canopy::security::attestation
