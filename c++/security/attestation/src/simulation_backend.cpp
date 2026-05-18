/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/simulation_backend.h>

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

        [[nodiscard]] auto sign_evidence(
            const rpc::attestation::sgx_sim_report_evidence& evidence,
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

#if CANOPY_ATTESTATION_HAS_INTEL_SGX_SIM_REPORT
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

            // This is a self-targeted report because the current generic
            // handshake has no peer target-info exchange. Verifying it here
            // proves the SIM report path works in this enclave, but it is not
            // a substitute for peer-targeted local attestation.
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
        return fallback_.verify_evidence(evidence, expected_binding, policy);
    }
} // namespace canopy::security::attestation
