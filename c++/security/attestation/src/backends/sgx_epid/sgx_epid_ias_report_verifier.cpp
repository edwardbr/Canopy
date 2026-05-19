/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backends/sgx_epid/sgx_epid_ias_report_verifier.h>

#include "crypto_hash.h"

#include <algorithm>
#include <utility>

namespace canopy::security::attestation
{
    namespace
    {
        constexpr size_t sgx_report_data_size = 64U;
        constexpr size_t report_data_hash_size = detail::crypto_sha256_digest_size;

        [[nodiscard]] auto reject(std::string reason) -> attestation_verdict
        {
            attestation_verdict verdict;
            verdict.accepted = false;
            verdict.reason = std::move(reason);
            verdict.backend_id = sgx_epid_backend_id;
            verdict.level = security_level::hardware_legacy;
            return verdict;
        }

        [[nodiscard]] auto accepted_status(
            const std::vector<std::string>& allowed,
            const std::string& actual) -> bool
        {
            return std::find(allowed.begin(), allowed.end(), actual) != allowed.end();
        }

        [[nodiscard]] auto report_data_matches(
            const std::vector<uint8_t>& report_data,
            const std::vector<uint8_t>& expected_hash) -> bool
        {
            if (report_data.size() != sgx_report_data_size || expected_hash.size() != report_data_hash_size)
                return false;

            const std::vector<uint8_t> actual_hash(report_data.begin(), report_data.begin() + report_data_hash_size);
            if (!detail::constant_time_equal(actual_hash, expected_hash))
                return false;

            return std::all_of(
                report_data.begin() + report_data_hash_size, report_data.end(), [](uint8_t value) { return value == 0; });
        }
    } // namespace

    sgx_epid_ias_report_verifier::sgx_epid_ias_report_verifier(sgx_epid_ias_report_verifier_options options)
        : options_(std::move(options))
    {
    }

    auto sgx_epid_ias_report_verifier::verify_quote(
        const sgx_epid_verifier_input& input,
        const attestation_policy& policy) const -> attestation_verdict
    {
        if (!input.quote.ias_report.has_value())
            return reject("SGX EPID IAS report is missing");

        const auto& report = input.quote.ias_report.value();
        if (!accepted_status(options_.accepted_quote_statuses, report.quote_status))
            return reject("SGX EPID IAS quote status is not accepted by policy");

        if (options_.require_report_data_match)
        {
            if (!options_.extract_report_data)
                return reject("SGX EPID quote report_data extractor is not configured");

            auto report_data = options_.extract_report_data(input.quote.quote);
            if (!report_data.has_value() || !report_data_matches(report_data.value(), input.report_data_sha256))
                return reject("SGX EPID quote report_data does not match the Canopy transcript binding");
        }

        if (!options_.validate_ias_report)
            return reject("SGX EPID IAS report validator is not configured");

        auto validation = options_.validate_ias_report(input, policy);
        if (!validation.accepted)
        {
            if (validation.reason.empty())
                validation.reason = "SGX EPID IAS report validator rejected the quote";
            return reject(std::move(validation.reason));
        }

        attestation_verdict verdict;
        verdict.accepted = true;
        verdict.reason = validation.reason.empty() ? "SGX EPID IAS report accepted" : std::move(validation.reason);
        verdict.backend_id = sgx_epid_backend_id;
        verdict.level = security_level::hardware_legacy;
        verdict.peer_identity = input.expected_binding.subject;
        return verdict;
    }
} // namespace canopy::security::attestation
