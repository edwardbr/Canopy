/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <security/attestation/backends/sgx_epid/sgx_epid_backend.h>

namespace canopy::security::attestation
{
    struct sgx_epid_ias_report_validation_result
    {
        bool accepted{false};
        std::string reason;
    };

    struct sgx_epid_ias_report_verifier_options
    {
        // IAS quote status is policy, not wire correctness. The default accepts
        // only the clean IAS result; demos can explicitly add advisory statuses.
        std::vector<std::string> accepted_quote_statuses{"OK"};
        bool require_report_data_match{true};

        // Intel quote layout parsing is deliberately outside this generic
        // adapter. Production wiring should extract the 64-byte SGX report_data
        // field from the quote using Intel-owned structures, then this adapter
        // checks that it is Canopy's transcript hash followed by 32 zero bytes.
        std::function<std::optional<std::vector<uint8_t>>(const std::vector<uint8_t>& quote)> extract_report_data;

        // This callback is the load-bearing IAS implementation seam. It must
        // verify the IAS signature, signing certificate chain/trust anchor,
        // body_json/quote freshness, revocation status, advisories, enclave
        // measurements, debug state, and local policy before accepting.
        std::function<sgx_epid_ias_report_validation_result(const sgx_epid_verifier_input& input, const attestation_policy& policy)>
            validate_ias_report;
    };

    class sgx_epid_ias_report_verifier final : public sgx_epid_quote_verifier
    {
    public:
        explicit sgx_epid_ias_report_verifier(sgx_epid_ias_report_verifier_options options);

        [[nodiscard]] auto verify_quote(
            const sgx_epid_verifier_input& input,
            const attestation_policy& policy) const -> attestation_verdict override;

    private:
        sgx_epid_ias_report_verifier_options options_;
    };
} // namespace canopy::security::attestation
