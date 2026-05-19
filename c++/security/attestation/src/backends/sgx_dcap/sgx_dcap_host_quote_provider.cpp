/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backends/sgx_dcap/sgx_dcap_host_quote_provider.h>

#include "crypto_hash.h"

#include <utility>

namespace canopy::security::attestation
{
    namespace
    {
        constexpr uint32_t max_dcap_quote_size = 256U * 1024U;

        [[nodiscard]] auto valid_options(const sgx_dcap_host_quote_provider_options& options) -> bool
        {
            return options.functions.get_target_info && options.functions.get_quote_size && options.functions.get_quote
                   && options.report_producer;
        }
    } // namespace

    sgx_dcap_host_quote_provider::sgx_dcap_host_quote_provider(sgx_dcap_host_quote_provider_options options)
        : options_(std::move(options))
    {
    }

    auto sgx_dcap_host_quote_provider::produce_quote(const sgx_dcap_quote_request& request) const
        -> std::optional<sgx_dcap_quote_material>
    {
        if (!valid_options(options_) || request.binding.transcript_id == 0
            || request.report_data_sha256.size() != detail::crypto_sha256_digest_size)
        {
            return std::nullopt;
        }

        auto target_info = options_.functions.get_target_info();
        if (!target_info.has_value() || target_info->qe_target_info.empty())
            return std::nullopt;

        // The host quote path cannot manufacture an enclave report itself. It
        // first asks enclave-resident code to create a report targeted at the
        // quoting enclave, with report_data bound to Canopy's transcript hash.
        sgx_dcap_report_request report_request;
        report_request.binding = request.binding;
        report_request.qe_target_info = target_info->qe_target_info;
        report_request.report_data_sha256 = request.report_data_sha256;

        auto report = options_.report_producer(report_request);
        if (!report.has_value() || report->empty())
            return std::nullopt;

        auto quote_size = options_.functions.get_quote_size();
        if (!quote_size.has_value() || quote_size.value() == 0 || quote_size.value() > max_dcap_quote_size)
            return std::nullopt;

        // A production function table entry should call sgx_qe_get_quote (or a
        // TEE-compatible wrapper) with the enclave report and return the exact
        // quote bytes produced by the platform quote provider.
        sgx_dcap_get_quote_request quote_request;
        quote_request.binding = request.binding;
        quote_request.report = std::move(report.value());
        quote_request.quote_size = quote_size.value();
        quote_request.report_data_sha256 = request.report_data_sha256;

        auto quote = options_.functions.get_quote(quote_request);
        if (!quote.has_value() || quote->empty() || quote->size() > quote_size.value())
            return std::nullopt;

        sgx_dcap_quote_material material;
        material.quote = std::move(quote.value());
        return material;
    }
} // namespace canopy::security::attestation
