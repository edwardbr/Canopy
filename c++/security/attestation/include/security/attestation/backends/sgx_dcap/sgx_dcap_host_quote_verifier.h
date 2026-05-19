/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <security/attestation/backends/sgx_dcap/sgx_dcap_backend.h>

namespace canopy::security::attestation
{
    struct sgx_dcap_host_quote_verification
    {
        bool call_succeeded{false};
        sgx_dcap_verification_result_material result;
        std::string failure_reason;
    };

    struct sgx_dcap_host_quote_verifier_options
    {
        // Intel's SGX_QL_QV_RESULT_OK value is zero. Additional QvL/QvE result
        // values such as CONFIG_NEEDED must be admitted deliberately by policy.
        std::vector<uint64_t> accepted_quote_verification_results{0};
        bool reject_expired_collateral{true};

        // Production wiring should call sgx_qv_verify_quote or tee_verify_quote
        // here and return the QvL/QvE result material. This adapter performs
        // Canopy policy mapping only after that concrete verifier succeeds.
        std::function<sgx_dcap_host_quote_verification(const sgx_dcap_verifier_input& input, const attestation_policy& policy)> verify_quote;
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
