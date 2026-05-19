/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backends/simulation/simulation_backend.h>

#include "crypto_hash.h"

#include <attestation/sgx_sim_protocol.h>
#include <rpc/internal/serialiser.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(FOR_SGX) && defined(SGX_SIM) && !defined(CANOPY_FAKE_SGX)
#  include <sgx_utils.h>
#  define CANOPY_ATTESTATION_HAS_INTEL_SGX_SIM_REPORT 1
#else
#  define CANOPY_ATTESTATION_HAS_INTEL_SGX_SIM_REPORT 0
#endif

namespace canopy::security::attestation
{
    namespace
    {
        constexpr size_t sgx_report_data_size = 64;
        constexpr uint8_t sgx_sim_flag_false = 0;
        constexpr uint8_t sgx_sim_flag_true = 1;
        constexpr std::string_view sgx_sim_report_reason = "SGX simulation report evidence accepted";
        constexpr std::string_view sgx_sim_local_report_reason
            = "SGX simulation local-attestation report evidence accepted";
        constexpr std::string_view sgx_sim_development_reason
            = "SGX simulation report evidence accepted as development evidence";

        [[nodiscard]] auto make_simulation_fallback(std::string development_key) -> fake_backend
        {
            return fake_backend(
                std::move(development_key),
                fake_backend_profile{simulation_backend_id,
                    simulation_evidence_media_type,
                    simulation_evidence_content_format,
                    security_level::simulation});
        }

        [[nodiscard]] auto reject(std::string reason) -> attestation_verdict
        {
            attestation_verdict verdict;
            verdict.reason = std::move(reason);
            return verdict;
        }

        [[nodiscard]] auto policy_accepts_simulation(const attestation_policy& policy) -> std::optional<attestation_verdict>
        {
            if (!policy.allow_development_evidence)
                return reject("simulation evidence is not allowed by policy");
            if (!policy.required_backend_id.empty() && policy.required_backend_id != simulation_backend_id)
                return reject("policy requires a different backend");
            if (security_level_rank(security_level::simulation) < security_level_rank(policy.minimum_security_level))
                return reject("simulation evidence does not meet the minimum security level");
            return std::nullopt;
        }

#if CANOPY_ATTESTATION_HAS_INTEL_SGX_SIM_REPORT
        [[nodiscard]] auto make_rpc_identity(const identity& value) -> rpc::attestation_identity
        {
            rpc::attestation_identity out;
            out.enclave_id = value.enclave_id;
            out.zone_id = value.zone_id;
            return out;
        }

        [[nodiscard]] auto make_binding(const evidence_binding& binding) -> rpc::attestation::sgx_sim_report_binding
        {
            rpc::attestation::sgx_sim_report_binding out;
            out.backend_id = simulation_backend_id;
            out.subject = make_rpc_identity(binding.subject);
            out.transcript_id = binding.transcript_id;
            out.nonce = binding.nonce;
            return out;
        }
#endif

        [[nodiscard]] auto make_identity(const rpc::attestation_identity& value) -> identity
        {
            return identity{value.enclave_id, value.zone_id};
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

        [[nodiscard]] auto make_report_data_hash(const rpc::attestation::sgx_sim_report_binding& binding)
            -> std::optional<std::vector<uint8_t>>
        {
            auto encoded = serialise_canonical(binding);
            if (!encoded.has_value())
                return std::nullopt;
            return detail::sha256_digest(encoded.value());
        }

        [[nodiscard]] auto signature_payload(rpc::attestation::sgx_sim_report_evidence evidence)
            -> std::optional<std::vector<uint8_t>>
        {
            evidence.development_signature.clear();
            return serialise_canonical(evidence);
        }

        [[nodiscard]] auto signature_payload(rpc::attestation::sgx_sim_local_attestation_report evidence)
            -> std::optional<std::vector<uint8_t>>
        {
            evidence.development_signature.clear();
            return serialise_canonical(evidence);
        }

        [[nodiscard]] auto sign_evidence(
            const rpc::attestation::sgx_sim_report_evidence& evidence,
            std::string_view development_key) -> std::optional<std::vector<uint8_t>>
        {
            auto payload = signature_payload(evidence);
            if (!payload.has_value())
                return std::nullopt;
            return detail::hmac_sha256(development_key, payload.value());
        }

        [[nodiscard]] auto sign_evidence(
            const rpc::attestation::sgx_sim_local_attestation_report& evidence,
            std::string_view development_key) -> std::optional<std::vector<uint8_t>>
        {
            auto payload = signature_payload(evidence);
            if (!payload.has_value())
                return std::nullopt;
            return detail::hmac_sha256(development_key, payload.value());
        }

        template<typename T> [[nodiscard]] auto bytes_from_trivial_object(const T& value) -> std::vector<uint8_t>
        {
            static_assert(std::is_trivially_copyable_v<T>, "SGX report carrier must be trivially copyable");
            const auto* begin = reinterpret_cast<const uint8_t*>(&value);
            return std::vector<uint8_t>(begin, begin + sizeof(T));
        }

        template<typename T>
        [[nodiscard]] auto trivial_object_from_bytes(
            const std::vector<uint8_t>& bytes,
            T& value) -> bool
        {
            static_assert(std::is_trivially_copyable_v<T>, "SGX report carrier must be trivially copyable");
            if (bytes.size() != sizeof(T))
                return false;
            std::memcpy(&value, bytes.data(), sizeof(T));
            return true;
        }

        [[nodiscard]] auto make_local_report_data_hash(const rpc::attestation::sgx_sim_local_report_binding& binding)
            -> std::optional<std::vector<uint8_t>>
        {
            auto encoded = serialise_canonical(binding);
            if (!encoded.has_value())
                return std::nullopt;
            return detail::sha256_digest(encoded.value());
        }

#if CANOPY_ATTESTATION_HAS_INTEL_SGX_SIM_REPORT
        [[nodiscard]] auto make_local_report_binding(
            const evidence_binding& binding,
            const rpc::attestation::sgx_sim_local_attestation_challenge& challenge)
            -> rpc::attestation::sgx_sim_local_report_binding
        {
            rpc::attestation::sgx_sim_local_report_binding out;
            out.backend_id = simulation_backend_id;
            out.claimant = make_rpc_identity(binding.subject);
            out.verifier = challenge.verifier;
            out.transcript_id = challenge.transcript_id;
            out.challenge_nonce = challenge.nonce;
            return out;
        }

        [[nodiscard]] auto report_data_matches_hash(
            const sgx_report_t& report,
            const std::vector<uint8_t>& hash) -> bool
        {
            if (hash.size() != detail::crypto_sha256_digest_size)
                return false;
            if (!std::equal(hash.begin(), hash.end(), report.body.report_data.d))
                return false;
            const auto* zero_begin = report.body.report_data.d + detail::crypto_sha256_digest_size;
            const auto* zero_end = report.body.report_data.d + sgx_report_data_size;
            return std::all_of(zero_begin, zero_end, [](uint8_t value) { return value == 0; });
        }

        [[nodiscard]] auto append_sgx_sim_report(rpc::attestation::sgx_sim_report_evidence& evidence) -> bool
        {
            if (evidence.report_data_sha256.size() != detail::crypto_sha256_digest_size)
                return false;

            sgx_target_info_t target_info{};
            if (sgx_self_target(&target_info) != SGX_SUCCESS)
                return false;

            sgx_report_data_t report_data{};
            std::copy(evidence.report_data_sha256.begin(), evidence.report_data_sha256.end(), report_data.d);

            sgx_report_t report{};
            if (sgx_create_report(&target_info, &report_data, &report) != SGX_SUCCESS)
                return false;

            evidence.target_info = bytes_from_trivial_object(target_info);
            evidence.report = bytes_from_trivial_object(report);

            // This standalone evidence shape is self-targeted. Verifying it
            // here proves the SIM report path works in this enclave. Route
            // handshakes that carry verifier_challenge use
            // make_local_report_evidence() below for peer-targeted reports.
            sgx_report_t report_copy = report;
            if (sgx_verify_report(&report_copy) != SGX_SUCCESS)
                return false;
            evidence.report_verified_by_producer = sgx_sim_flag_true;
            return true;
        }

        [[nodiscard]] auto locally_verify_sgx_sim_report(
            const rpc::attestation::sgx_sim_report_evidence& evidence,
            const std::vector<uint8_t>& expected_hash) -> bool
        {
            sgx_report_t report{};
            if (!trivial_object_from_bytes(evidence.report, report))
                return false;
            if (!report_data_matches_hash(report, expected_hash))
                return false;
            return sgx_verify_report(&report) == SGX_SUCCESS;
        }
#endif

        [[nodiscard]] auto make_local_challenge(const evidence_binding& binding) -> std::optional<cmw>
        {
#if !CANOPY_ATTESTATION_HAS_INTEL_SGX_SIM_REPORT
            (void)binding;
            return std::nullopt;
#else
            if (binding.transcript_id == 0 || binding.nonce.empty())
                return std::nullopt;

            sgx_target_info_t target_info{};
            if (sgx_self_target(&target_info) != SGX_SUCCESS)
                return std::nullopt;

            rpc::attestation::sgx_sim_local_attestation_challenge challenge;
            challenge.backend_id = simulation_backend_id;
            challenge.verifier = make_rpc_identity(binding.subject);
            challenge.transcript_id = binding.transcript_id;
            challenge.nonce = binding.nonce;
            challenge.target_info = bytes_from_trivial_object(target_info);

            auto payload = serialise_canonical(challenge);
            if (!payload.has_value())
                return std::nullopt;

            cmw out;
            out.media_type = simulation_evidence_media_type;
            out.content_format = simulation_local_challenge_content_format;
            out.payload = std::move(payload.value());
            return out;
#endif
        }

        [[nodiscard]] auto parse_local_challenge(const cmw& challenge)
            -> std::optional<rpc::attestation::sgx_sim_local_attestation_challenge>
        {
            if (challenge.media_type != simulation_evidence_media_type)
                return std::nullopt;
            if (challenge.content_format != simulation_local_challenge_content_format)
                return std::nullopt;
            auto parsed = deserialise_canonical<rpc::attestation::sgx_sim_local_attestation_challenge>(challenge.payload);
            if (!parsed.has_value())
                return std::nullopt;
            if (parsed->backend_id != simulation_backend_id || parsed->transcript_id == 0 || parsed->nonce.empty()
                || parsed->target_info.empty())
            {
                return std::nullopt;
            }
            return parsed;
        }

        [[nodiscard]] auto make_local_report_evidence(
            const cmw& verifier_challenge,
            const evidence_binding& binding,
            std::string_view development_key) -> std::optional<cmw>
        {
#if !CANOPY_ATTESTATION_HAS_INTEL_SGX_SIM_REPORT
            (void)verifier_challenge;
            (void)binding;
            (void)development_key;
            return std::nullopt;
#else
            auto parsed_challenge = parse_local_challenge(verifier_challenge);
            if (!parsed_challenge.has_value())
                return std::nullopt;

            const auto& challenge = parsed_challenge.value();
            if (challenge.transcript_id != binding.transcript_id)
                return std::nullopt;
            if (!binding.nonce.empty() && binding.nonce != challenge.nonce)
                return std::nullopt;

            sgx_target_info_t target_info{};
            if (!trivial_object_from_bytes(challenge.target_info, target_info))
                return std::nullopt;

            rpc::attestation::sgx_sim_local_attestation_report evidence;
            evidence.binding = make_local_report_binding(binding, challenge);

            auto report_hash = make_local_report_data_hash(evidence.binding);
            if (!report_hash.has_value())
                return std::nullopt;
            evidence.report_data_sha256 = std::move(report_hash.value());

            sgx_report_data_t report_data{};
            std::copy(evidence.report_data_sha256.begin(), evidence.report_data_sha256.end(), report_data.d);

            sgx_report_t report{};
            if (sgx_create_report(&target_info, &report_data, &report) != SGX_SUCCESS)
                return std::nullopt;
            evidence.report = bytes_from_trivial_object(report);

            auto signature = sign_evidence(evidence, development_key);
            if (!signature.has_value())
                return std::nullopt;
            evidence.development_signature = std::move(signature.value());

            auto payload = serialise_canonical(evidence);
            if (!payload.has_value())
                return std::nullopt;

            cmw out;
            out.media_type = simulation_evidence_media_type;
            out.content_format = simulation_local_report_evidence_content_format;
            out.payload = std::move(payload.value());
            return out;
#endif
        }

        [[nodiscard]] auto make_report_evidence(
            const evidence_binding& binding,
            std::string_view development_key) -> std::optional<cmw>
        {
#if !CANOPY_ATTESTATION_HAS_INTEL_SGX_SIM_REPORT
            (void)binding;
            (void)development_key;
            return std::nullopt;
#else
            rpc::attestation::sgx_sim_report_evidence evidence;
            evidence.binding = make_binding(binding);

            auto report_hash = make_report_data_hash(evidence.binding);
            if (!report_hash.has_value())
                return std::nullopt;
            evidence.report_data_sha256 = std::move(report_hash.value());

            if (!append_sgx_sim_report(evidence))
                return std::nullopt;

            auto signature = sign_evidence(evidence, development_key);
            if (!signature.has_value())
                return std::nullopt;
            evidence.development_signature = std::move(signature.value());

            auto payload = serialise_canonical(evidence);
            if (!payload.has_value())
                return std::nullopt;

            cmw out;
            out.media_type = simulation_evidence_media_type;
            out.content_format = simulation_report_evidence_content_format;
            out.payload = std::move(payload.value());
            return out;
#endif
        }

        [[nodiscard]] auto verify_report_evidence(
            const cmw& evidence,
            const evidence_binding& expected_binding,
            const attestation_policy& policy,
            std::string_view development_key) -> attestation_verdict
        {
            if (evidence.media_type != simulation_evidence_media_type)
                return reject("unsupported SGX simulation evidence media type");
            if (evidence.content_format != simulation_report_evidence_content_format)
                return reject("unsupported SGX simulation evidence content format");
            if (auto policy_error = policy_accepts_simulation(policy); policy_error.has_value())
                return std::move(policy_error.value());

            auto parsed = deserialise_canonical<rpc::attestation::sgx_sim_report_evidence>(evidence.payload);
            if (!parsed.has_value())
                return reject("malformed SGX simulation report evidence");

            auto& value = parsed.value();
            if (value.binding.backend_id != simulation_backend_id)
                return reject("SGX simulation evidence backend id mismatch");
            auto parsed_identity = make_identity(value.binding.subject);
            if (parsed_identity.enclave_id != expected_binding.subject.enclave_id
                || parsed_identity.zone_id != expected_binding.subject.zone_id)
            {
                return reject("SGX simulation evidence identity mismatch");
            }
            if (value.binding.transcript_id != expected_binding.transcript_id)
                return reject("SGX simulation evidence transcript mismatch");
            if (value.binding.nonce != expected_binding.nonce)
                return reject("SGX simulation evidence nonce mismatch");
            if (value.report_verified_by_producer != sgx_sim_flag_false
                && value.report_verified_by_producer != sgx_sim_flag_true)
            {
                return reject("SGX simulation evidence verification flag is invalid");
            }

            auto expected_hash = make_report_data_hash(value.binding);
            if (!expected_hash.has_value() || value.report_data_sha256 != expected_hash.value())
                return reject("SGX simulation report_data binding mismatch");
            if (value.report.empty())
                return reject("SGX simulation report is missing");
            if (value.target_info.empty())
                return reject("SGX simulation target_info is missing");

            auto expected_signature = sign_evidence(value, development_key);
            if (!expected_signature.has_value()
                || !detail::constant_time_equal(value.development_signature, expected_signature.value()))
            {
                return reject("SGX simulation development signature mismatch");
            }

            bool locally_verified = false;
#if CANOPY_ATTESTATION_HAS_INTEL_SGX_SIM_REPORT
            locally_verified = locally_verify_sgx_sim_report(value, expected_hash.value());
#endif

            attestation_verdict verdict;
            verdict.accepted = true;
            verdict.reason
                = locally_verified ? std::string(sgx_sim_report_reason) : std::string(sgx_sim_development_reason);
            verdict.backend_id = simulation_backend_id;
            verdict.level = security_level::simulation;
            verdict.peer_identity = std::move(parsed_identity);
            return verdict;
        }

        [[nodiscard]] auto verify_local_report_evidence(
            const cmw& evidence,
            const cmw& verifier_challenge,
            const evidence_binding& expected_binding,
            const attestation_policy& policy,
            std::string_view development_key) -> attestation_verdict
        {
            if (evidence.media_type != simulation_evidence_media_type)
                return reject("unsupported SGX simulation local report media type");
            if (evidence.content_format != simulation_local_report_evidence_content_format)
                return reject("unsupported SGX simulation local report content format");
            if (auto policy_error = policy_accepts_simulation(policy); policy_error.has_value())
                return std::move(policy_error.value());

            auto challenge = parse_local_challenge(verifier_challenge);
            if (!challenge.has_value())
                return reject("malformed SGX simulation local-attestation challenge");

            auto parsed_report
                = deserialise_canonical<rpc::attestation::sgx_sim_local_attestation_report>(evidence.payload);
            if (!parsed_report.has_value())
                return reject("malformed SGX simulation local-attestation report");

            auto& report_evidence = parsed_report.value();
            if (report_evidence.binding.backend_id != simulation_backend_id)
                return reject("SGX simulation local report backend id mismatch");
            if (report_evidence.binding.transcript_id != expected_binding.transcript_id
                || report_evidence.binding.transcript_id != challenge->transcript_id)
            {
                return reject("SGX simulation local report transcript mismatch");
            }
            if (report_evidence.binding.challenge_nonce != challenge->nonce)
                return reject("SGX simulation local report challenge nonce mismatch");
            if (!expected_binding.nonce.empty() && expected_binding.nonce != challenge->nonce)
                return reject("SGX simulation local report expected nonce mismatch");
            if (report_evidence.binding.verifier != challenge->verifier)
                return reject("SGX simulation local report verifier mismatch");

            auto parsed_identity = make_identity(report_evidence.binding.claimant);
            if (parsed_identity.enclave_id != expected_binding.subject.enclave_id
                || parsed_identity.zone_id != expected_binding.subject.zone_id)
            {
                return reject("SGX simulation local report identity mismatch");
            }

            auto expected_hash = make_local_report_data_hash(report_evidence.binding);
            if (!expected_hash.has_value() || report_evidence.report_data_sha256 != expected_hash.value())
                return reject("SGX simulation local report_data binding mismatch");
            if (report_evidence.report.empty())
                return reject("SGX simulation local report is missing");

            auto expected_signature = sign_evidence(report_evidence, development_key);
            if (!expected_signature.has_value()
                || !detail::constant_time_equal(report_evidence.development_signature, expected_signature.value()))
            {
                return reject("SGX simulation local report development signature mismatch");
            }

#if CANOPY_ATTESTATION_HAS_INTEL_SGX_SIM_REPORT
            sgx_report_t report{};
            if (!trivial_object_from_bytes(report_evidence.report, report))
                return reject("SGX simulation local report blob is not an sgx_report_t");
            if (!report_data_matches_hash(report, expected_hash.value()))
                return reject("SGX simulation local report_data did not match expected transcript binding");
            if (sgx_verify_report(&report) != SGX_SUCCESS)
                return reject("SGX simulation local report verification failed");

            attestation_verdict verdict;
            verdict.accepted = true;
            verdict.reason = std::string(sgx_sim_local_report_reason);
            verdict.backend_id = simulation_backend_id;
            verdict.level = security_level::simulation;
            verdict.peer_identity = std::move(parsed_identity);
            return verdict;
#else
            return reject("SGX simulation local report verification is not available in this build");
#endif
        }

    } // namespace

    simulation_backend::simulation_backend(std::string development_key)
        : development_key_(std::move(development_key))
        , fallback_(make_simulation_fallback(development_key_))
    {
    }

    auto simulation_backend::backend_id() const -> std::string
    {
        return simulation_backend_id;
    }

    auto simulation_backend::level() const -> security_level
    {
        return security_level::simulation;
    }

    auto simulation_backend::produce_evidence(const evidence_binding& binding) const -> cmw
    {
        if (auto report_evidence = make_report_evidence(binding, development_key_); report_evidence.has_value())
            return std::move(report_evidence.value());
        return fallback_.produce_evidence(binding);
    }

    auto simulation_backend::verify_evidence(
        const cmw& evidence,
        const evidence_binding& expected_binding,
        const attestation_policy& policy) const -> attestation_verdict
    {
        if (evidence.content_format == simulation_report_evidence_content_format)
            return verify_report_evidence(evidence, expected_binding, policy, development_key_);
        if (evidence.content_format == simulation_local_report_evidence_content_format)
            return reject("SGX simulation local report evidence requires a verifier challenge");
        return fallback_.verify_evidence(evidence, expected_binding, policy);
    }

    auto simulation_backend::supports_verifier_challenge() const -> bool
    {
        return CANOPY_ATTESTATION_HAS_INTEL_SGX_SIM_REPORT != 0;
    }

    auto simulation_backend::make_verifier_challenge(const evidence_binding& binding) const -> std::optional<cmw>
    {
        return make_local_challenge(binding);
    }

    auto simulation_backend::produce_evidence_for_challenge(
        const cmw& verifier_challenge,
        const evidence_binding& binding) const -> std::optional<cmw>
    {
        return make_local_report_evidence(verifier_challenge, binding, development_key_);
    }

    auto simulation_backend::verify_evidence_for_challenge(
        const cmw& evidence,
        const cmw& verifier_challenge,
        const evidence_binding& expected_binding,
        const attestation_policy& policy) const -> attestation_verdict
    {
        if (evidence.content_format == simulation_local_report_evidence_content_format)
            return verify_local_report_evidence(evidence, verifier_challenge, expected_binding, policy, development_key_);
        return verify_evidence(evidence, expected_binding, policy);
    }
} // namespace canopy::security::attestation
