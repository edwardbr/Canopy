/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <attestation/sgx_sim_protocol.h>
#include <attestation_test/attestation_test.h>
#include <example/example.h>
#include <rpc/internal/serialiser.h>
#include <rpc/rpc.h>
#include <security/attestation/backends/simulation/simulation_backend.h>
#include <transports/sgx_coroutine/enclave/runtime.h>

#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Keep the self-test binding deterministic. This test is not proving
    // freshness; it is proving that the SGX SIM backend can create a report,
    // bind that report to Canopy transcript fields, and then verify the same
    // evidence inside an enclave.
    constexpr uint64_t test_transcript_id = 0x51584753494dULL;
    constexpr uint64_t test_local_attestation_transcript_base = 0x4c4f43414cULL;
    constexpr uint64_t expected_sha256_digest_size = 32;

    [[nodiscard]] auto make_binding() -> canopy::security::attestation::evidence_binding
    {
        // evidence_binding is the backend-neutral input used by the
        // attestation_service. The SGX SIM backend canonical_crypto serializes
        // this data and hashes it into sgx_report_data_t, so changing any of
        // these values must change the expected report_data digest.
        canopy::security::attestation::evidence_binding binding;
        binding.subject = canopy::security::attestation::identity{
            "sgx-sim-attestation-test-enclave", "sgx-sim-attestation-test-zone"};
        binding.transcript_id = test_transcript_id;
        binding.nonce = {0x43, 0x61, 0x6e, 0x6f, 0x70, 0x79, 0x2d, 0x53, 0x47, 0x58, 0x2d, 0x53, 0x49, 0x4d};
        return binding;
    }

    [[nodiscard]] auto make_policy() -> canopy::security::attestation::attestation_policy
    {
        // SGX SIM evidence is deliberately accepted only as development /
        // simulation evidence. The policy mirrors the production-facing
        // defaults for CANOPY_ATTESTATION_BACKEND=SGX_SIM without pretending
        // this is hardware remote attestation.
        canopy::security::attestation::attestation_policy policy;
        policy.required_backend_id = canopy::security::attestation::simulation_backend_id;
        policy.minimum_security_level = canopy::security::attestation::security_level::simulation;
        policy.allow_development_evidence = true;
        return policy;
    }

    [[nodiscard]] auto make_local_attestation_nonce(uint8_t challenge_seed) -> std::vector<uint8_t>
    {
        std::vector<uint8_t> nonce(canopy::security::attestation::attestation_nonce_size);
        for (size_t index = 0; index < nonce.size(); ++index)
            nonce[index] = static_cast<uint8_t>(challenge_seed + index);
        return nonce;
    }

    [[nodiscard]] auto make_local_binding(
        uint64_t transcript_id,
        std::vector<uint8_t> nonce) -> canopy::security::attestation::evidence_binding
    {
        canopy::security::attestation::evidence_binding binding;
        binding.subject = canopy::security::attestation::identity{
            "sgx-sim-attestation-test-enclave", "sgx-sim-attestation-test-zone"};
        binding.transcript_id = transcript_id;
        binding.nonce = std::move(nonce);
        return binding;
    }

    [[nodiscard]] auto to_wire_cmw(const canopy::security::attestation::cmw& value) -> attestation_test::sgx_sim_test_cmw
    {
        attestation_test::sgx_sim_test_cmw out;
        out.media_type = value.media_type;
        out.content_format = value.content_format;
        out.payload = value.payload;
        return out;
    }

    [[nodiscard]] auto from_wire_cmw(const attestation_test::sgx_sim_test_cmw& value) -> canopy::security::attestation::cmw
    {
        canopy::security::attestation::cmw out;
        out.media_type = value.media_type;
        out.content_format = value.content_format;
        out.payload = value.payload;
        return out;
    }

    [[nodiscard]] auto make_identity(const rpc::attestation_identity& value) -> canopy::security::attestation::identity
    {
        return canopy::security::attestation::identity{value.enclave_id, value.zone_id};
    }

    [[nodiscard]] auto parse_local_challenge(
        const attestation_test::sgx_sim_test_cmw& challenge,
        rpc::attestation::sgx_sim_local_attestation_challenge& out) -> bool
    {
        if (challenge.content_format != canopy::security::attestation::simulation_local_challenge_content_format)
            return false;
        return rpc::from_canonical_crypto(rpc::byte_span(challenge.payload), out).empty();
    }

    [[nodiscard]] auto parse_local_report(
        const attestation_test::sgx_sim_test_cmw& report,
        rpc::attestation::sgx_sim_local_attestation_report& out) -> bool
    {
        if (report.content_format != canopy::security::attestation::simulation_local_report_evidence_content_format)
            return false;
        return rpc::from_canonical_crypto(rpc::byte_span(report.payload), out).empty();
    }

    class attestation_enclave_test final
        : public rpc::base<attestation_enclave_test, attestation_test::i_attestation_enclave_test>,
          public rpc::enable_shared_from_this<attestation_enclave_test>
    {
    public:
        CORO_TASK(int)
        sgx_sim_report_self_test(attestation_test::sgx_sim_report_self_test_result& details) override
        {
            details = {};

#if defined(SGX_SIM) && !defined(CANOPY_FAKE_SGX) && defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM)                       \
    && defined(CANOPY_BUILD_CANONICAL_CRYPTO)
            // This code must execute inside an Intel SGX simulation enclave.
            // Host builds of simulation_backend intentionally fall back to
            // development evidence and would not exercise sgx_create_report.
            canopy::security::attestation::simulation_backend backend;
            const auto binding = make_binding();
            auto evidence = backend.produce_evidence(binding);

            // The first assertion is about backend selection: if the enclave
            // could not produce the SGX SIM report payload, it would fall back
            // to the development evidence shape and this test would fail.
            details.content_format = evidence.content_format;
            details.payload_size = evidence.payload.size();
            details.produced_report_evidence
                = evidence.content_format == canopy::security::attestation::simulation_report_evidence_content_format;
            if (!details.produced_report_evidence)
            {
                CO_RETURN rpc::error::ZONE_NOT_SUPPORTED();
            }

            // Decode the IDL-defined evidence so the host test can check
            // concrete SGX artefacts were emitted. This parse is separate from
            // backend.verify_evidence below; it gives the test visibility into
            // report size, target-info size, and producer-side verification.
            rpc::attestation::sgx_sim_report_evidence parsed;
            if (!rpc::from_canonical_crypto(rpc::byte_span(evidence.payload), parsed).empty())
            {
                CO_RETURN rpc::error::INVALID_DATA();
            }

            details.report_size = parsed.report.size();
            details.target_info_size = parsed.target_info.size();
            details.report_data_hash_size = parsed.report_data_sha256.size();
            details.development_signature_size = parsed.development_signature.size();
            details.producer_verified_report = parsed.report_verified_by_producer != 0;

            // Run the normal verifier path as well. This method is a backend
            // self-check, so the report is self-targeted and verified inside
            // the same enclave. The peer-targeted test methods below exercise
            // the SGX local-attestation target-info exchange between enclaves.
            auto verdict = backend.verify_evidence(evidence, binding, make_policy());
            details.verifier_accepted_report = verdict.accepted;
            details.verifier_reason = std::move(verdict.reason);

            // Return an RPC error if any mandatory proof component is absent.
            // The host test also asserts the returned details so failures are
            // visible at the field level, not just as a generic SECURITY_ERROR.
            if (!details.verifier_accepted_report || !details.producer_verified_report || details.report_size == 0
                || details.target_info_size == 0 || details.report_data_hash_size != expected_sha256_digest_size
                || details.development_signature_size == 0)
            {
                CO_RETURN rpc::error::SECURITY_ERROR();
            }

            CO_RETURN rpc::error::OK();
#else
            // The dedicated host test skips unless CANOPY_ATTESTATION_BACKEND
            // selects SGX_SIM. Keep the enclave method available in other test
            // builds so the generated interface remains buildable everywhere.
            CO_RETURN rpc::error::NOT_IMPLEMENTED();
#endif
        }

        CORO_TASK(int)
        sgx_sim_make_local_attestation_challenge(
            uint8_t challenge_seed,
            attestation_test::sgx_sim_test_cmw& challenge) override
        {
            challenge = {};

#if defined(SGX_SIM) && !defined(CANOPY_FAKE_SGX) && defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM)                       \
    && defined(CANOPY_BUILD_CANONICAL_CRYPTO)
            canopy::security::attestation::simulation_backend backend;
            const auto transcript_id = test_local_attestation_transcript_base + challenge_seed;
            auto cmw = backend.make_verifier_challenge(
                make_local_binding(transcript_id, make_local_attestation_nonce(challenge_seed)));
            if (!cmw.has_value())
                CO_RETURN rpc::error::ZONE_NOT_SUPPORTED();
            challenge = to_wire_cmw(cmw.value());
            CO_RETURN rpc::error::OK();
#else
            CO_RETURN rpc::error::NOT_IMPLEMENTED();
#endif
        }

        CORO_TASK(int)
        sgx_sim_make_local_attestation_report(
            const attestation_test::sgx_sim_test_cmw& challenge,
            attestation_test::sgx_sim_test_cmw& report_message) override
        {
            report_message = {};

#if defined(SGX_SIM) && !defined(CANOPY_FAKE_SGX) && defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM)                       \
    && defined(CANOPY_BUILD_CANONICAL_CRYPTO)
            rpc::attestation::sgx_sim_local_attestation_challenge parsed_challenge;
            if (!parse_local_challenge(challenge, parsed_challenge))
                CO_RETURN rpc::error::INVALID_DATA();

            canopy::security::attestation::simulation_backend backend;
            auto cmw = backend.produce_evidence_for_challenge(
                from_wire_cmw(challenge), make_local_binding(parsed_challenge.transcript_id, parsed_challenge.nonce));
            if (!cmw.has_value())
                CO_RETURN rpc::error::ZONE_NOT_SUPPORTED();
            report_message = to_wire_cmw(cmw.value());
            CO_RETURN rpc::error::OK();
#else
            CO_RETURN rpc::error::NOT_IMPLEMENTED();
#endif
        }

        CORO_TASK(int)
        sgx_sim_verify_local_attestation_report(
            const attestation_test::sgx_sim_test_cmw& report_message,
            const attestation_test::sgx_sim_test_cmw& challenge,
            attestation_test::sgx_sim_local_attestation_verification& verification) override
        {
            verification = {};

#if defined(SGX_SIM) && !defined(CANOPY_FAKE_SGX) && defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM)                       \
    && defined(CANOPY_BUILD_CANONICAL_CRYPTO)
            rpc::attestation::sgx_sim_local_attestation_challenge parsed_challenge;
            rpc::attestation::sgx_sim_local_attestation_report parsed_report;
            if (!parse_local_challenge(challenge, parsed_challenge) || !parse_local_report(report_message, parsed_report))
            {
                verification.failure_reason = "malformed SGX SIM local-attestation CMW";
                CO_RETURN rpc::error::INVALID_DATA();
            }
            verification.report_size = parsed_report.report.size();
            verification.expected_report_data_size = parsed_report.report_data_sha256.size();

            canopy::security::attestation::simulation_backend backend;
            auto binding = make_local_binding(parsed_report.binding.transcript_id, parsed_challenge.nonce);
            binding.subject = make_identity(parsed_report.binding.claimant);

            auto verdict = backend.verify_evidence_for_challenge(
                from_wire_cmw(report_message), from_wire_cmw(challenge), binding, make_policy());
            verification.accepted = verdict.accepted;
            verification.report_data_matched = verdict.accepted;
            verification.sgx_verify_report_succeeded = verdict.accepted;
            verification.failure_reason = std::move(verdict.reason);
            if (!verification.accepted)
            {
                CO_RETURN rpc::error::OK();
            }
            CO_RETURN rpc::error::OK();
#else
            verification.failure_reason = "SGX SIM local attestation is not available in this build";
            CO_RETURN rpc::error::NOT_IMPLEMENTED();
#endif
        }
    };

    struct connection_factory_registrar
    {
        connection_factory_registrar()
        {
            // The SGX coroutine host transport discovers an enclave entry point
            // by asking the enclave runtime for this registered factory. Keeping
            // the attestation test in its own factory/enclave avoids adding
            // attestation-only methods to unrelated io_uring or RPC tests.
            rpc::sgx::coro::enclave::register_connection_factory<yyy::i_host, attestation_test::i_attestation_enclave_test>(
                "sgx_attestation_test_enclave",
                [](rpc::shared_ptr<yyy::i_host> host, std::shared_ptr<rpc::service> child_service)
                    -> CORO_TASK(rpc::service_connect_result<attestation_test::i_attestation_enclave_test>)
                {
                    (void)host;
                    (void)child_service;
                    try
                    {
                        rpc::shared_ptr<attestation_test::i_attestation_enclave_test> test(new attestation_enclave_test());
                        CO_RETURN rpc::service_connect_result<attestation_test::i_attestation_enclave_test>{
                            rpc::error::OK(), std::move(test)};
                    }
                    catch (const std::bad_alloc&)
                    {
                        CO_RETURN rpc::service_connect_result<attestation_test::i_attestation_enclave_test>{
                            rpc::error::OUT_OF_MEMORY(), {}};
                    }
                    catch (...)
                    {
                        CO_RETURN rpc::service_connect_result<attestation_test::i_attestation_enclave_test>{
                            rpc::error::EXCEPTION(), {}};
                    }
                });
        }
    };

    connection_factory_registrar g_connection_factory_registrar;
}
