/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backends/sgx_dcap/sgx_dcap_backend.h>

#include "crypto_hash.h"

#include <attestation/sgx_dcap_protocol.h>
#include <rpc/internal/serialiser.h>

#include <utility>

namespace canopy::security::attestation
{
    namespace
    {
        constexpr size_t report_data_sha256_size = detail::crypto_sha256_digest_size;
        constexpr size_t max_dcap_cmw_payload_size = 1024U * 1024U;
        constexpr size_t max_dcap_quote_size = 256U * 1024U;
        constexpr size_t max_dcap_qve_report_size = 64U * 1024U;
        constexpr size_t max_dcap_qve_report_signature_size = 16U * 1024U;
        constexpr size_t max_dcap_supplemental_data_size = 256U * 1024U;
        constexpr size_t max_dcap_result_name_size = 128U;
        constexpr size_t max_dcap_advisory_ids_size = 16U * 1024U;

        [[nodiscard]] auto reject(std::string reason) -> attestation_verdict
        {
            attestation_verdict verdict;
            verdict.accepted = false;
            verdict.reason = std::move(reason);
            verdict.backend_id = sgx_dcap_backend_id;
            verdict.level = security_level::hardware;
            return verdict;
        }

        [[nodiscard]] auto make_wire_identity(const identity& value) -> rpc::attestation_identity
        {
            rpc::attestation_identity out;
            out.enclave_id = value.enclave_id;
            out.zone_id = value.zone_id;
            return out;
        }

        [[nodiscard]] auto make_identity(const rpc::attestation_identity& value) -> identity
        {
            return identity{value.enclave_id, value.zone_id};
        }

        [[nodiscard]] auto make_wire_binding(const evidence_binding& binding) -> rpc::attestation::sgx_dcap_report_binding
        {
            rpc::attestation::sgx_dcap_report_binding out;
            out.backend_id = sgx_dcap_backend_id;
            out.subject = make_wire_identity(binding.subject);
            out.transcript_id = binding.transcript_id;
            out.nonce = binding.nonce;
            return out;
        }

        template<typename T>
        [[nodiscard]] auto serialise_canonical(const T& value) -> std::optional<std::vector<uint8_t>>
        {
#ifdef CANOPY_BUILD_CANONICAL_CRYPTO
            return rpc::to_canonical_crypto<std::vector<uint8_t>>(value);
#else
            (void)value;
            return std::nullopt;
#endif
        }

        template<typename T>
        [[nodiscard]] auto deserialise_canonical(const std::vector<uint8_t>& bytes) -> std::optional<T>
        {
#ifdef CANOPY_BUILD_CANONICAL_CRYPTO
            T value;
            if (!rpc::from_canonical_crypto(rpc::byte_span(bytes), value).empty())
                return std::nullopt;
            return value;
#else
            (void)bytes;
            return std::nullopt;
#endif
        }

        [[nodiscard]] auto make_report_data_hash(const rpc::attestation::sgx_dcap_report_binding& binding)
            -> std::optional<std::vector<uint8_t>>
        {
            auto encoded = serialise_canonical(binding);
            if (!encoded.has_value())
                return std::nullopt;
            return detail::sha256_digest(encoded.value());
        }

        [[nodiscard]] auto make_unavailable_evidence() -> cmw
        {
            cmw out;
            out.media_type = sgx_dcap_evidence_media_type;
            out.content_format = sgx_dcap_unavailable_content_format;
            return out;
        }

        [[nodiscard]] auto to_wire_verification_result(const sgx_dcap_verification_result_material& result)
            -> rpc::attestation::sgx_dcap_verification_result
        {
            rpc::attestation::sgx_dcap_verification_result out;
            out.quote_verification_result = result.quote_verification_result;
            out.quote_verification_result_name = result.quote_verification_result_name;
            out.collateral_expired = result.collateral_expired;
            out.verification_time = result.verification_time;
            out.supplemental_data_version = result.supplemental_data_version;
            out.tee_type = result.tee_type;
            out.qve_report = result.qve_report;
            out.qve_report_signature = result.qve_report_signature;
            out.supplemental_data = result.supplemental_data;
            out.advisory_ids = result.advisory_ids;
            return out;
        }

        [[nodiscard]] auto from_wire_verification_result(const rpc::attestation::sgx_dcap_verification_result& result)
            -> sgx_dcap_verification_result_material
        {
            sgx_dcap_verification_result_material out;
            out.quote_verification_result = result.quote_verification_result;
            out.quote_verification_result_name = result.quote_verification_result_name;
            out.collateral_expired = result.collateral_expired;
            out.verification_time = result.verification_time;
            out.supplemental_data_version = result.supplemental_data_version;
            out.tee_type = result.tee_type;
            out.qve_report = result.qve_report;
            out.qve_report_signature = result.qve_report_signature;
            out.supplemental_data = result.supplemental_data;
            out.advisory_ids = result.advisory_ids;
            return out;
        }

        [[nodiscard]] auto to_wire_quote_evidence(
            const evidence_binding& binding,
            std::vector<uint8_t> report_data_sha256,
            const sgx_dcap_quote_material& quote) -> rpc::attestation::sgx_dcap_quote_evidence
        {
            rpc::attestation::sgx_dcap_quote_evidence out;
            out.binding = make_wire_binding(binding);
            out.report_data_sha256 = std::move(report_data_sha256);
            out.quote = quote.quote;
            if (quote.verification_result.has_value())
                out.verification_result = to_wire_verification_result(quote.verification_result.value());
            return out;
        }

        [[nodiscard]] auto from_wire_quote_material(const rpc::attestation::sgx_dcap_quote_evidence& evidence)
            -> sgx_dcap_quote_material
        {
            sgx_dcap_quote_material out;
            out.quote = evidence.quote;
            if (evidence.verification_result.has_value())
                out.verification_result = from_wire_verification_result(evidence.verification_result.value());
            return out;
        }

        [[nodiscard]] auto field_too_large(
            size_t size,
            size_t limit) noexcept -> bool
        {
            return size > limit;
        }

        [[nodiscard]] auto validate_verification_result_size(const sgx_dcap_verification_result_material& result)
            -> std::optional<std::string>
        {
            if (field_too_large(result.quote_verification_result_name.size(), max_dcap_result_name_size))
                return "SGX DCAP quote-verification result name is too large";
            if (field_too_large(result.qve_report.size(), max_dcap_qve_report_size))
                return "SGX DCAP QvE report is too large";
            if (field_too_large(result.qve_report_signature.size(), max_dcap_qve_report_signature_size))
                return "SGX DCAP QvE report signature is too large";
            if (field_too_large(result.supplemental_data.size(), max_dcap_supplemental_data_size))
                return "SGX DCAP supplemental data is too large";
            if (field_too_large(result.advisory_ids.size(), max_dcap_advisory_ids_size))
                return "SGX DCAP advisory id list is too large";
            return std::nullopt;
        }

        [[nodiscard]] auto validate_quote_material_size(const sgx_dcap_quote_material& quote) -> std::optional<std::string>
        {
            if (quote.quote.empty())
                return "SGX DCAP quote is missing";
            if (field_too_large(quote.quote.size(), max_dcap_quote_size))
                return "SGX DCAP quote is too large";
            if (quote.verification_result.has_value())
                return validate_verification_result_size(quote.verification_result.value());
            return std::nullopt;
        }

        [[nodiscard]] auto policy_accepts_dcap(const attestation_policy& policy) -> std::optional<attestation_verdict>
        {
            if (!policy.required_backend_id.empty() && policy.required_backend_id != sgx_dcap_backend_id)
                return reject("SGX DCAP evidence backend id did not match policy");
            if (security_level_rank(security_level::hardware) < security_level_rank(policy.minimum_security_level))
                return reject("SGX DCAP evidence did not meet minimum security level");
            return std::nullopt;
        }

        [[nodiscard]] auto binding_matches(
            const rpc::attestation::sgx_dcap_report_binding& actual,
            const evidence_binding& expected) -> bool
        {
            const auto actual_identity = make_identity(actual.subject);
            return actual.backend_id == sgx_dcap_backend_id && actual_identity.enclave_id == expected.subject.enclave_id
                   && actual_identity.zone_id == expected.subject.zone_id
                   && actual.transcript_id == expected.transcript_id && actual.nonce == expected.nonce;
        }
    } // namespace

    sgx_dcap_backend::sgx_dcap_backend(
        std::shared_ptr<sgx_dcap_quote_provider> quote_provider,
        std::shared_ptr<sgx_dcap_quote_verifier> quote_verifier)
        : quote_provider_(std::move(quote_provider))
        , quote_verifier_(std::move(quote_verifier))
    {
    }

    auto sgx_dcap_backend::backend_id() const -> std::string
    {
        return sgx_dcap_backend_id;
    }

    auto sgx_dcap_backend::level() const -> security_level
    {
        return security_level::hardware;
    }

    auto sgx_dcap_backend::produce_evidence(const evidence_binding& binding) const -> cmw
    {
        // DCAP quote generation needs an SGX-FLC platform, AESM/QPL, and the
        // enclave report flow. The default backend deliberately has no provider
        // wired, so non-DCAP builds return a typed unavailable CMW instead of
        // silently downgrading to fake or simulation evidence.
        if (!quote_provider_ || binding.transcript_id == 0 || binding.nonce.empty())
            return make_unavailable_evidence();

        auto wire_binding = make_wire_binding(binding);
        auto report_data_sha256 = make_report_data_hash(wire_binding);
        if (!report_data_sha256.has_value())
            return make_unavailable_evidence();

        sgx_dcap_quote_request request;
        request.binding = binding;
        request.report_data_sha256 = report_data_sha256.value();
        auto quote = quote_provider_->produce_quote(request);
        if (!quote.has_value() || quote->quote.empty())
            return make_unavailable_evidence();
        if (validate_quote_material_size(quote.value()).has_value())
            return make_unavailable_evidence();

        auto evidence = to_wire_quote_evidence(binding, std::move(report_data_sha256.value()), quote.value());
        auto payload = serialise_canonical(evidence);
        if (!payload.has_value())
            return make_unavailable_evidence();

        cmw out;
        out.media_type = sgx_dcap_evidence_media_type;
        out.content_format = sgx_dcap_quote_evidence_content_format;
        out.payload = std::move(payload.value());
        return out;
    }

    auto sgx_dcap_backend::verify_evidence(
        const cmw& evidence,
        const evidence_binding& expected_binding,
        const attestation_policy& policy) const -> attestation_verdict
    {
        if (evidence.media_type != sgx_dcap_evidence_media_type)
            return reject("unsupported SGX DCAP evidence media type");
        if (evidence.content_format == sgx_dcap_unavailable_content_format)
            return reject("SGX DCAP quote provider is not available");
        if (evidence.content_format != sgx_dcap_quote_evidence_content_format)
            return reject("unsupported SGX DCAP evidence content format");
        if (auto policy_error = policy_accepts_dcap(policy); policy_error.has_value())
            return std::move(policy_error.value());
        if (!quote_verifier_)
            return reject("SGX DCAP quote verifier is not configured");
        if (field_too_large(evidence.payload.size(), max_dcap_cmw_payload_size))
            return reject("SGX DCAP evidence payload is too large");

        auto parsed = deserialise_canonical<rpc::attestation::sgx_dcap_quote_evidence>(evidence.payload);
        if (!parsed.has_value())
            return reject("malformed SGX DCAP quote evidence");
        if (!binding_matches(parsed->binding, expected_binding))
            return reject("SGX DCAP evidence binding mismatch");

        auto expected_hash = make_report_data_hash(parsed->binding);
        if (!expected_hash.has_value() || expected_hash->size() != report_data_sha256_size
            || parsed->report_data_sha256.size() != report_data_sha256_size
            || !detail::constant_time_equal(parsed->report_data_sha256, expected_hash.value()))
            return reject("SGX DCAP report_data binding mismatch");
        auto quote = from_wire_quote_material(parsed.value());
        if (auto size_error = validate_quote_material_size(quote); size_error.has_value())
            return reject(std::move(size_error.value()));

        sgx_dcap_verifier_input input;
        input.expected_binding = expected_binding;
        input.report_data_sha256 = std::move(expected_hash.value());
        input.quote = std::move(quote);

        auto verdict = quote_verifier_->verify_quote(input, policy);
        if (!verdict.accepted)
        {
            if (verdict.backend_id.empty())
                verdict.backend_id = sgx_dcap_backend_id;
            if (verdict.level == security_level::none)
                verdict.level = security_level::hardware;
            return verdict;
        }

        verdict.backend_id = sgx_dcap_backend_id;
        verdict.level = security_level::hardware;
        verdict.peer_identity = expected_binding.subject;
        if (verdict.reason.empty())
            verdict.reason = "SGX DCAP quote accepted";
        return verdict;
    }
} // namespace canopy::security::attestation
