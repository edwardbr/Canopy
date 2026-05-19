/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backends/sgx_epid/sgx_epid_host_quote_provider.h>

#include "crypto_hash.h"

#include <limits>
#include <utility>

namespace canopy::security::attestation
{
    namespace
    {
        constexpr size_t epid_spid_size = 16U;
        constexpr size_t max_epid_sig_rl_size = 64U * 1024U;
        constexpr uint32_t max_epid_quote_size = 64U * 1024U;

        [[nodiscard]] auto valid_options(const sgx_epid_host_quote_provider_options& options) -> bool
        {
            return options.spid.size() == epid_spid_size && options.sig_rl.size() <= max_epid_sig_rl_size
                   && options.functions.init_quote && options.functions.calculate_quote_size
                   && options.functions.get_quote && options.report_producer;
        }
    } // namespace

    sgx_epid_host_quote_provider::sgx_epid_host_quote_provider(sgx_epid_host_quote_provider_options options)
        : options_(std::move(options))
    {
    }

    auto sgx_epid_host_quote_provider::produce_quote(const sgx_epid_quote_request& request) const
        -> std::optional<sgx_epid_quote_material>
    {
        if (!valid_options(options_) || request.binding.transcript_id == 0
            || request.report_data_sha256.size() != detail::crypto_sha256_digest_size)
        {
            return std::nullopt;
        }

        auto init = options_.functions.init_quote();
        if (!init.has_value() || init->qe_target_info.empty())
            return std::nullopt;

        sgx_epid_report_request report_request;
        report_request.binding = request.binding;
        report_request.qe_target_info = init->qe_target_info;
        report_request.report_data_sha256 = request.report_data_sha256;

        auto report = options_.report_producer(report_request);
        if (!report.has_value() || report->empty())
            return std::nullopt;

        auto quote_size = options_.functions.calculate_quote_size(options_.sig_rl);
        if (!quote_size.has_value() || quote_size.value() == 0 || quote_size.value() > max_epid_quote_size)
            return std::nullopt;

        sgx_epid_get_quote_request quote_request;
        quote_request.binding = request.binding;
        quote_request.report = std::move(report.value());
        quote_request.quote_sign_type = options_.quote_sign_type;
        quote_request.spid = options_.spid;
        quote_request.sig_rl = options_.sig_rl;
        quote_request.quote_size = quote_size.value();
        quote_request.report_data_sha256 = request.report_data_sha256;

        auto quote = options_.functions.get_quote(quote_request);
        if (!quote.has_value() || quote->empty() || quote->size() > quote_size.value())
            return std::nullopt;

        sgx_epid_quote_material material;
        material.extended_epid_group_id = init->extended_epid_group_id;
        material.quote_sign_type = options_.quote_sign_type;
        material.spid = options_.spid;
        material.sig_rl = options_.sig_rl;
        material.quote = std::move(quote.value());
        return material;
    }
} // namespace canopy::security::attestation
