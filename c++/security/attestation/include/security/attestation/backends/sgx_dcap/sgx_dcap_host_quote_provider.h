/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <security/attestation/backends/sgx_dcap/sgx_dcap_backend.h>

namespace canopy::security::attestation
{
    struct sgx_dcap_target_info_result
    {
        std::vector<uint8_t> qe_target_info;
    };

    struct sgx_dcap_report_request
    {
        evidence_binding binding;
        std::vector<uint8_t> qe_target_info;
        std::vector<uint8_t> report_data_sha256;
    };

    struct sgx_dcap_get_quote_request
    {
        evidence_binding binding;
        std::vector<uint8_t> report;
        uint32_t quote_size{0};
        std::vector<uint8_t> report_data_sha256;
    };

    struct sgx_dcap_host_quote_provider_functions
    {
        std::function<std::optional<sgx_dcap_target_info_result>()> get_target_info;
        std::function<std::optional<uint32_t>()> get_quote_size;
        std::function<std::optional<std::vector<uint8_t>>(const sgx_dcap_get_quote_request& request)> get_quote;
    };

    struct sgx_dcap_host_quote_provider_options
    {
        sgx_dcap_host_quote_provider_functions functions;
        std::function<std::optional<std::vector<uint8_t>>(const sgx_dcap_report_request& request)> report_producer;
    };

    class sgx_dcap_host_quote_provider final : public sgx_dcap_quote_provider
    {
    public:
        explicit sgx_dcap_host_quote_provider(sgx_dcap_host_quote_provider_options options);

        [[nodiscard]] auto produce_quote(const sgx_dcap_quote_request& request) const
            -> std::optional<sgx_dcap_quote_material> override;

    private:
        sgx_dcap_host_quote_provider_options options_;
    };
} // namespace canopy::security::attestation
