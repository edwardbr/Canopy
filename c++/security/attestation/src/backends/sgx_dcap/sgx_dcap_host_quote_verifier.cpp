/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backends/sgx_dcap/sgx_dcap_host_quote_verifier.h>

#include "crypto_hash.h"

#include <security/attestation/backends/sgx_dcap/sgx_dcap_quote3_parser.h>

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
            verdict.backend_id = sgx_dcap_backend_id;
            verdict.level = security_level::hardware;
            return verdict;
        }

        [[nodiscard]] auto accepted_qv_result(
            const std::vector<uint64_t>& allowed,
            uint64_t actual) -> bool
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

        [[nodiscard]] auto verify_delegated_result_policy(
            const sgx_dcap_host_quote_verifier_options& options,
            const sgx_dcap_verifier_input& input,
            const attestation_policy& policy) -> std::optional<attestation_verdict>
        {
            if (!input.quote.verification_result.has_value())
                return std::nullopt;

            switch (options.delegated_result_mode)
            {
            case sgx_dcap_delegated_verification_result_mode::reject_if_present:
                return reject("SGX DCAP delegated verification_result is not accepted by this verifier");
            case sgx_dcap_delegated_verification_result_mode::ignore:
                return std::nullopt;
            case sgx_dcap_delegated_verification_result_mode::require_verified:
                if (!options.verify_delegated_verification_result)
                    return reject("SGX DCAP delegated verification_result checker is not configured");
                break;
            }

            auto delegated = options.verify_delegated_verification_result(input, policy);
            if (!delegated.accepted)
            {
                if (delegated.failure_reason.empty())
                    delegated.failure_reason = "SGX DCAP delegated verification_result was not verified";
                return reject(std::move(delegated.failure_reason));
            }

            return std::nullopt;
        }
    } // namespace

    sgx_dcap_host_quote_verifier::sgx_dcap_host_quote_verifier(sgx_dcap_host_quote_verifier_options options)
        : options_(std::move(options))
    {
        if (!options_.extract_report_data)
            options_.extract_report_data = extract_sgx_quote3_report_data;
    }

    auto sgx_dcap_host_quote_verifier::verify_quote(
        const sgx_dcap_verifier_input& input,
        const attestation_policy& policy) const -> attestation_verdict
    {
        if (!options_.verify_quote)
            return reject("SGX DCAP quote verifier callback is not configured");

        if (options_.require_report_data_match)
        {
            if (!options_.extract_report_data)
                return reject("SGX DCAP quote report_data extractor is not configured");

            auto report_data = options_.extract_report_data(input.quote.quote);
            if (!report_data.has_value() || !report_data_matches(report_data.value(), input.report_data_sha256))
                return reject("SGX DCAP quote report_data does not match the Canopy transcript binding");
        }

        if (auto delegated_error = verify_delegated_result_policy(options_, input, policy); delegated_error.has_value())
            return std::move(delegated_error.value());

        // The concrete callback owns Intel QvL/QvE details: collateral lookup,
        // quote signature checks, PCK chain validation, QvE report verification,
        // and measurement/advisory policy. This adapter then maps the returned
        // result into Canopy's backend-neutral verdict shape.
        auto verification = options_.verify_quote(input, policy);
        if (!verification.call_succeeded)
        {
            if (verification.failure_reason.empty())
                verification.failure_reason = "SGX DCAP quote verification call failed";
            return reject(std::move(verification.failure_reason));
        }

        if (options_.reject_expired_collateral && verification.result.collateral_expired != 0)
            return reject("SGX DCAP collateral is expired");

        if (!accepted_qv_result(options_.accepted_quote_verification_results, verification.result.quote_verification_result))
        {
            return reject("SGX DCAP quote-verification result is not accepted by policy");
        }

        attestation_verdict verdict;
        verdict.accepted = true;
        verdict.reason = verification.result.quote_verification_result_name.empty()
                             ? "SGX DCAP quote accepted"
                             : "SGX DCAP quote accepted: " + verification.result.quote_verification_result_name;
        verdict.backend_id = sgx_dcap_backend_id;
        verdict.level = security_level::hardware;
        verdict.peer_identity = input.expected_binding.subject;
        return verdict;
    }
} // namespace canopy::security::attestation
