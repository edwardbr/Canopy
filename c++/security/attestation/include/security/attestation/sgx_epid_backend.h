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
    inline constexpr const char* sgx_epid_backend_id = "sgx-epid";
    inline constexpr const char* sgx_epid_evidence_media_type = "application/canopy-sgx-epid-evidence";
    inline constexpr const char* sgx_epid_quote_evidence_content_format = "canopy.sgx-epid-quote.v1";
    inline constexpr const char* sgx_epid_unavailable_content_format = "canopy.sgx-epid-unavailable.v1";

    struct sgx_epid_ias_report_material
    {
        std::string body_json;
        std::vector<uint8_t> signature;
        std::vector<uint8_t> signing_certificate_chain;
        std::string quote_status;
        std::string advisory_ids;
    };

    struct sgx_epid_quote_material
    {
        uint32_t extended_epid_group_id{0};
        uint32_t quote_sign_type{0};
        std::vector<uint8_t> spid;
        std::vector<uint8_t> basename;
        std::vector<uint8_t> sig_rl;
        std::vector<uint8_t> quote;
        std::optional<sgx_epid_ias_report_material> ias_report;
    };

    struct sgx_epid_quote_request
    {
        evidence_binding binding;
        std::vector<uint8_t> report_data_sha256;
    };

    struct sgx_epid_verifier_input
    {
        evidence_binding expected_binding;
        std::vector<uint8_t> report_data_sha256;
        sgx_epid_quote_material quote;
    };

    class sgx_epid_quote_provider
    {
    public:
        virtual ~sgx_epid_quote_provider() = default;

        // The provider is the only layer that should call the SGX PSW/SDK quote
        // APIs. The backend supplies the exact report_data hash that must be
        // embedded in the quote, so the RPC/security layer remains independent
        // of Intel header layouts.
        [[nodiscard]] virtual auto produce_quote(const sgx_epid_quote_request& request) const
            -> std::optional<sgx_epid_quote_material> = 0;
    };

    class sgx_epid_quote_verifier
    {
    public:
        virtual ~sgx_epid_quote_verifier() = default;

        // The verifier owns IAS report signature/status/revocation appraisal.
        // The backend validates the Canopy binding before this call, then
        // normalizes an accepted verdict to sgx-epid/hardware_legacy.
        [[nodiscard]] virtual auto verify_quote(
            const sgx_epid_verifier_input& input,
            const attestation_policy& policy) const -> attestation_verdict = 0;
    };

    class sgx_epid_backend final : public attestation_backend
    {
    public:
        sgx_epid_backend(
            std::shared_ptr<sgx_epid_quote_provider> quote_provider = nullptr,
            std::shared_ptr<sgx_epid_quote_verifier> quote_verifier = nullptr);

        [[nodiscard]] auto backend_id() const -> std::string override;
        [[nodiscard]] auto level() const -> security_level override;
        [[nodiscard]] auto produce_evidence(const evidence_binding& binding) const -> cmw override;
        [[nodiscard]] auto verify_evidence(
            const cmw& evidence,
            const evidence_binding& expected_binding,
            const attestation_policy& policy) const -> attestation_verdict override;

    private:
        std::shared_ptr<sgx_epid_quote_provider> quote_provider_;
        std::shared_ptr<sgx_epid_quote_verifier> quote_verifier_;
    };
} // namespace canopy::security::attestation
