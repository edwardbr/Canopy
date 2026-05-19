/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <optional>

#include <security/attestation/types.h>

namespace canopy::security::attestation
{
    inline constexpr const char* sgx_dcap_backend_id = "sgx-dcap";
    inline constexpr const char* sgx_dcap_evidence_media_type = "application/canopy-sgx-dcap-evidence";
    inline constexpr const char* sgx_dcap_raw_quote_media_type = "application/sgx-quote3";
    inline constexpr const char* sgx_dcap_quote_evidence_content_format = "canopy.sgx-dcap-quote.v1";
    inline constexpr const char* sgx_dcap_unavailable_content_format = "canopy.sgx-dcap-unavailable.v1";

    struct sgx_dcap_verification_result_material
    {
        uint64_t quote_verification_result{0};
        std::string quote_verification_result_name;
        uint8_t collateral_expired{0};
        uint64_t verification_time{0};
        uint32_t supplemental_data_version{0};
        uint32_t tee_type{0};
        std::vector<uint8_t> qve_report;
        std::vector<uint8_t> qve_report_signature;
        std::vector<uint8_t> supplemental_data;
        std::string advisory_ids;
    };

    struct sgx_dcap_quote_material
    {
        std::vector<uint8_t> quote;
        std::optional<sgx_dcap_verification_result_material> verification_result;
    };

    struct sgx_dcap_quote_request
    {
        evidence_binding binding;
        std::vector<uint8_t> report_data_sha256;
    };

    struct sgx_dcap_verifier_input
    {
        evidence_binding expected_binding;
        std::vector<uint8_t> report_data_sha256;
        sgx_dcap_quote_material quote;
    };

    class sgx_dcap_quote_provider
    {
    public:
        virtual ~sgx_dcap_quote_provider() = default;

        // The provider is the only layer that should call quote-generation
        // APIs such as sgx_qe_get_target_info, sgx_create_report, and
        // sgx_qe_get_quote. The backend supplies the exact report_data hash
        // that must be embedded in the quote.
        [[nodiscard]] virtual auto produce_quote(const sgx_dcap_quote_request& request) const
            -> std::optional<sgx_dcap_quote_material> = 0;
    };

    class sgx_dcap_quote_verifier
    {
    public:
        virtual ~sgx_dcap_quote_verifier() = default;

        // Load-bearing production verifier contract:
        //
        // The backend verifies only the Canopy-owned wrapper: media type,
        // schema, transcript binding, field sizes, and the canonical
        // report_data hash. The concrete verifier must reject unless QL/QvL or
        // QvE/TVL appraisal has checked the quote signature, PCK/certificate
        // chain, collateral freshness, quote-verification result, advisory ids,
        // sgx_report_data_t == report_data_sha256 || zero(32), MRENCLAVE,
        // MRSIGNER, ISVPRODID, ISVSVN, debug state, TCB status, and local
        // policy. Only this verifier may map a real DCAP success to
        // security_level::hardware.
        [[nodiscard]] virtual auto verify_quote(
            const sgx_dcap_verifier_input& input,
            const attestation_policy& policy) const -> attestation_verdict = 0;
    };

    class sgx_dcap_backend final : public attestation_backend
    {
    public:
        sgx_dcap_backend(
            std::shared_ptr<sgx_dcap_quote_provider> quote_provider = nullptr,
            std::shared_ptr<sgx_dcap_quote_verifier> quote_verifier = nullptr);

        [[nodiscard]] auto backend_id() const -> std::string override;
        [[nodiscard]] auto level() const -> security_level override;
        [[nodiscard]] auto produce_evidence(const evidence_binding& binding) const -> cmw override;
        [[nodiscard]] auto verify_evidence(
            const cmw& evidence,
            const evidence_binding& expected_binding,
            const attestation_policy& policy) const -> attestation_verdict override;

    private:
        std::shared_ptr<sgx_dcap_quote_provider> quote_provider_;
        std::shared_ptr<sgx_dcap_quote_verifier> quote_verifier_;
    };
} // namespace canopy::security::attestation
