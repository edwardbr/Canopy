/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/sgx_epid_backend.h>

#include "crypto_hash.h"

#include <attestation/sgx_epid_protocol.h>
#include <rpc/internal/serialiser.h>

#include <utility>

namespace canopy::security::attestation
{
    namespace
    {
        constexpr size_t report_data_sha256_size = detail::crypto_sha256_digest_size;
        constexpr size_t max_epid_cmw_payload_size = 256U * 1024U;
        constexpr size_t epid_spid_size = 16U;
        constexpr size_t max_epid_basename_size = 32U;
        constexpr size_t max_epid_sig_rl_size = 64U * 1024U;
        constexpr size_t max_epid_quote_size = 64U * 1024U;
        constexpr size_t max_ias_report_body_json_size = 64U * 1024U;
        constexpr size_t max_ias_signature_size = 16U * 1024U;
        constexpr size_t max_ias_signing_certificate_chain_size = 64U * 1024U;
        constexpr size_t max_ias_quote_status_size = 128U;
        constexpr size_t max_ias_advisory_ids_size = 4U * 1024U;

        [[nodiscard]] auto reject(std::string reason) -> attestation_verdict
        {
            attestation_verdict verdict;
            verdict.accepted = false;
            verdict.reason = std::move(reason);
            verdict.backend_id = sgx_epid_backend_id;
            verdict.level = security_level::hardware_legacy;
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

        [[nodiscard]] auto make_wire_binding(const evidence_binding& binding) -> rpc::attestation::sgx_epid_report_binding
        {
            rpc::attestation::sgx_epid_report_binding out;
            out.backend_id = sgx_epid_backend_id;
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

        [[nodiscard]] auto make_report_data_hash(const rpc::attestation::sgx_epid_report_binding& binding)
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
            out.media_type = sgx_epid_evidence_media_type;
            out.content_format = sgx_epid_unavailable_content_format;
            return out;
        }

        [[nodiscard]] auto to_wire_ias_report(const sgx_epid_ias_report_material& report)
            -> rpc::attestation::sgx_epid_ias_report
        {
            rpc::attestation::sgx_epid_ias_report out;
            out.body_json = report.body_json;
            out.signature = report.signature;
            out.signing_certificate_chain = report.signing_certificate_chain;
            out.quote_status = report.quote_status;
            out.advisory_ids = report.advisory_ids;
            return out;
        }

        [[nodiscard]] auto from_wire_ias_report(const rpc::attestation::sgx_epid_ias_report& report)
            -> sgx_epid_ias_report_material
        {
            sgx_epid_ias_report_material out;
            out.body_json = report.body_json;
            out.signature = report.signature;
            out.signing_certificate_chain = report.signing_certificate_chain;
            out.quote_status = report.quote_status;
            out.advisory_ids = report.advisory_ids;
            return out;
        }

        [[nodiscard]] auto to_wire_quote_evidence(
            const evidence_binding& binding,
            std::vector<uint8_t> report_data_sha256,
            const sgx_epid_quote_material& quote) -> rpc::attestation::sgx_epid_quote_evidence
        {
            rpc::attestation::sgx_epid_quote_evidence out;
            out.binding = make_wire_binding(binding);
            out.report_data_sha256 = std::move(report_data_sha256);
            out.extended_epid_group_id = quote.extended_epid_group_id;
            out.quote_sign_type = quote.quote_sign_type;
            out.spid = quote.spid;
            out.basename = quote.basename;
            out.sig_rl = quote.sig_rl;
            out.quote = quote.quote;
            if (quote.ias_report.has_value())
                out.ias_report = to_wire_ias_report(quote.ias_report.value());
            return out;
        }

        [[nodiscard]] auto from_wire_quote_material(const rpc::attestation::sgx_epid_quote_evidence& evidence)
            -> sgx_epid_quote_material
        {
            sgx_epid_quote_material out;
            out.extended_epid_group_id = evidence.extended_epid_group_id;
            out.quote_sign_type = evidence.quote_sign_type;
            out.spid = evidence.spid;
            out.basename = evidence.basename;
            out.sig_rl = evidence.sig_rl;
            out.quote = evidence.quote;
            if (evidence.ias_report.has_value())
                out.ias_report = from_wire_ias_report(evidence.ias_report.value());
            return out;
        }

        [[nodiscard]] auto field_too_large(
            size_t size,
            size_t limit) noexcept -> bool
        {
            return size > limit;
        }

        [[nodiscard]] auto validate_ias_report_size(const sgx_epid_ias_report_material& report)
            -> std::optional<std::string>
        {
            if (field_too_large(report.body_json.size(), max_ias_report_body_json_size))
                return "SGX EPID IAS report body is too large";
            if (field_too_large(report.signature.size(), max_ias_signature_size))
                return "SGX EPID IAS report signature is too large";
            if (field_too_large(report.signing_certificate_chain.size(), max_ias_signing_certificate_chain_size))
                return "SGX EPID IAS signing certificate chain is too large";
            if (field_too_large(report.quote_status.size(), max_ias_quote_status_size))
                return "SGX EPID IAS quote status is too large";
            if (field_too_large(report.advisory_ids.size(), max_ias_advisory_ids_size))
                return "SGX EPID IAS advisory id list is too large";
            return std::nullopt;
        }

        [[nodiscard]] auto validate_quote_material_size(const sgx_epid_quote_material& quote) -> std::optional<std::string>
        {
            if (quote.spid.size() != epid_spid_size)
                return "SGX EPID SPID has an invalid size";
            if (field_too_large(quote.basename.size(), max_epid_basename_size))
                return "SGX EPID basename is too large";
            if (field_too_large(quote.sig_rl.size(), max_epid_sig_rl_size))
                return "SGX EPID SigRL is too large";
            if (quote.quote.empty())
                return "SGX EPID quote is missing";
            if (field_too_large(quote.quote.size(), max_epid_quote_size))
                return "SGX EPID quote is too large";
            if (quote.ias_report.has_value())
                return validate_ias_report_size(quote.ias_report.value());
            return std::nullopt;
        }

        [[nodiscard]] auto policy_accepts_epid(const attestation_policy& policy) -> std::optional<attestation_verdict>
        {
            if (!policy.required_backend_id.empty() && policy.required_backend_id != sgx_epid_backend_id)
                return reject("SGX EPID evidence backend id did not match policy");
            if (security_level_rank(security_level::hardware_legacy) < security_level_rank(policy.minimum_security_level))
                return reject("SGX EPID evidence did not meet minimum security level");
            return std::nullopt;
        }

        [[nodiscard]] auto binding_matches(
            const rpc::attestation::sgx_epid_report_binding& actual,
            const evidence_binding& expected) -> bool
        {
            const auto actual_identity = make_identity(actual.subject);
            return actual.backend_id == sgx_epid_backend_id && actual_identity.enclave_id == expected.subject.enclave_id
                   && actual_identity.zone_id == expected.subject.zone_id
                   && actual.transcript_id == expected.transcript_id && actual.nonce == expected.nonce;
        }
    } // namespace

    sgx_epid_backend::sgx_epid_backend(
        std::shared_ptr<sgx_epid_quote_provider> quote_provider,
        std::shared_ptr<sgx_epid_quote_verifier> quote_verifier)
        : quote_provider_(std::move(quote_provider))
        , quote_verifier_(std::move(quote_verifier))
    {
    }

    auto sgx_epid_backend::backend_id() const -> std::string
    {
        return sgx_epid_backend_id;
    }

    auto sgx_epid_backend::level() const -> security_level
    {
        return security_level::hardware_legacy;
    }

    auto sgx_epid_backend::produce_evidence(const evidence_binding& binding) const -> cmw
    {
        // EPID quote generation requires the Intel PSW/AESM quote path and, in
        // production, IAS material. The default backend has no provider wired,
        // so it returns a typed unavailable CMW that peers reject cleanly rather
        // than silently downgrading to fake evidence.
        if (!quote_provider_ || binding.transcript_id == 0 || binding.nonce.empty())
            return make_unavailable_evidence();

        auto wire_binding = make_wire_binding(binding);
        auto report_data_sha256 = make_report_data_hash(wire_binding);
        if (!report_data_sha256.has_value())
            return make_unavailable_evidence();

        sgx_epid_quote_request request;
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
        out.media_type = sgx_epid_evidence_media_type;
        out.content_format = sgx_epid_quote_evidence_content_format;
        out.payload = std::move(payload.value());
        return out;
    }

    auto sgx_epid_backend::verify_evidence(
        const cmw& evidence,
        const evidence_binding& expected_binding,
        const attestation_policy& policy) const -> attestation_verdict
    {
        if (evidence.media_type != sgx_epid_evidence_media_type)
            return reject("unsupported SGX EPID evidence media type");
        if (evidence.content_format == sgx_epid_unavailable_content_format)
            return reject("SGX EPID quote provider is not available");
        if (evidence.content_format != sgx_epid_quote_evidence_content_format)
            return reject("unsupported SGX EPID evidence content format");
        if (auto policy_error = policy_accepts_epid(policy); policy_error.has_value())
            return std::move(policy_error.value());
        if (!quote_verifier_)
            return reject("SGX EPID quote verifier is not configured");
        if (field_too_large(evidence.payload.size(), max_epid_cmw_payload_size))
            return reject("SGX EPID evidence payload is too large");

        auto parsed = deserialise_canonical<rpc::attestation::sgx_epid_quote_evidence>(evidence.payload);
        if (!parsed.has_value())
            return reject("malformed SGX EPID quote evidence");
        if (!binding_matches(parsed->binding, expected_binding))
            return reject("SGX EPID evidence binding mismatch");

        auto expected_hash = make_report_data_hash(parsed->binding);
        if (!expected_hash.has_value() || expected_hash->size() != report_data_sha256_size
            || parsed->report_data_sha256.size() != report_data_sha256_size
            || !detail::constant_time_equal(parsed->report_data_sha256, expected_hash.value()))
            return reject("SGX EPID report_data binding mismatch");
        auto quote = from_wire_quote_material(parsed.value());
        if (auto size_error = validate_quote_material_size(quote); size_error.has_value())
            return reject(std::move(size_error.value()));

        sgx_epid_verifier_input input;
        input.expected_binding = expected_binding;
        input.report_data_sha256 = std::move(expected_hash.value());
        input.quote = std::move(quote);

        auto verdict = quote_verifier_->verify_quote(input, policy);
        if (!verdict.accepted)
        {
            if (verdict.backend_id.empty())
                verdict.backend_id = sgx_epid_backend_id;
            if (verdict.level == security_level::none)
                verdict.level = security_level::hardware_legacy;
            return verdict;
        }

        verdict.backend_id = sgx_epid_backend_id;
        verdict.level = security_level::hardware_legacy;
        verdict.peer_identity = expected_binding.subject;
        if (verdict.reason.empty())
            verdict.reason = "SGX EPID quote accepted";
        return verdict;
    }
} // namespace canopy::security::attestation
