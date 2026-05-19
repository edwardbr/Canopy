/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <security/attestation/aead.h>
#include <security/attestation/backend_factory.h>
#include <security/attestation/backends/fake/fake_backend.h>
#include <security/attestation/backends/null/null_backend.h>
#include <security/attestation/backends/sgx_dcap/sgx_dcap_backend.h>
#include <security/attestation/backends/sgx_dcap/sgx_dcap_host_quote_provider.h>
#include <security/attestation/backends/sgx_dcap/sgx_dcap_host_quote_verifier.h>
#include <security/attestation/backends/sgx_dcap/sgx_dcap_quote3_parser.h>
#include <security/attestation/backends/sgx_epid/sgx_epid_backend.h>
#include <security/attestation/backends/sgx_epid/sgx_epid_host_quote_provider.h>
#include <security/attestation/backends/sgx_epid/sgx_epid_ias_report_verifier.h>
#include <security/attestation/backends/simulation/simulation_backend.h>
#include <security/attestation/protected_rpc.h>
#include <attestation/sgx_dcap_protocol.h>
#include <attestation/sgx_epid_protocol.h>
#include <attestation/route_attestation_protocol.h>
#include <rpc/internal/serialiser.h>
#include <security/attestation/service.h>
#include <security/attestation/zone_security_policy.h>

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using canopy::security::attestation::aead_aes_256_gcm_tag_size;
    using canopy::security::attestation::attestation_policy;
    using canopy::security::attestation::attestation_service;
    using canopy::security::attestation::attestation_service_options;
    using canopy::security::attestation::attestation_verdict;
    using canopy::security::attestation::backend_factory_overrides;
    using canopy::security::attestation::cmw;
    using canopy::security::attestation::configured_attestation_backend_kind;
    using canopy::security::attestation::configured_attestation_backend_name;
    using canopy::security::attestation::configured_backend_kind;
    using canopy::security::attestation::decrypt_aes_256_gcm;
    using canopy::security::attestation::encrypt_aes_256_gcm;
    using canopy::security::attestation::encrypted_payload_type_id;
    using canopy::security::attestation::establish_session_params;
    using canopy::security::attestation::evaluate_missing_peer_evidence_policy;
    using canopy::security::attestation::evaluate_reference_route_policy;
    using canopy::security::attestation::evaluate_route_attestation_state;
    using canopy::security::attestation::evidence_binding;
    using canopy::security::attestation::extract_sgx_quote3_report_data;
    using canopy::security::attestation::fake_backend;
    using canopy::security::attestation::fake_backend_id;
    using canopy::security::attestation::fake_evidence_content_format;
    using canopy::security::attestation::fake_evidence_media_type;
    using canopy::security::attestation::identity;
    using canopy::security::attestation::make_attestation_backend;
    using canopy::security::attestation::make_attestation_nonce;
    using canopy::security::attestation::make_configured_attestation_backend;
    using canopy::security::attestation::make_configured_attestation_service_options;
    using canopy::security::attestation::make_session_id;
    using canopy::security::attestation::null_backend;
    using canopy::security::attestation::null_backend_id;
    using canopy::security::attestation::protect_add_ref_request;
    using canopy::security::attestation::protect_object_released_request;
    using canopy::security::attestation::protect_post_request;
    using canopy::security::attestation::protect_release_request;
    using canopy::security::attestation::protect_send_request;
    using canopy::security::attestation::protect_send_response;
    using canopy::security::attestation::protect_transport_down_request;
    using canopy::security::attestation::protect_try_cast_request;
    using canopy::security::attestation::protected_key_scope;
    using canopy::security::attestation::protected_rpc_direction;
    using canopy::security::attestation::reference_route_policy_input;
    using canopy::security::attestation::route_attestation_action;
    using canopy::security::attestation::route_attestation_state;
    using canopy::security::attestation::route_attestation_status;
    using canopy::security::attestation::security_context;
    using canopy::security::attestation::security_level;
    using canopy::security::attestation::sgx_dcap_backend;
    using canopy::security::attestation::sgx_dcap_backend_id;
    using canopy::security::attestation::sgx_dcap_delegated_verification_result_check;
    using canopy::security::attestation::sgx_dcap_delegated_verification_result_mode;
    using canopy::security::attestation::sgx_dcap_evidence_media_type;
    using canopy::security::attestation::sgx_dcap_get_quote_request;
    using canopy::security::attestation::sgx_dcap_host_quote_provider;
    using canopy::security::attestation::sgx_dcap_host_quote_provider_functions;
    using canopy::security::attestation::sgx_dcap_host_quote_provider_options;
    using canopy::security::attestation::sgx_dcap_host_quote_verification;
    using canopy::security::attestation::sgx_dcap_host_quote_verifier;
    using canopy::security::attestation::sgx_dcap_host_quote_verifier_options;
    using canopy::security::attestation::sgx_dcap_quote3_report_data_offset;
    using canopy::security::attestation::sgx_dcap_quote3_report_data_size;
    using canopy::security::attestation::sgx_dcap_quote3_signature_data_len_offset;
    using canopy::security::attestation::sgx_dcap_quote3_signature_data_offset;
    using canopy::security::attestation::sgx_dcap_quote_evidence_content_format;
    using canopy::security::attestation::sgx_dcap_quote_material;
    using canopy::security::attestation::sgx_dcap_quote_provider;
    using canopy::security::attestation::sgx_dcap_quote_request;
    using canopy::security::attestation::sgx_dcap_quote_verifier;
    using canopy::security::attestation::sgx_dcap_report_request;
    using canopy::security::attestation::sgx_dcap_target_info_result;
    using canopy::security::attestation::sgx_dcap_unavailable_content_format;
    using canopy::security::attestation::sgx_dcap_verification_result_material;
    using canopy::security::attestation::sgx_dcap_verifier_input;
    using canopy::security::attestation::sgx_epid_backend;
    using canopy::security::attestation::sgx_epid_backend_id;
    using canopy::security::attestation::sgx_epid_evidence_media_type;
    using canopy::security::attestation::sgx_epid_get_quote_request;
    using canopy::security::attestation::sgx_epid_host_quote_provider;
    using canopy::security::attestation::sgx_epid_host_quote_provider_functions;
    using canopy::security::attestation::sgx_epid_host_quote_provider_options;
    using canopy::security::attestation::sgx_epid_ias_report_material;
    using canopy::security::attestation::sgx_epid_ias_report_validation_result;
    using canopy::security::attestation::sgx_epid_ias_report_verifier;
    using canopy::security::attestation::sgx_epid_ias_report_verifier_options;
    using canopy::security::attestation::sgx_epid_init_quote_result;
    using canopy::security::attestation::sgx_epid_quote_evidence_content_format;
    using canopy::security::attestation::sgx_epid_quote_material;
    using canopy::security::attestation::sgx_epid_quote_provider;
    using canopy::security::attestation::sgx_epid_quote_request;
    using canopy::security::attestation::sgx_epid_quote_verifier;
    using canopy::security::attestation::sgx_epid_report_request;
    using canopy::security::attestation::sgx_epid_unavailable_content_format;
    using canopy::security::attestation::sgx_epid_verifier_input;
    using canopy::security::attestation::simulation_backend;
    using canopy::security::attestation::simulation_backend_id;
    using canopy::security::attestation::simulation_evidence_content_format;
    using canopy::security::attestation::simulation_evidence_media_type;
    using canopy::security::attestation::simulation_report_evidence_content_format;
    using canopy::security::attestation::unprotect_add_ref_request;
    using canopy::security::attestation::unprotect_object_released_request;
    using canopy::security::attestation::unprotect_post_request;
    using canopy::security::attestation::unprotect_release_request;
    using canopy::security::attestation::unprotect_send_request;
    using canopy::security::attestation::unprotect_send_response;
    using canopy::security::attestation::unprotect_transport_down_request;
    using canopy::security::attestation::unprotect_try_cast_request;
    using canopy::security::attestation::zone_security_policy;
    using canopy::security::attestation::zone_security_policy_options;

    auto make_service(
        std::string enclave_id,
        std::string zone_id,
        uint64_t max_counter_value = std::numeric_limits<uint64_t>::max()) -> std::shared_ptr<attestation_service>
    {
        attestation_service_options options;
        options.local_identity = identity{std::move(enclave_id), std::move(zone_id)};
        options.backend = std::make_shared<fake_backend>();
        options.policy = attestation_policy{};
        options.policy.required_backend_id = fake_backend_id;
        options.policy.minimum_security_level = security_level::development;
        options.max_counter_value = max_counter_value;
        return std::make_shared<attestation_service>(std::move(options));
    }

    auto establish(
        const std::shared_ptr<attestation_service>& service,
        identity peer,
        uint64_t transcript_id = 99,
        std::vector<uint8_t> shared_secret = {}) -> security_context
    {
        establish_session_params params;
        params.peer_identity = std::move(peer);
        params.transcript_id = transcript_id;
        params.local_evidence_sent = true;
        params.peer_attested = true;
        params.verified_backend_id = fake_backend_id;
        params.verified_level = security_level::development;
        params.shared_secret = std::move(shared_secret);
        return service->establish_session(params);
    }

    auto make_scope(
        const security_context& context,
        identity caller,
        identity destination,
        protected_rpc_direction direction = protected_rpc_direction::caller_to_destination) -> protected_key_scope
    {
        protected_key_scope scope;
        scope.session_id = context.session_id;
        scope.caller_identity = std::move(caller);
        scope.destination_identity = std::move(destination);
        scope.direction = direction;
        return scope;
    }

    auto make_zone(uint64_t subnet) -> rpc::zone
    {
        auto addr = rpc::DEFAULT_PREFIX;
        auto set_result = addr.set_subnet(subnet);
        EXPECT_TRUE(set_result.has_value());
        return rpc::zone(addr);
    }

    auto agreed_payload_encodings() -> std::vector<rpc::encoding>
    {
        std::vector<rpc::encoding> encodings{rpc::encoding::yas_binary};
#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
        encodings.push_back(rpc::encoding::protocol_buffers);
#endif
#ifdef CANOPY_BUILD_NANOPB
        encodings.push_back(rpc::encoding::nanopb);
#endif
        return encodings;
    }

    auto make_identity_payload(
        rpc::encoding encoding,
        std::string zone_id) -> std::vector<char>
    {
        rpc::attestation_identity payload;
        payload.enclave_id = "payload-enclave";
        payload.zone_id = std::move(zone_id);
        return rpc::serialise<std::vector<char>>(payload, encoding);
    }

    void expect_identity_payload(
        const std::vector<char>& payload,
        rpc::encoding encoding,
        const std::string& expected_zone_id)
    {
        rpc::attestation_identity decoded;
        EXPECT_TRUE(rpc::deserialise(encoding, rpc::byte_span(payload), decoded).empty());
        EXPECT_EQ(decoded.enclave_id, "payload-enclave");
        EXPECT_EQ(decoded.zone_id, expected_zone_id);
    }

    void expect_identity_typed_payload(
        const std::optional<rpc::typed_payload>& payload,
        uint64_t type_id,
        rpc::encoding encoding,
        const std::string& expected_zone_id)
    {
        ASSERT_TRUE(payload.has_value());
        EXPECT_EQ(payload->get_type_id(), type_id);
        EXPECT_EQ(payload->get_encoding(), encoding);
        expect_identity_payload(payload->get_payload(), encoding, expected_zone_id);
    }

    auto make_raw_payload(
        uint64_t type_id,
        rpc::encoding encoding,
        std::vector<char> payload) -> rpc::typed_payload
    {
        return rpc::typed_payload(type_id, encoding, std::move(payload));
    }

    void expect_payload(
        const std::optional<rpc::typed_payload>& payload,
        uint64_t type_id,
        rpc::encoding encoding,
        const std::vector<char>& bytes)
    {
        ASSERT_TRUE(payload.has_value());
        EXPECT_EQ(payload->get_type_id(), type_id);
        EXPECT_EQ(payload->get_encoding(), encoding);
        EXPECT_EQ(payload->get_payload(), bytes);
    }

    void expect_protected_payload(
        const std::optional<rpc::typed_payload>& payload,
        rpc::encoding encoding)
    {
        ASSERT_TRUE(payload.has_value());
        EXPECT_EQ(payload->get_type_id(), encrypted_payload_type_id(rpc::get_version()));
        EXPECT_EQ(payload->get_encoding(), encoding);
        EXPECT_FALSE(payload->get_payload().empty());
    }

    template<typename T> auto decode_canonical_payload(const cmw& evidence) -> std::optional<T>
    {
        T decoded;
        if (!rpc::from_canonical_crypto(rpc::byte_span(evidence.payload), decoded).empty())
            return std::nullopt;
        return decoded;
    }

    template<typename T> auto encode_canonical_payload(const T& value) -> std::optional<std::vector<uint8_t>>
    {
        return rpc::to_canonical_crypto<std::vector<uint8_t>>(value);
    }

    auto make_sgx_report_data(const std::vector<uint8_t>& report_data_sha256) -> std::vector<uint8_t>
    {
        auto out = report_data_sha256;
        out.resize(64U, 0);
        return out;
    }

    auto make_sgx_quote3_quote(
        const std::vector<uint8_t>& report_data,
        uint32_t signature_data_size = 0U) -> std::vector<uint8_t>
    {
        auto quote = std::vector<uint8_t>(sgx_dcap_quote3_signature_data_offset + signature_data_size, 0);
        EXPECT_EQ(report_data.size(), sgx_dcap_quote3_report_data_size);
        std::copy(report_data.begin(), report_data.end(), quote.begin() + sgx_dcap_quote3_report_data_offset);
        quote[sgx_dcap_quote3_signature_data_len_offset] = static_cast<uint8_t>(signature_data_size & 0xffU);
        quote[sgx_dcap_quote3_signature_data_len_offset + 1U] = static_cast<uint8_t>((signature_data_size >> 8U) & 0xffU);
        quote[sgx_dcap_quote3_signature_data_len_offset + 2U] = static_cast<uint8_t>((signature_data_size >> 16U) & 0xffU);
        quote[sgx_dcap_quote3_signature_data_len_offset + 3U] = static_cast<uint8_t>((signature_data_size >> 24U) & 0xffU);
        return quote;
    }

    auto extract_sgx_report_data_prefix(const std::vector<uint8_t>& quote) -> std::optional<std::vector<uint8_t>>
    {
        if (quote.size() < 64U)
            return std::nullopt;
        return std::vector<uint8_t>(quote.begin(), quote.begin() + 64U);
    }

    class test_sgx_epid_quote_provider final : public sgx_epid_quote_provider
    {
    public:
        bool include_ias_report{false};
        size_t sig_rl_size{3};
        size_t quote_padding_size{1};

        [[nodiscard]] auto produce_quote(const sgx_epid_quote_request& request) const
            -> std::optional<sgx_epid_quote_material> override
        {
            if (request.binding.transcript_id == 0 || request.report_data_sha256.size() != 32)
                return std::nullopt;

            sgx_epid_quote_material quote;
            quote.extended_epid_group_id = 0x12345678U;
            quote.quote_sign_type = 0;
            quote.spid = std::vector<uint8_t>(16, 0x5a);
            quote.sig_rl = std::vector<uint8_t>(sig_rl_size, 0x17);
            quote.quote = request.report_data_sha256;
            quote.quote.insert(quote.quote.end(), quote_padding_size, 0x42);
            if (include_ias_report)
            {
                sgx_epid_ias_report_material report;
                report.body_json = R"({"isvEnclaveQuoteStatus":"OK"})";
                report.signature = {0x10, 0x20, 0x30};
                report.signing_certificate_chain = {0x40, 0x50, 0x60};
                report.quote_status = "OK";
                report.advisory_ids = "";
                quote.ias_report = report;
            }
            return quote;
        }
    };

    class test_sgx_epid_quote_verifier final : public sgx_epid_quote_verifier
    {
    public:
        [[nodiscard]] auto verify_quote(
            const sgx_epid_verifier_input& input,
            const attestation_policy&) const -> attestation_verdict override
        {
            attestation_verdict verdict;
            if (input.report_data_sha256.size() != 32 || input.quote.quote.empty()
                || input.quote.extended_epid_group_id != 0x12345678U)
            {
                verdict.reason = "test EPID quote was malformed";
                return verdict;
            }

            verdict.accepted = true;
            verdict.reason = "test EPID quote accepted";
            verdict.backend_id = sgx_epid_backend_id;
            verdict.level = security_level::hardware_legacy;
            verdict.peer_identity = input.expected_binding.subject;
            return verdict;
        }
    };

    class strict_test_sgx_epid_quote_verifier final : public sgx_epid_quote_verifier
    {
    public:
        [[nodiscard]] auto verify_quote(
            const sgx_epid_verifier_input& input,
            const attestation_policy&) const -> attestation_verdict override
        {
            attestation_verdict verdict;
            if (input.report_data_sha256.size() != 32 || input.quote.quote.size() < 33)
            {
                verdict.reason = "test EPID quote report_data was malformed";
                return verdict;
            }
            if (!input.quote.ias_report.has_value())
            {
                verdict.reason = "test EPID IAS report is missing";
                return verdict;
            }
            const auto& report = input.quote.ias_report.value();
            if (report.body_json.empty() || report.signature.empty() || report.signing_certificate_chain.empty()
                || report.quote_status != "OK")
            {
                verdict.reason = "test EPID IAS appraisal was incomplete";
                return verdict;
            }

            verdict.accepted = true;
            verdict.reason = "test EPID quote accepted with IAS report";
            verdict.backend_id = sgx_epid_backend_id;
            verdict.level = security_level::hardware_legacy;
            verdict.peer_identity = input.expected_binding.subject;
            return verdict;
        }
    };

    class counting_sgx_epid_quote_verifier final : public sgx_epid_quote_verifier
    {
    public:
        mutable int calls{0};

        [[nodiscard]] auto verify_quote(
            const sgx_epid_verifier_input&,
            const attestation_policy&) const -> attestation_verdict override
        {
            ++calls;
            attestation_verdict verdict;
            verdict.accepted = true;
            verdict.backend_id = sgx_epid_backend_id;
            verdict.level = security_level::hardware_legacy;
            return verdict;
        }
    };

    class test_sgx_dcap_quote_provider final : public sgx_dcap_quote_provider
    {
    public:
        size_t quote_padding_size{1};
        size_t supplemental_data_size{0};

        [[nodiscard]] auto produce_quote(const sgx_dcap_quote_request& request) const
            -> std::optional<sgx_dcap_quote_material> override
        {
            if (request.binding.transcript_id == 0 || request.report_data_sha256.size() != 32)
                return std::nullopt;

            sgx_dcap_quote_material quote;
            quote.quote = request.report_data_sha256;
            quote.quote.insert(quote.quote.end(), quote_padding_size, 0xdc);

            sgx_dcap_verification_result_material result;
            result.quote_verification_result = 0;
            result.quote_verification_result_name = "OK";
            result.verification_time = 123456U;
            result.supplemental_data = std::vector<uint8_t>(supplemental_data_size, 0x44);
            quote.verification_result = result;
            return quote;
        }
    };

    class test_sgx_dcap_quote_verifier final : public sgx_dcap_quote_verifier
    {
    public:
        [[nodiscard]] auto verify_quote(
            const sgx_dcap_verifier_input& input,
            const attestation_policy&) const -> attestation_verdict override
        {
            attestation_verdict verdict;
            if (input.report_data_sha256.size() != 32 || input.quote.quote.size() != 33 || input.quote.quote.back() != 0xdc)
            {
                verdict.reason = "test DCAP quote was malformed";
                return verdict;
            }
            if (!input.quote.verification_result.has_value()
                || input.quote.verification_result->quote_verification_result_name != "OK")
            {
                verdict.reason = "test DCAP verification result was missing";
                return verdict;
            }

            verdict.accepted = true;
            verdict.reason = "test DCAP quote accepted";
            verdict.backend_id = sgx_dcap_backend_id;
            verdict.level = security_level::hardware;
            verdict.peer_identity = input.expected_binding.subject;
            return verdict;
        }
    };

    class counting_sgx_dcap_quote_verifier final : public sgx_dcap_quote_verifier
    {
    public:
        mutable int calls{0};

        [[nodiscard]] auto verify_quote(
            const sgx_dcap_verifier_input&,
            const attestation_policy&) const -> attestation_verdict override
        {
            ++calls;
            attestation_verdict verdict;
            verdict.accepted = true;
            verdict.backend_id = sgx_dcap_backend_id;
            verdict.level = security_level::hardware;
            return verdict;
        }
    };
} // namespace

TEST(
    AttestationService,
    FakeBackendRejectsMalformedEvidenceSafely)
{
    fake_backend backend;
    evidence_binding expected_binding;
    expected_binding.subject = identity{"enclave-a", "zone-a"};
    expected_binding.transcript_id = 17;
    expected_binding.nonce = {1, 2, 3, 4};

    attestation_policy policy;
    policy.required_backend_id = fake_backend_id;
    policy.minimum_security_level = security_level::development;

    auto valid = backend.produce_evidence(expected_binding);
    EXPECT_TRUE(backend.verify_evidence(valid, expected_binding, policy).accepted);

    auto wrong_nonce_binding = expected_binding;
    wrong_nonce_binding.nonce.push_back(5);
    EXPECT_FALSE(backend.verify_evidence(valid, wrong_nonce_binding, policy).accepted);

    auto wrong_format = valid;
    wrong_format.content_format = "canopy.fake.unknown";
    EXPECT_FALSE(backend.verify_evidence(wrong_format, expected_binding, policy).accepted);

    cmw truncated;
    truncated.media_type = fake_evidence_media_type;
    truncated.content_format = fake_evidence_content_format;
    truncated.payload = {4, 0, 0, 0, 'f'};
    EXPECT_FALSE(backend.verify_evidence(truncated, expected_binding, policy).accepted);

    cmw oversized;
    oversized.media_type = fake_evidence_media_type;
    oversized.content_format = fake_evidence_content_format;
    oversized.payload = {0xff, 0xff, 0xff, 0xff};
    EXPECT_FALSE(backend.verify_evidence(oversized, expected_binding, policy).accepted);
}

TEST(
    AttestationService,
    NullBackendDoesNotCreateAttestedEvidence)
{
    attestation_service_options options;
    options.local_identity = identity{"client-process", "client-zone"};
    options.backend = std::make_shared<null_backend>();
    options.policy = attestation_policy{};
    options.policy.send_local_evidence = true;
    options.policy.require_peer_evidence = false;
    options.policy.allow_unattested_peer = true;
    options.policy.minimum_security_level = security_level::none;
    options.policy.required_backend_id = null_backend_id;

    attestation_service service(std::move(options));
    auto evidence = service.produce_evidence(11, {1, 2, 3, 4});
    EXPECT_FALSE(evidence.accepted);
    EXPECT_EQ(service.backend_id(), null_backend_id);
    EXPECT_EQ(service.backend_level(), security_level::none);
    EXPECT_FALSE(service.requires_peer_evidence());
    EXPECT_TRUE(service.allows_unattested_peer());
}

TEST(
    AttestationService,
    SgxSimulationProfileBackendIsNotHardwareEvidence)
{
    simulation_backend backend;
    evidence_binding expected_binding;
    expected_binding.subject = identity{"sim-enclave", "sim-zone"};
    expected_binding.transcript_id = 23;
    expected_binding.nonce = {9, 8, 7, 6};

    auto evidence = backend.produce_evidence(expected_binding);
    EXPECT_EQ(backend.backend_id(), simulation_backend_id);
    EXPECT_EQ(backend.level(), security_level::simulation);
    EXPECT_EQ(evidence.media_type, simulation_evidence_media_type);
    EXPECT_EQ(evidence.content_format, simulation_evidence_content_format);
    EXPECT_FALSE(evidence.payload.empty());

    attestation_policy policy;
    policy.required_backend_id = simulation_backend_id;
    policy.minimum_security_level = security_level::simulation;
    policy.allow_development_evidence = true;

    auto verdict = backend.verify_evidence(evidence, expected_binding, policy);
    EXPECT_TRUE(verdict.accepted) << verdict.reason;
    EXPECT_EQ(verdict.backend_id, simulation_backend_id);
    EXPECT_EQ(verdict.level, security_level::simulation);
    EXPECT_EQ(verdict.peer_identity.enclave_id, expected_binding.subject.enclave_id);
    EXPECT_EQ(verdict.peer_identity.zone_id, expected_binding.subject.zone_id);

    auto production_policy = policy;
    production_policy.allow_development_evidence = false;
    EXPECT_FALSE(backend.verify_evidence(evidence, expected_binding, production_policy).accepted);

    auto hardware_policy = policy;
    hardware_policy.minimum_security_level = security_level::hardware;
    EXPECT_FALSE(backend.verify_evidence(evidence, expected_binding, hardware_policy).accepted);
}

TEST(
    AttestationService,
    RejectsMalformedSgxSimulationReportEvidence)
{
    simulation_backend backend;

    evidence_binding expected_binding;
    expected_binding.subject = identity{"sim-enclave", "sim-zone"};
    expected_binding.transcript_id = 24;
    expected_binding.nonce = {1, 3, 5, 7};

    cmw malformed;
    malformed.media_type = simulation_evidence_media_type;
    malformed.content_format = simulation_report_evidence_content_format;
    malformed.payload = {1, 2, 3};

    attestation_policy policy;
    policy.required_backend_id = simulation_backend_id;
    policy.minimum_security_level = security_level::simulation;
    policy.allow_development_evidence = true;

    auto verdict = backend.verify_evidence(malformed, expected_binding, policy);
    EXPECT_FALSE(verdict.accepted);
}

TEST(
    AttestationService,
    SgxEpidBackendWithoutQuoteProviderFailsClosed)
{
    sgx_epid_backend backend;

    evidence_binding expected_binding;
    expected_binding.subject = identity{"epid-enclave", "epid-zone"};
    expected_binding.transcript_id = 31;
    expected_binding.nonce = {2, 4, 6, 8};

    auto evidence = backend.produce_evidence(expected_binding);
    EXPECT_EQ(backend.backend_id(), sgx_epid_backend_id);
    EXPECT_EQ(backend.level(), security_level::hardware_legacy);
    EXPECT_EQ(evidence.media_type, sgx_epid_evidence_media_type);
    EXPECT_EQ(evidence.content_format, sgx_epid_unavailable_content_format);

    attestation_policy policy;
    policy.required_backend_id = sgx_epid_backend_id;
    policy.minimum_security_level = security_level::hardware_legacy;
    policy.allow_development_evidence = false;

    auto verdict = backend.verify_evidence(evidence, expected_binding, policy);
    EXPECT_FALSE(verdict.accepted);
}

TEST(
    AttestationService,
    SgxEpidBackendUsesInjectedQuoteProviderAndVerifier)
{
    auto provider = std::make_shared<test_sgx_epid_quote_provider>();
    auto verifier = std::make_shared<test_sgx_epid_quote_verifier>();
    sgx_epid_backend backend(std::move(provider), std::move(verifier));

    evidence_binding expected_binding;
    expected_binding.subject = identity{"epid-enclave", "epid-zone"};
    expected_binding.transcript_id = 32;
    expected_binding.nonce = {3, 5, 7, 9};

    auto evidence = backend.produce_evidence(expected_binding);
    EXPECT_EQ(evidence.media_type, sgx_epid_evidence_media_type);
    EXPECT_EQ(evidence.content_format, sgx_epid_quote_evidence_content_format);
    EXPECT_FALSE(evidence.payload.empty());

    attestation_policy policy;
    policy.required_backend_id = sgx_epid_backend_id;
    policy.minimum_security_level = security_level::hardware_legacy;
    policy.allow_development_evidence = false;

    auto verdict = backend.verify_evidence(evidence, expected_binding, policy);
    EXPECT_TRUE(verdict.accepted) << verdict.reason;
    EXPECT_EQ(verdict.backend_id, sgx_epid_backend_id);
    EXPECT_EQ(verdict.level, security_level::hardware_legacy);
    EXPECT_EQ(verdict.peer_identity.enclave_id, expected_binding.subject.enclave_id);
    EXPECT_EQ(verdict.peer_identity.zone_id, expected_binding.subject.zone_id);

    auto wrong_nonce_binding = expected_binding;
    wrong_nonce_binding.nonce.push_back(11);
    EXPECT_FALSE(backend.verify_evidence(evidence, wrong_nonce_binding, policy).accepted);

    auto hardware_only_policy = policy;
    hardware_only_policy.minimum_security_level = security_level::hardware;
    EXPECT_FALSE(backend.verify_evidence(evidence, expected_binding, hardware_only_policy).accepted);
}

TEST(
    AttestationService,
    SgxEpidHostQuoteProviderRunsPswStyleQuoteSequence)
{
    evidence_binding binding;
    binding.subject = identity{"epid-enclave", "epid-zone"};
    binding.transcript_id = 36;
    binding.nonce = {1, 4, 9, 16};
    const std::vector<uint8_t> report_data_hash(32U, 0xa5);
    const std::vector<uint8_t> spid(16U, 0x5a);
    const std::vector<uint8_t> sig_rl{0x01, 0x02, 0x03};
    const std::vector<uint8_t> target_info{0x10, 0x20, 0x30};
    const std::vector<uint8_t> report{0x40, 0x50};
    const uint32_t quote_sign_type = 2;

    int init_calls = 0;
    int report_calls = 0;
    int size_calls = 0;
    int quote_calls = 0;

    sgx_epid_host_quote_provider_options options;
    options.spid = spid;
    options.sig_rl = sig_rl;
    options.quote_sign_type = quote_sign_type;
    options.functions.init_quote = [&]() -> std::optional<sgx_epid_init_quote_result>
    {
        ++init_calls;
        sgx_epid_init_quote_result result;
        result.qe_target_info = target_info;
        result.extended_epid_group_id = 0x12345678U;
        return result;
    };
    options.report_producer = [&](const sgx_epid_report_request& request) -> std::optional<std::vector<uint8_t>>
    {
        ++report_calls;
        EXPECT_EQ(request.binding.subject.enclave_id, binding.subject.enclave_id);
        EXPECT_EQ(request.binding.subject.zone_id, binding.subject.zone_id);
        EXPECT_EQ(request.binding.transcript_id, binding.transcript_id);
        EXPECT_EQ(request.qe_target_info, target_info);
        EXPECT_EQ(request.report_data_sha256, report_data_hash);
        return report;
    };
    options.functions.calculate_quote_size = [&](const std::vector<uint8_t>& actual_sig_rl) -> std::optional<uint32_t>
    {
        ++size_calls;
        EXPECT_EQ(actual_sig_rl, sig_rl);
        return 96U;
    };
    options.functions.get_quote = [&](const sgx_epid_get_quote_request& request) -> std::optional<std::vector<uint8_t>>
    {
        ++quote_calls;
        EXPECT_EQ(request.binding.transcript_id, binding.transcript_id);
        EXPECT_EQ(request.report, report);
        EXPECT_EQ(request.quote_sign_type, quote_sign_type);
        EXPECT_EQ(request.spid, spid);
        EXPECT_EQ(request.sig_rl, sig_rl);
        EXPECT_EQ(request.quote_size, 96U);
        EXPECT_EQ(request.report_data_sha256, report_data_hash);
        auto quote = make_sgx_report_data(request.report_data_sha256);
        quote.push_back(0x99);
        return quote;
    };

    sgx_epid_host_quote_provider provider(std::move(options));
    sgx_epid_quote_request request;
    request.binding = binding;
    request.report_data_sha256 = report_data_hash;

    auto material = provider.produce_quote(request);
    ASSERT_TRUE(material.has_value());
    EXPECT_EQ(material->extended_epid_group_id, 0x12345678U);
    EXPECT_EQ(material->quote_sign_type, 2U);
    EXPECT_EQ(material->spid, spid);
    EXPECT_EQ(material->sig_rl, sig_rl);
    ASSERT_EQ(material->quote.size(), 65U);
    EXPECT_EQ(
        std::vector<uint8_t>(material->quote.begin(), material->quote.begin() + 64U),
        make_sgx_report_data(report_data_hash));
    EXPECT_EQ(material->quote.back(), 0x99);
    EXPECT_EQ(init_calls, 1);
    EXPECT_EQ(report_calls, 1);
    EXPECT_EQ(size_calls, 1);
    EXPECT_EQ(quote_calls, 1);
}

TEST(
    AttestationService,
    SgxEpidHostQuoteProviderFailsClosedOnInvalidPswInputs)
{
    sgx_epid_host_quote_provider_options options;
    options.spid = std::vector<uint8_t>(16U, 0x5a);
    options.functions.init_quote = []() -> std::optional<sgx_epid_init_quote_result>
    {
        sgx_epid_init_quote_result result;
        result.qe_target_info = {0x01};
        return result;
    };
    options.report_producer = [](const sgx_epid_report_request&) -> std::optional<std::vector<uint8_t>>
    { return std::vector<uint8_t>{0x02}; };
    options.functions.calculate_quote_size
        = [](const std::vector<uint8_t>&) -> std::optional<uint32_t> { return (64U * 1024U) + 1U; };
    options.functions.get_quote = [](const sgx_epid_get_quote_request&) -> std::optional<std::vector<uint8_t>>
    { return std::vector<uint8_t>{0x03}; };

    sgx_epid_host_quote_provider provider(std::move(options));
    sgx_epid_quote_request request;
    request.binding.subject = identity{"epid-enclave", "epid-zone"};
    request.binding.transcript_id = 37;
    request.binding.nonce = {1, 2, 3};
    request.report_data_sha256 = std::vector<uint8_t>(32U, 0xa5);

    EXPECT_FALSE(provider.produce_quote(request).has_value());
}

TEST(
    AttestationService,
    SgxEpidHostQuoteProviderRejectsOversizedIntermediateBuffers)
{
    sgx_epid_quote_request request;
    request.binding.subject = identity{"epid-enclave", "epid-zone"};
    request.binding.transcript_id = 137;
    request.binding.nonce = {1, 2, 3};
    request.report_data_sha256 = std::vector<uint8_t>(32U, 0xa5);

    sgx_epid_host_quote_provider_options oversized_target_info_options;
    oversized_target_info_options.spid = std::vector<uint8_t>(16U, 0x5a);
    oversized_target_info_options.functions.init_quote = []() -> std::optional<sgx_epid_init_quote_result>
    {
        sgx_epid_init_quote_result result;
        result.qe_target_info = std::vector<uint8_t>((4U * 1024U) + 1U, 0x01);
        return result;
    };
    oversized_target_info_options.report_producer =
        [](const sgx_epid_report_request&) -> std::optional<std::vector<uint8_t>> { return std::vector<uint8_t>{0x02}; };
    oversized_target_info_options.functions.calculate_quote_size
        = [](const std::vector<uint8_t>&) -> std::optional<uint32_t> { return 96U; };
    oversized_target_info_options.functions.get_quote
        = [](const sgx_epid_get_quote_request&) -> std::optional<std::vector<uint8_t>>
    { return std::vector<uint8_t>{0x03}; };
    sgx_epid_host_quote_provider oversized_target_info_provider(std::move(oversized_target_info_options));
    EXPECT_FALSE(oversized_target_info_provider.produce_quote(request).has_value());

    sgx_epid_host_quote_provider_options oversized_report_options;
    oversized_report_options.spid = std::vector<uint8_t>(16U, 0x5a);
    oversized_report_options.functions.init_quote = []() -> std::optional<sgx_epid_init_quote_result>
    {
        sgx_epid_init_quote_result result;
        result.qe_target_info = {0x01};
        return result;
    };
    oversized_report_options.report_producer = [](const sgx_epid_report_request&) -> std::optional<std::vector<uint8_t>>
    { return std::vector<uint8_t>((4U * 1024U) + 1U, 0x02); };
    oversized_report_options.functions.calculate_quote_size
        = [](const std::vector<uint8_t>&) -> std::optional<uint32_t> { return 96U; };
    oversized_report_options.functions.get_quote
        = [](const sgx_epid_get_quote_request&) -> std::optional<std::vector<uint8_t>>
    { return std::vector<uint8_t>{0x03}; };
    sgx_epid_host_quote_provider oversized_report_provider(std::move(oversized_report_options));
    EXPECT_FALSE(oversized_report_provider.produce_quote(request).has_value());
}

TEST(
    AttestationService,
    SgxEpidBackendRejectsOversizedEvidenceBeforeVerifier)
{
    auto verifier = std::make_shared<test_sgx_epid_quote_verifier>();
    sgx_epid_backend backend(nullptr, std::move(verifier));

    evidence_binding expected_binding;
    expected_binding.subject = identity{"epid-enclave", "epid-zone"};
    expected_binding.transcript_id = 33;
    expected_binding.nonce = {1, 2, 3, 4};

    cmw evidence;
    evidence.media_type = sgx_epid_evidence_media_type;
    evidence.content_format = sgx_epid_quote_evidence_content_format;
    evidence.payload = std::vector<uint8_t>((256U * 1024U) + 1U, 0x7f);

    attestation_policy policy;
    policy.required_backend_id = sgx_epid_backend_id;
    policy.minimum_security_level = security_level::hardware_legacy;
    policy.allow_development_evidence = false;

    auto verdict = backend.verify_evidence(evidence, expected_binding, policy);
    EXPECT_FALSE(verdict.accepted);
    EXPECT_EQ(verdict.backend_id, sgx_epid_backend_id);
    EXPECT_EQ(verdict.level, security_level::hardware_legacy);
}

TEST(
    AttestationService,
    SgxEpidBackendRefusesToEmitOversizedProviderMaterial)
{
    auto provider = std::make_shared<test_sgx_epid_quote_provider>();
    provider->sig_rl_size = (64U * 1024U) + 1U;
    auto verifier = std::make_shared<test_sgx_epid_quote_verifier>();
    sgx_epid_backend backend(std::move(provider), std::move(verifier));

    evidence_binding expected_binding;
    expected_binding.subject = identity{"epid-enclave", "epid-zone"};
    expected_binding.transcript_id = 34;
    expected_binding.nonce = {4, 3, 2, 1};

    auto evidence = backend.produce_evidence(expected_binding);
    EXPECT_EQ(evidence.media_type, sgx_epid_evidence_media_type);
    EXPECT_EQ(evidence.content_format, sgx_epid_unavailable_content_format);
    EXPECT_TRUE(evidence.payload.empty());
}

TEST(
    AttestationService,
    SgxEpidBackendRejectsOversizedWireFieldsBeforeVerifier)
{
    auto provider = std::make_shared<test_sgx_epid_quote_provider>();
    provider->include_ias_report = true;
    auto verifier = std::make_shared<counting_sgx_epid_quote_verifier>();
    sgx_epid_backend backend(provider, verifier);

    evidence_binding expected_binding;
    expected_binding.subject = identity{"epid-enclave", "epid-zone"};
    expected_binding.transcript_id = 38;
    expected_binding.nonce = {8, 6, 4, 2};

    auto evidence = backend.produce_evidence(expected_binding);
    ASSERT_EQ(evidence.content_format, sgx_epid_quote_evidence_content_format);

    auto wire = decode_canonical_payload<rpc::attestation::sgx_epid_quote_evidence>(evidence);
    ASSERT_TRUE(wire.has_value());
    ASSERT_TRUE(wire->ias_report.has_value());

    attestation_policy policy;
    policy.required_backend_id = sgx_epid_backend_id;
    policy.minimum_security_level = security_level::hardware_legacy;
    policy.allow_development_evidence = false;

    auto oversized_ias_report = wire.value();
    oversized_ias_report.ias_report->body_json = std::string((64U * 1024U) + 1U, 'x');
    auto oversized_ias_payload = encode_canonical_payload(oversized_ias_report);
    ASSERT_TRUE(oversized_ias_payload.has_value());

    auto oversized_ias_evidence = evidence;
    oversized_ias_evidence.payload = std::move(oversized_ias_payload.value());
    auto oversized_ias_verdict = backend.verify_evidence(oversized_ias_evidence, expected_binding, policy);
    EXPECT_FALSE(oversized_ias_verdict.accepted);
    EXPECT_EQ(verifier->calls, 0);

    auto oversized_sig_rl = wire.value();
    oversized_sig_rl.sig_rl = std::vector<uint8_t>((64U * 1024U) + 1U, 0x17);
    auto oversized_sig_rl_payload = encode_canonical_payload(oversized_sig_rl);
    ASSERT_TRUE(oversized_sig_rl_payload.has_value());

    auto oversized_sig_rl_evidence = evidence;
    oversized_sig_rl_evidence.payload = std::move(oversized_sig_rl_payload.value());
    auto oversized_sig_rl_verdict = backend.verify_evidence(oversized_sig_rl_evidence, expected_binding, policy);
    EXPECT_FALSE(oversized_sig_rl_verdict.accepted);
    EXPECT_EQ(verifier->calls, 0);
}

TEST(
    AttestationService,
    SgxEpidVerifierContractCanRequireIasChecks)
{
    auto provider = std::make_shared<test_sgx_epid_quote_provider>();
    auto strict_verifier = std::make_shared<strict_test_sgx_epid_quote_verifier>();
    sgx_epid_backend backend(provider, strict_verifier);

    evidence_binding expected_binding;
    expected_binding.subject = identity{"epid-enclave", "epid-zone"};
    expected_binding.transcript_id = 35;
    expected_binding.nonce = {9, 7, 5, 3};

    attestation_policy policy;
    policy.required_backend_id = sgx_epid_backend_id;
    policy.minimum_security_level = security_level::hardware_legacy;
    policy.allow_development_evidence = false;

    auto evidence_without_ias = backend.produce_evidence(expected_binding);
    auto rejected = backend.verify_evidence(evidence_without_ias, expected_binding, policy);
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.backend_id, sgx_epid_backend_id);
    EXPECT_EQ(rejected.level, security_level::hardware_legacy);

    provider->include_ias_report = true;
    auto evidence_with_ias = backend.produce_evidence(expected_binding);
    auto accepted = backend.verify_evidence(evidence_with_ias, expected_binding, policy);
    EXPECT_TRUE(accepted.accepted) << accepted.reason;
    EXPECT_EQ(accepted.backend_id, sgx_epid_backend_id);
    EXPECT_EQ(accepted.level, security_level::hardware_legacy);
}

TEST(
    AttestationService,
    SgxEpidIasVerifierAcceptsReportDataBoundQuote)
{
    sgx_epid_ias_report_verifier_options options;
    bool report_data_extracted = false;
    bool ias_report_validated = false;
    options.extract_report_data = [&](const std::vector<uint8_t>& quote) -> std::optional<std::vector<uint8_t>>
    {
        report_data_extracted = true;
        return extract_sgx_report_data_prefix(quote);
    };
    options.validate_ias_report = [&](const sgx_epid_verifier_input& input,
                                      const attestation_policy& policy) -> sgx_epid_ias_report_validation_result
    {
        ias_report_validated = true;
        EXPECT_EQ(policy.required_backend_id, sgx_epid_backend_id);
        EXPECT_TRUE(input.quote.ias_report.has_value());
        EXPECT_EQ(input.quote.ias_report->quote_status, "OK");
        return sgx_epid_ias_report_validation_result{true, "IAS report accepted"};
    };

    sgx_epid_ias_report_verifier verifier(std::move(options));
    sgx_epid_verifier_input input;
    input.expected_binding.subject = identity{"epid-enclave", "epid-zone"};
    input.report_data_sha256 = std::vector<uint8_t>(32U, 0x44);
    input.quote.quote = make_sgx_report_data(input.report_data_sha256);
    input.quote.quote.push_back(0x17);
    sgx_epid_ias_report_material report;
    report.body_json = R"({"isvEnclaveQuoteStatus":"OK"})";
    report.signature = {0x10};
    report.signing_certificate_chain = {0x20};
    report.quote_status = "OK";
    input.quote.ias_report = report;

    attestation_policy policy;
    policy.required_backend_id = sgx_epid_backend_id;
    policy.minimum_security_level = security_level::hardware_legacy;

    auto verdict = verifier.verify_quote(input, policy);
    EXPECT_TRUE(verdict.accepted) << verdict.reason;
    EXPECT_EQ(verdict.backend_id, sgx_epid_backend_id);
    EXPECT_EQ(verdict.level, security_level::hardware_legacy);
    EXPECT_EQ(verdict.peer_identity.enclave_id, input.expected_binding.subject.enclave_id);
    EXPECT_TRUE(report_data_extracted);
    EXPECT_TRUE(ias_report_validated);
}

TEST(
    AttestationService,
    SgxEpidIasVerifierRejectsUnexpectedStatusAndReportData)
{
    sgx_epid_ias_report_verifier_options options;
    options.extract_report_data = [](const std::vector<uint8_t>& quote) -> std::optional<std::vector<uint8_t>>
    { return extract_sgx_report_data_prefix(quote); };
    options.validate_ias_report
        = [](const sgx_epid_verifier_input&, const attestation_policy&) -> sgx_epid_ias_report_validation_result
    { return sgx_epid_ias_report_validation_result{true, "unexpected acceptance"}; };

    sgx_epid_ias_report_verifier verifier(std::move(options));
    sgx_epid_verifier_input input;
    input.expected_binding.subject = identity{"epid-enclave", "epid-zone"};
    input.report_data_sha256 = std::vector<uint8_t>(32U, 0x44);
    input.quote.quote = make_sgx_report_data(std::vector<uint8_t>(32U, 0x55));
    sgx_epid_ias_report_material report;
    report.quote_status = "GROUP_OUT_OF_DATE";
    input.quote.ias_report = report;

    attestation_policy policy;
    policy.required_backend_id = sgx_epid_backend_id;
    policy.minimum_security_level = security_level::hardware_legacy;

    auto bad_status = verifier.verify_quote(input, policy);
    EXPECT_FALSE(bad_status.accepted);

    input.quote.ias_report->quote_status = "OK";
    auto bad_report_data = verifier.verify_quote(input, policy);
    EXPECT_FALSE(bad_report_data.accepted);
}

TEST(
    AttestationService,
    SgxDcapBackendWithoutQuoteProviderFailsClosed)
{
    sgx_dcap_backend backend;

    evidence_binding expected_binding;
    expected_binding.subject = identity{"dcap-enclave", "dcap-zone"};
    expected_binding.transcript_id = 41;
    expected_binding.nonce = {2, 4, 6, 8};

    auto evidence = backend.produce_evidence(expected_binding);
    EXPECT_EQ(backend.backend_id(), sgx_dcap_backend_id);
    EXPECT_EQ(backend.level(), security_level::hardware);
    EXPECT_EQ(evidence.media_type, sgx_dcap_evidence_media_type);
    EXPECT_EQ(evidence.content_format, sgx_dcap_unavailable_content_format);

    attestation_policy policy;
    policy.required_backend_id = sgx_dcap_backend_id;
    policy.minimum_security_level = security_level::hardware;
    policy.allow_development_evidence = false;

    auto verdict = backend.verify_evidence(evidence, expected_binding, policy);
    EXPECT_FALSE(verdict.accepted);
}

TEST(
    AttestationService,
    SgxDcapBackendUsesInjectedQuoteProviderAndVerifier)
{
    auto provider = std::make_shared<test_sgx_dcap_quote_provider>();
    auto verifier = std::make_shared<test_sgx_dcap_quote_verifier>();
    sgx_dcap_backend backend(std::move(provider), std::move(verifier));

    evidence_binding expected_binding;
    expected_binding.subject = identity{"dcap-enclave", "dcap-zone"};
    expected_binding.transcript_id = 42;
    expected_binding.nonce = {3, 5, 7, 9};

    auto evidence = backend.produce_evidence(expected_binding);
    EXPECT_EQ(evidence.media_type, sgx_dcap_evidence_media_type);
    EXPECT_EQ(evidence.content_format, sgx_dcap_quote_evidence_content_format);
    EXPECT_FALSE(evidence.payload.empty());

    attestation_policy policy;
    policy.required_backend_id = sgx_dcap_backend_id;
    policy.minimum_security_level = security_level::hardware;
    policy.allow_development_evidence = false;

    auto verdict = backend.verify_evidence(evidence, expected_binding, policy);
    EXPECT_TRUE(verdict.accepted) << verdict.reason;
    EXPECT_EQ(verdict.backend_id, sgx_dcap_backend_id);
    EXPECT_EQ(verdict.level, security_level::hardware);
    EXPECT_EQ(verdict.peer_identity.enclave_id, expected_binding.subject.enclave_id);
    EXPECT_EQ(verdict.peer_identity.zone_id, expected_binding.subject.zone_id);

    auto wrong_nonce_binding = expected_binding;
    wrong_nonce_binding.nonce.push_back(11);
    EXPECT_FALSE(backend.verify_evidence(evidence, wrong_nonce_binding, policy).accepted);

    auto wrong_backend_policy = policy;
    wrong_backend_policy.required_backend_id = sgx_epid_backend_id;
    EXPECT_FALSE(backend.verify_evidence(evidence, expected_binding, wrong_backend_policy).accepted);
}

TEST(
    AttestationService,
    SgxDcapHostQuoteProviderRunsQeQuoteSequence)
{
    evidence_binding binding;
    binding.subject = identity{"dcap-enclave", "dcap-zone"};
    binding.transcript_id = 45;
    binding.nonce = {2, 3, 5, 7};
    const std::vector<uint8_t> report_data_hash(32U, 0xd5);
    const std::vector<uint8_t> target_info{0x11, 0x22, 0x33};
    const std::vector<uint8_t> report{0x44, 0x55};

    int target_info_calls = 0;
    int report_calls = 0;
    int size_calls = 0;
    int quote_calls = 0;

    sgx_dcap_host_quote_provider_options options;
    options.functions.get_target_info = [&]() -> std::optional<sgx_dcap_target_info_result>
    {
        ++target_info_calls;
        sgx_dcap_target_info_result result;
        result.qe_target_info = target_info;
        return result;
    };
    options.report_producer = [&](const sgx_dcap_report_request& request) -> std::optional<std::vector<uint8_t>>
    {
        ++report_calls;
        EXPECT_EQ(request.binding.subject.enclave_id, binding.subject.enclave_id);
        EXPECT_EQ(request.binding.subject.zone_id, binding.subject.zone_id);
        EXPECT_EQ(request.binding.transcript_id, binding.transcript_id);
        EXPECT_EQ(request.qe_target_info, target_info);
        EXPECT_EQ(request.report_data_sha256, report_data_hash);
        return report;
    };
    options.functions.get_quote_size = [&]() -> std::optional<uint32_t>
    {
        ++size_calls;
        return 96U;
    };
    options.functions.get_quote = [&](const sgx_dcap_get_quote_request& request) -> std::optional<std::vector<uint8_t>>
    {
        ++quote_calls;
        EXPECT_EQ(request.binding.transcript_id, binding.transcript_id);
        EXPECT_EQ(request.report, report);
        EXPECT_EQ(request.quote_size, 96U);
        EXPECT_EQ(request.report_data_sha256, report_data_hash);
        auto quote = make_sgx_report_data(request.report_data_sha256);
        quote.push_back(0xdc);
        return quote;
    };

    sgx_dcap_host_quote_provider provider(std::move(options));
    sgx_dcap_quote_request request;
    request.binding = binding;
    request.report_data_sha256 = report_data_hash;

    auto material = provider.produce_quote(request);
    ASSERT_TRUE(material.has_value());
    ASSERT_EQ(material->quote.size(), 65U);
    EXPECT_EQ(
        std::vector<uint8_t>(material->quote.begin(), material->quote.begin() + 64U),
        make_sgx_report_data(report_data_hash));
    EXPECT_EQ(material->quote.back(), 0xdc);
    EXPECT_EQ(target_info_calls, 1);
    EXPECT_EQ(report_calls, 1);
    EXPECT_EQ(size_calls, 1);
    EXPECT_EQ(quote_calls, 1);
}

TEST(
    AttestationService,
    SgxDcapHostQuoteProviderFailsClosedOnInvalidQeInputs)
{
    sgx_dcap_host_quote_provider_options options;
    options.functions.get_target_info = []() -> std::optional<sgx_dcap_target_info_result>
    {
        sgx_dcap_target_info_result result;
        result.qe_target_info = {0x01};
        return result;
    };
    options.report_producer = [](const sgx_dcap_report_request&) -> std::optional<std::vector<uint8_t>>
    { return std::vector<uint8_t>{0x02}; };
    options.functions.get_quote_size = []() -> std::optional<uint32_t> { return (256U * 1024U) + 1U; };
    options.functions.get_quote = [](const sgx_dcap_get_quote_request&) -> std::optional<std::vector<uint8_t>>
    { return std::vector<uint8_t>{0x03}; };

    sgx_dcap_host_quote_provider provider(std::move(options));
    sgx_dcap_quote_request request;
    request.binding.subject = identity{"dcap-enclave", "dcap-zone"};
    request.binding.transcript_id = 46;
    request.binding.nonce = {1, 2, 3};
    request.report_data_sha256 = std::vector<uint8_t>(32U, 0xd5);

    EXPECT_FALSE(provider.produce_quote(request).has_value());
}

TEST(
    AttestationService,
    SgxDcapHostQuoteProviderRejectsOversizedIntermediateBuffers)
{
    sgx_dcap_quote_request request;
    request.binding.subject = identity{"dcap-enclave", "dcap-zone"};
    request.binding.transcript_id = 146;
    request.binding.nonce = {1, 2, 3};
    request.report_data_sha256 = std::vector<uint8_t>(32U, 0xd5);

    sgx_dcap_host_quote_provider_options oversized_target_info_options;
    oversized_target_info_options.functions.get_target_info = []() -> std::optional<sgx_dcap_target_info_result>
    {
        sgx_dcap_target_info_result result;
        result.qe_target_info = std::vector<uint8_t>((4U * 1024U) + 1U, 0x01);
        return result;
    };
    oversized_target_info_options.report_producer =
        [](const sgx_dcap_report_request&) -> std::optional<std::vector<uint8_t>> { return std::vector<uint8_t>{0x02}; };
    oversized_target_info_options.functions.get_quote_size = []() -> std::optional<uint32_t> { return 96U; };
    oversized_target_info_options.functions.get_quote
        = [](const sgx_dcap_get_quote_request&) -> std::optional<std::vector<uint8_t>>
    { return std::vector<uint8_t>{0x03}; };
    sgx_dcap_host_quote_provider oversized_target_info_provider(std::move(oversized_target_info_options));
    EXPECT_FALSE(oversized_target_info_provider.produce_quote(request).has_value());

    sgx_dcap_host_quote_provider_options oversized_report_options;
    oversized_report_options.functions.get_target_info = []() -> std::optional<sgx_dcap_target_info_result>
    {
        sgx_dcap_target_info_result result;
        result.qe_target_info = {0x01};
        return result;
    };
    oversized_report_options.report_producer = [](const sgx_dcap_report_request&) -> std::optional<std::vector<uint8_t>>
    { return std::vector<uint8_t>((4U * 1024U) + 1U, 0x02); };
    oversized_report_options.functions.get_quote_size = []() -> std::optional<uint32_t> { return 96U; };
    oversized_report_options.functions.get_quote
        = [](const sgx_dcap_get_quote_request&) -> std::optional<std::vector<uint8_t>>
    { return std::vector<uint8_t>{0x03}; };
    sgx_dcap_host_quote_provider oversized_report_provider(std::move(oversized_report_options));
    EXPECT_FALSE(oversized_report_provider.produce_quote(request).has_value());
}

TEST(
    AttestationService,
    SgxDcapQuote3ParserExtractsReportDataAndRejectsMalformedQuotes)
{
    const auto report_data = make_sgx_report_data(std::vector<uint8_t>(32U, 0x5c));
    auto quote = make_sgx_quote3_quote(report_data, 4U);
    quote[sgx_dcap_quote3_signature_data_offset] = 0xaa;

    auto extracted = extract_sgx_quote3_report_data(quote);
    ASSERT_TRUE(extracted.has_value());
    EXPECT_EQ(extracted.value(), report_data);

    auto truncated_fixed_prefix = quote;
    truncated_fixed_prefix.resize(sgx_dcap_quote3_signature_data_offset - 1U);
    EXPECT_FALSE(extract_sgx_quote3_report_data(truncated_fixed_prefix).has_value());

    auto truncated_signature_data = quote;
    truncated_signature_data.resize(sgx_dcap_quote3_signature_data_offset + 1U);
    EXPECT_FALSE(extract_sgx_quote3_report_data(truncated_signature_data).has_value());
}

TEST(
    AttestationService,
    SgxDcapHostVerifierAcceptsConfiguredQvResult)
{
    sgx_dcap_host_quote_verifier_options options;
    bool verified = false;
    options.verify_quote
        = [&](const sgx_dcap_verifier_input& input, const attestation_policy& policy) -> sgx_dcap_host_quote_verification
    {
        verified = true;
        EXPECT_EQ(policy.required_backend_id, sgx_dcap_backend_id);
        EXPECT_EQ(
            extract_sgx_quote3_report_data(input.quote.quote).value(), make_sgx_report_data(input.report_data_sha256));

        sgx_dcap_host_quote_verification result;
        result.call_succeeded = true;
        result.result.quote_verification_result = canopy::security::attestation::sgx_dcap_qv_result_ok;
        result.result.quote_verification_result_name = "OK";
        result.result.collateral_expired = 0;
        return result;
    };

    sgx_dcap_host_quote_verifier verifier(std::move(options));
    sgx_dcap_verifier_input input;
    input.expected_binding.subject = identity{"dcap-enclave", "dcap-zone"};
    input.report_data_sha256 = std::vector<uint8_t>(32U, 0x12);
    input.quote.quote = make_sgx_quote3_quote(make_sgx_report_data(input.report_data_sha256));

    attestation_policy policy;
    policy.required_backend_id = sgx_dcap_backend_id;
    policy.minimum_security_level = security_level::hardware;

    auto verdict = verifier.verify_quote(input, policy);
    EXPECT_TRUE(verdict.accepted) << verdict.reason;
    EXPECT_EQ(verdict.backend_id, sgx_dcap_backend_id);
    EXPECT_EQ(verdict.level, security_level::hardware);
    EXPECT_EQ(verdict.peer_identity.enclave_id, input.expected_binding.subject.enclave_id);
    EXPECT_TRUE(verified);
}

TEST(
    AttestationService,
    SgxDcapHostVerifierRequiresReportDataBoundQuote)
{
    bool verify_quote_called = false;
    sgx_dcap_host_quote_verifier_options malformed_quote_options;
    malformed_quote_options.verify_quote
        = [&](const sgx_dcap_verifier_input&, const attestation_policy&) -> sgx_dcap_host_quote_verification
    {
        verify_quote_called = true;
        return sgx_dcap_host_quote_verification{true, {}, "unexpected verifier call"};
    };

    sgx_dcap_verifier_input input;
    input.expected_binding.subject = identity{"dcap-enclave", "dcap-zone"};
    input.report_data_sha256 = std::vector<uint8_t>(32U, 0x12);
    input.quote.quote = make_sgx_report_data(input.report_data_sha256);

    attestation_policy policy;
    policy.required_backend_id = sgx_dcap_backend_id;
    policy.minimum_security_level = security_level::hardware;

    sgx_dcap_host_quote_verifier malformed_quote_verifier(std::move(malformed_quote_options));
    auto malformed_quote = malformed_quote_verifier.verify_quote(input, policy);
    EXPECT_FALSE(malformed_quote.accepted);
    EXPECT_FALSE(verify_quote_called);

    sgx_dcap_host_quote_verifier_options mismatched_report_data_options;
    mismatched_report_data_options.verify_quote
        = [&](const sgx_dcap_verifier_input&, const attestation_policy&) -> sgx_dcap_host_quote_verification
    {
        verify_quote_called = true;
        return sgx_dcap_host_quote_verification{true, {}, "unexpected verifier call"};
    };

    input.quote.quote = make_sgx_quote3_quote(make_sgx_report_data(std::vector<uint8_t>(32U, 0x34)));
    sgx_dcap_host_quote_verifier mismatched_report_data_verifier(std::move(mismatched_report_data_options));
    auto mismatched_report_data = mismatched_report_data_verifier.verify_quote(input, policy);
    EXPECT_FALSE(mismatched_report_data.accepted);
    EXPECT_FALSE(verify_quote_called);
}

TEST(
    AttestationService,
    SgxDcapHostVerifierMakesDelegatedVerificationResultExplicit)
{
    sgx_dcap_verifier_input input;
    input.expected_binding.subject = identity{"dcap-enclave", "dcap-zone"};
    input.report_data_sha256 = std::vector<uint8_t>(32U, 0x12);
    input.quote.quote = make_sgx_quote3_quote(make_sgx_report_data(input.report_data_sha256));
    input.quote.verification_result = sgx_dcap_verification_result_material{};
    input.quote.verification_result->quote_verification_result = canopy::security::attestation::sgx_dcap_qv_result_ok;

    attestation_policy policy;
    policy.required_backend_id = sgx_dcap_backend_id;
    policy.minimum_security_level = security_level::hardware;

    bool verify_quote_called = false;
    sgx_dcap_host_quote_verifier_options default_options;
    default_options.verify_quote
        = [&](const sgx_dcap_verifier_input&, const attestation_policy&) -> sgx_dcap_host_quote_verification
    {
        verify_quote_called = true;
        return sgx_dcap_host_quote_verification{true, {}, "unexpected verifier call"};
    };

    sgx_dcap_host_quote_verifier default_verifier(std::move(default_options));
    auto default_rejection = default_verifier.verify_quote(input, policy);
    EXPECT_FALSE(default_rejection.accepted);
    EXPECT_FALSE(verify_quote_called);

    bool delegated_checked = false;
    sgx_dcap_host_quote_verifier_options verified_options;
    verified_options.delegated_result_mode = sgx_dcap_delegated_verification_result_mode::require_verified;
    verified_options.verify_delegated_verification_result
        = [&](const sgx_dcap_verifier_input& delegated_input,
              const attestation_policy& delegated_policy) -> sgx_dcap_delegated_verification_result_check
    {
        delegated_checked = true;
        EXPECT_TRUE(delegated_input.quote.verification_result.has_value());
        EXPECT_EQ(delegated_policy.required_backend_id, sgx_dcap_backend_id);
        return sgx_dcap_delegated_verification_result_check{true, ""};
    };
    verified_options.verify_quote
        = [&](const sgx_dcap_verifier_input&, const attestation_policy&) -> sgx_dcap_host_quote_verification
    {
        verify_quote_called = true;
        sgx_dcap_host_quote_verification result;
        result.call_succeeded = true;
        result.result.quote_verification_result = canopy::security::attestation::sgx_dcap_qv_result_ok;
        result.result.quote_verification_result_name = "OK";
        return result;
    };

    sgx_dcap_host_quote_verifier verified_verifier(std::move(verified_options));
    auto accepted = verified_verifier.verify_quote(input, policy);
    EXPECT_TRUE(accepted.accepted) << accepted.reason;
    EXPECT_TRUE(delegated_checked);
    EXPECT_TRUE(verify_quote_called);
}

TEST(
    AttestationService,
    SgxDcapHostVerifierRejectsUnacceptedQvResultAndExpiredCollateral)
{
    sgx_dcap_host_quote_verifier_options options;
    options.verify_quote = [](const sgx_dcap_verifier_input&, const attestation_policy&) -> sgx_dcap_host_quote_verification
    {
        sgx_dcap_host_quote_verification result;
        result.call_succeeded = true;
        result.result.quote_verification_result = 2;
        result.result.quote_verification_result_name = "CONFIG_NEEDED";
        return result;
    };

    sgx_dcap_host_quote_verifier verifier(std::move(options));
    sgx_dcap_verifier_input input;
    input.expected_binding.subject = identity{"dcap-enclave", "dcap-zone"};
    input.report_data_sha256 = std::vector<uint8_t>(32U, 0x12);
    input.quote.quote = make_sgx_quote3_quote(make_sgx_report_data(input.report_data_sha256));

    attestation_policy policy;
    policy.required_backend_id = sgx_dcap_backend_id;
    policy.minimum_security_level = security_level::hardware;

    auto bad_qv_result = verifier.verify_quote(input, policy);
    EXPECT_FALSE(bad_qv_result.accepted);

    sgx_dcap_host_quote_verifier_options expired_options;
    expired_options.verify_quote
        = [](const sgx_dcap_verifier_input&, const attestation_policy&) -> sgx_dcap_host_quote_verification
    {
        sgx_dcap_host_quote_verification result;
        result.call_succeeded = true;
        result.result.quote_verification_result = 0;
        result.result.quote_verification_result_name = "OK";
        result.result.collateral_expired = 1;
        return result;
    };

    sgx_dcap_host_quote_verifier expired_verifier(std::move(expired_options));
    auto expired = expired_verifier.verify_quote(input, policy);
    EXPECT_FALSE(expired.accepted);
}

TEST(
    AttestationService,
    SgxDcapBackendRejectsOversizedEvidenceBeforeVerifier)
{
    auto verifier = std::make_shared<test_sgx_dcap_quote_verifier>();
    sgx_dcap_backend backend(nullptr, std::move(verifier));

    evidence_binding expected_binding;
    expected_binding.subject = identity{"dcap-enclave", "dcap-zone"};
    expected_binding.transcript_id = 43;
    expected_binding.nonce = {1, 2, 3, 4};

    cmw evidence;
    evidence.media_type = sgx_dcap_evidence_media_type;
    evidence.content_format = sgx_dcap_quote_evidence_content_format;
    evidence.payload = std::vector<uint8_t>((1024U * 1024U) + 1U, 0x7f);

    attestation_policy policy;
    policy.required_backend_id = sgx_dcap_backend_id;
    policy.minimum_security_level = security_level::hardware;
    policy.allow_development_evidence = false;

    auto verdict = backend.verify_evidence(evidence, expected_binding, policy);
    EXPECT_FALSE(verdict.accepted);
    EXPECT_EQ(verdict.backend_id, sgx_dcap_backend_id);
    EXPECT_EQ(verdict.level, security_level::hardware);
}

TEST(
    AttestationService,
    SgxDcapBackendRefusesToEmitOversizedProviderMaterial)
{
    auto provider = std::make_shared<test_sgx_dcap_quote_provider>();
    provider->quote_padding_size = (256U * 1024U) + 1U;
    auto verifier = std::make_shared<test_sgx_dcap_quote_verifier>();
    sgx_dcap_backend backend(std::move(provider), std::move(verifier));

    evidence_binding expected_binding;
    expected_binding.subject = identity{"dcap-enclave", "dcap-zone"};
    expected_binding.transcript_id = 44;
    expected_binding.nonce = {4, 3, 2, 1};

    auto evidence = backend.produce_evidence(expected_binding);
    EXPECT_EQ(evidence.media_type, sgx_dcap_evidence_media_type);
    EXPECT_EQ(evidence.content_format, sgx_dcap_unavailable_content_format);
    EXPECT_TRUE(evidence.payload.empty());
}

TEST(
    AttestationService,
    SgxDcapBackendRejectsOversizedWireFieldsBeforeVerifier)
{
    auto provider = std::make_shared<test_sgx_dcap_quote_provider>();
    provider->supplemental_data_size = 1U;
    auto verifier = std::make_shared<counting_sgx_dcap_quote_verifier>();
    sgx_dcap_backend backend(provider, verifier);

    evidence_binding expected_binding;
    expected_binding.subject = identity{"dcap-enclave", "dcap-zone"};
    expected_binding.transcript_id = 47;
    expected_binding.nonce = {8, 6, 4, 2};

    auto evidence = backend.produce_evidence(expected_binding);
    ASSERT_EQ(evidence.content_format, sgx_dcap_quote_evidence_content_format);

    auto wire = decode_canonical_payload<rpc::attestation::sgx_dcap_quote_evidence>(evidence);
    ASSERT_TRUE(wire.has_value());
    ASSERT_TRUE(wire->verification_result.has_value());

    attestation_policy policy;
    policy.required_backend_id = sgx_dcap_backend_id;
    policy.minimum_security_level = security_level::hardware;
    policy.allow_development_evidence = false;

    auto oversized_supplemental_data = wire.value();
    oversized_supplemental_data.verification_result->supplemental_data = std::vector<uint8_t>((256U * 1024U) + 1U, 0x44);
    auto oversized_supplemental_payload = encode_canonical_payload(oversized_supplemental_data);
    ASSERT_TRUE(oversized_supplemental_payload.has_value());

    auto oversized_supplemental_evidence = evidence;
    oversized_supplemental_evidence.payload = std::move(oversized_supplemental_payload.value());
    auto oversized_supplemental_verdict
        = backend.verify_evidence(oversized_supplemental_evidence, expected_binding, policy);
    EXPECT_FALSE(oversized_supplemental_verdict.accepted);
    EXPECT_EQ(verifier->calls, 0);

    auto oversized_quote = wire.value();
    oversized_quote.quote = std::vector<uint8_t>((256U * 1024U) + 1U, 0xdc);
    auto oversized_quote_payload = encode_canonical_payload(oversized_quote);
    ASSERT_TRUE(oversized_quote_payload.has_value());

    auto oversized_quote_evidence = evidence;
    oversized_quote_evidence.payload = std::move(oversized_quote_payload.value());
    auto oversized_quote_verdict = backend.verify_evidence(oversized_quote_evidence, expected_binding, policy);
    EXPECT_FALSE(oversized_quote_verdict.accepted);
    EXPECT_EQ(verifier->calls, 0);
}

TEST(
    AttestationService,
    BackendFactoryUsesConfiguredBackendDefaults)
{
    const auto configured_kind = configured_attestation_backend_kind();
    if (configured_kind == configured_backend_kind::sgx_epid_backend
        || configured_kind == configured_backend_kind::sgx_dcap_backend)
    {
        EXPECT_DEATH_IF_SUPPORTED(
            (void)make_configured_attestation_service_options(identity{"factory-enclave", "factory-zone"}), "");
        return;
    }

    auto options = make_configured_attestation_service_options(identity{"factory-enclave", "factory-zone"});
    ASSERT_TRUE(options.backend);
    EXPECT_EQ(options.backend->backend_id(), configured_attestation_backend_name());
    EXPECT_EQ(options.local_identity.enclave_id, "factory-enclave");
    EXPECT_EQ(options.local_identity.zone_id, "factory-zone");

    if (configured_kind == configured_backend_kind::null_backend)
    {
        EXPECT_FALSE(options.policy.send_local_evidence);
        EXPECT_FALSE(options.policy.require_peer_evidence);
        EXPECT_TRUE(options.policy.allow_unattested_peer);
        EXPECT_FALSE(options.policy.allow_development_evidence);
        EXPECT_EQ(options.policy.minimum_security_level, security_level::none);
        EXPECT_EQ(options.policy.required_backend_id, null_backend_id);
    }
    else if (configured_kind == configured_backend_kind::fake_backend)
    {
        EXPECT_TRUE(options.policy.send_local_evidence);
        EXPECT_TRUE(options.policy.require_peer_evidence);
        EXPECT_FALSE(options.policy.allow_unattested_peer);
        EXPECT_TRUE(options.policy.allow_development_evidence);
        EXPECT_EQ(options.policy.minimum_security_level, security_level::development);
        EXPECT_EQ(options.policy.required_backend_id, fake_backend_id);
    }
    else if (configured_kind == configured_backend_kind::sgx_sim_backend)
    {
        EXPECT_TRUE(options.policy.send_local_evidence);
        EXPECT_TRUE(options.policy.require_peer_evidence);
        EXPECT_FALSE(options.policy.allow_unattested_peer);
        EXPECT_TRUE(options.policy.allow_development_evidence);
        EXPECT_EQ(options.policy.minimum_security_level, security_level::simulation);
        EXPECT_EQ(options.policy.required_backend_id, simulation_backend_id);
    }
}

TEST(
    AttestationService,
    BackendFactoryCanUsePrebuiltHardwareBackendOverride)
{
    backend_factory_overrides overrides;
    overrides.backend = std::make_shared<sgx_epid_backend>(
        std::make_shared<test_sgx_epid_quote_provider>(), std::make_shared<test_sgx_epid_quote_verifier>());

    auto backend = make_configured_attestation_backend(std::move(overrides));
    ASSERT_TRUE(backend);
    EXPECT_EQ(backend->backend_id(), sgx_epid_backend_id);
    EXPECT_EQ(backend->level(), security_level::hardware_legacy);

    backend_factory_overrides service_overrides;
    service_overrides.backend = std::make_shared<sgx_epid_backend>(
        std::make_shared<test_sgx_epid_quote_provider>(), std::make_shared<test_sgx_epid_quote_verifier>());

    auto service_options = make_configured_attestation_service_options(
        identity{"factory-enclave", "factory-zone"}, std::move(service_overrides));
    ASSERT_TRUE(service_options.backend);
    EXPECT_EQ(service_options.backend->backend_id(), sgx_epid_backend_id);
    EXPECT_TRUE(service_options.policy.send_local_evidence);
    EXPECT_TRUE(service_options.policy.require_peer_evidence);
    EXPECT_FALSE(service_options.policy.allow_unattested_peer);
    EXPECT_FALSE(service_options.policy.allow_development_evidence);
    EXPECT_EQ(service_options.policy.minimum_security_level, security_level::hardware_legacy);
    EXPECT_EQ(service_options.policy.required_backend_id, sgx_epid_backend_id);
}

TEST(
    AttestationService,
    BackendFactoryRequiresExplicitHardwareBackendConstruction)
{
    EXPECT_DEATH_IF_SUPPORTED((void)make_attestation_backend(configured_backend_kind::sgx_epid_backend), "");
    EXPECT_DEATH_IF_SUPPORTED((void)make_attestation_backend(configured_backend_kind::sgx_dcap_backend), "");
}

TEST(
    AttestationService,
    SessionIdsAreScopedToEnclavePairs)
{
    auto first = make_session_id(identity{"enclave-a", "zone-a"}, identity{"enclave-b", "zone-b"}, 99);
    auto second = make_session_id(identity{"enclave-a", "zone-a-2"}, identity{"enclave-b", "zone-b-2"}, 99);
    auto third = make_session_id(identity{"enclave-a", "zone-a"}, identity{"enclave-c", "zone-c"}, 99);
    auto fourth = make_session_id(identity{"enclave-a", "zone-a"}, identity{"enclave-b", "zone-b"}, 100);
    auto framed_left = make_session_id(identity{"enclave:a", "zone-a"}, identity{"enclave-b", "zone-b"}, 99);
    auto framed_right = make_session_id(identity{"enclave", "zone-a"}, identity{"a:enclave-b", "zone-b"}, 99);

    EXPECT_EQ(first, second);
    EXPECT_NE(first, third);
    EXPECT_NE(first, fourth);
    EXPECT_NE(framed_left, framed_right);
}

TEST(
    AttestationService,
    RouteAttestationStateDecisionMatrix)
{
    route_attestation_state state;
    EXPECT_EQ(evaluate_route_attestation_state(state), route_attestation_action::start_handshake);

    state.status = route_attestation_status::handshaking;
    EXPECT_EQ(evaluate_route_attestation_state(state), route_attestation_action::wait_for_handshake);

    state.status = route_attestation_status::failed;
    EXPECT_EQ(evaluate_route_attestation_state(state), route_attestation_action::reject);

    state.status = route_attestation_status::unattested_allowed;
    EXPECT_EQ(evaluate_route_attestation_state(state), route_attestation_action::allow);

    state.status = route_attestation_status::attested;
    state.context.reset();
    EXPECT_EQ(evaluate_route_attestation_state(state), route_attestation_action::start_handshake);

    auto service = make_service("enclave-a", "zone-a");
    auto context = establish(service, identity{"enclave-b", "zone-b"});
    ASSERT_TRUE(context.established);
    state.context = context;
    EXPECT_EQ(evaluate_route_attestation_state(state), route_attestation_action::allow);

    state.context->established = false;
    EXPECT_EQ(evaluate_route_attestation_state(state), route_attestation_action::start_handshake);
}

TEST(
    AttestationService,
    ReferenceRoutePolicySeparatesAddRefHandshakeFromExistingReferenceControl)
{
    reference_route_policy_input input;
    input.attestation_required = false;
    EXPECT_EQ(evaluate_reference_route_policy(input).action, route_attestation_action::allow);

    input.attestation_required = true;
    input.route_is_local = true;
    EXPECT_EQ(evaluate_reference_route_policy(input).action, route_attestation_action::allow);

    input.route_is_local = false;
    input.may_start_handshake = true;
    input.state.status = route_attestation_status::unknown;
    auto add_ref_decision = evaluate_reference_route_policy(input);
    EXPECT_EQ(add_ref_decision.action, route_attestation_action::start_handshake);

    input.state.status = route_attestation_status::handshaking;
    auto concurrent_add_ref_decision = evaluate_reference_route_policy(input);
    EXPECT_EQ(concurrent_add_ref_decision.action, route_attestation_action::wait_for_handshake);

    input.state.status = route_attestation_status::unknown;
    input.may_start_handshake = false;
    auto existing_control_decision = evaluate_reference_route_policy(input);
    EXPECT_EQ(existing_control_decision.action, route_attestation_action::reject);
    EXPECT_NE(existing_control_decision.reason.find("completed add_ref"), std::string::npos)
        << existing_control_decision.reason;

    input.state.status = route_attestation_status::unattested_allowed;
    EXPECT_EQ(evaluate_reference_route_policy(input).action, route_attestation_action::allow);

    input.state.status = route_attestation_status::failed;
    input.state.failure_reason = "policy rejected route";
    auto failed_decision = evaluate_reference_route_policy(input);
    EXPECT_EQ(failed_decision.action, route_attestation_action::reject);
    EXPECT_EQ(failed_decision.reason, input.state.failure_reason);
}

TEST(
    AttestationService,
    MissingPeerEvidenceRequiresExplicitTwoPartPolicy)
{
    attestation_policy policy;

    auto default_decision = evaluate_missing_peer_evidence_policy(policy);
    EXPECT_FALSE(default_decision.accepted);

    policy.allow_unattested_peer = true;
    auto still_rejected = evaluate_missing_peer_evidence_policy(policy);
    EXPECT_FALSE(still_rejected.accepted);

    policy.require_peer_evidence = false;
    auto accepted = evaluate_missing_peer_evidence_policy(policy);
    EXPECT_TRUE(accepted.accepted);
}

TEST(
    AttestationService,
    ZoneSecurityPolicyOwnsServiceSpecificRouteAdmission)
{
    zone_security_policy_options options;
    options.reference_routes_require_attestation = true;
    options.peer_attestation_policy.require_peer_evidence = false;
    options.peer_attestation_policy.allow_unattested_peer = true;

    zone_security_policy policy(std::move(options));

    reference_route_policy_input input;
    input.state.status = route_attestation_status::unknown;
    input.may_start_handshake = true;
    EXPECT_EQ(policy.evaluate_reference_route(input).action, route_attestation_action::start_handshake);

    input.may_start_handshake = false;
    EXPECT_EQ(policy.evaluate_reference_route(input).action, route_attestation_action::reject);

    EXPECT_TRUE(policy.evaluate_missing_peer_evidence().accepted);

    policy.set_reference_routes_require_attestation(false);
    EXPECT_EQ(policy.evaluate_reference_route(input).action, route_attestation_action::allow);
}

TEST(
    AttestationService,
    GeneratesFixedSizeAttestationNonces)
{
    auto nonce = make_attestation_nonce();
    ASSERT_TRUE(nonce.has_value());
    EXPECT_EQ(nonce->size(), canopy::security::attestation::attestation_nonce_size);
}

TEST(
    AttestationService,
    RouteHandshakePayloadsUseGeneratedTypeIdsAndAgreedEncoding)
{
    rpc::route_attestation_handshake_request request;
    request.transcript_id = 42;
    request.claimant.enclave_id = "enclave-a";
    request.claimant.zone_id = "zone-a";
    request.backend_id = fake_backend_id;
    request.evidence = rpc::attestation_cmw{};
    request.evidence->media_type = fake_evidence_media_type;
    request.evidence->content_format = fake_evidence_content_format;
    request.evidence->payload = {1, 2, 3};
    request.verifier_challenge = rpc::attestation_cmw{};
    request.verifier_challenge->media_type = simulation_evidence_media_type;
    request.verifier_challenge->content_format = canopy::security::attestation::simulation_local_challenge_content_format;
    request.verifier_challenge->payload = {9, 9, 9};
    request.nonce = {4, 5, 6};

    auto request_type_id = rpc::id<rpc::route_attestation_handshake_request>::get(rpc::get_version());
    auto response_type_id = rpc::id<rpc::route_attestation_handshake_response>::get(rpc::get_version());
    EXPECT_NE(request_type_id, 0U);
    EXPECT_NE(response_type_id, 0U);
    EXPECT_NE(request_type_id, response_type_id);

    rpc::route_attestation_handshake_response response;
    response.transcript_id = request.transcript_id;
    response.accepted = true;
    response.reason = "accepted";
    response.responder.enclave_id = "enclave-b";
    response.responder.zone_id = "zone-b";
    response.backend_id = fake_backend_id;
    response.security_level = static_cast<uint64_t>(security_level::development);
    response.session_epoch = 1;
    response.evidence = request.evidence;
    response.nonce = {7, 8, 9};

    for (auto encoding : agreed_payload_encodings())
    {
        SCOPED_TRACE(static_cast<uint64_t>(encoding));

        auto bytes = rpc::serialise<std::vector<char>>(request, encoding);
        rpc::route_attestation_handshake_request decoded_request;
        EXPECT_TRUE(rpc::deserialise(encoding, rpc::byte_span(bytes), decoded_request).empty());
        EXPECT_EQ(decoded_request, request);

        auto response_bytes = rpc::serialise<std::vector<char>>(response, encoding);
        rpc::route_attestation_handshake_response decoded_response;
        EXPECT_TRUE(rpc::deserialise(encoding, rpc::byte_span(response_bytes), decoded_response).empty());
        EXPECT_EQ(decoded_response, response);

        auto request_without_evidence = request;
        request_without_evidence.evidence.reset();
        auto no_evidence_bytes = rpc::serialise<std::vector<char>>(request_without_evidence, encoding);
        rpc::route_attestation_handshake_request decoded_no_evidence_request;
        EXPECT_TRUE(rpc::deserialise(encoding, rpc::byte_span(no_evidence_bytes), decoded_no_evidence_request).empty());
        EXPECT_EQ(decoded_no_evidence_request, request_without_evidence);

        auto request_without_challenge = request;
        request_without_challenge.verifier_challenge.reset();
        auto no_challenge_bytes = rpc::serialise<std::vector<char>>(request_without_challenge, encoding);
        rpc::route_attestation_handshake_request decoded_no_challenge_request;
        EXPECT_TRUE(rpc::deserialise(encoding, rpc::byte_span(no_challenge_bytes), decoded_no_challenge_request).empty());
        EXPECT_EQ(decoded_no_challenge_request, request_without_challenge);
    }
}

TEST(
    AttestationService,
    RouteHandshakeRejectsPresentOptionalEvidenceWrapperWithoutValue)
{
    const std::vector<char> malformed_request = {
        static_cast<char>((4U << 3U) | 2U),
        0,
    };
    const std::vector<char> malformed_response = {
        static_cast<char>((8U << 3U) | 2U),
        0,
    };

    auto expect_rejected = [](rpc::encoding encoding, const std::vector<char>& bytes, auto& decoded)
    {
        const auto error = rpc::deserialise(encoding, rpc::byte_span(bytes), decoded);
        EXPECT_FALSE(error.empty());
        EXPECT_NE(error.find("wrapper is present without value"), std::string::npos) << error;
    };

#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
    {
        rpc::route_attestation_handshake_request decoded_request;
        expect_rejected(rpc::encoding::protocol_buffers, malformed_request, decoded_request);

        rpc::route_attestation_handshake_response decoded_response;
        expect_rejected(rpc::encoding::protocol_buffers, malformed_response, decoded_response);
    }
#endif

#ifdef CANOPY_BUILD_NANOPB
    {
        rpc::route_attestation_handshake_request decoded_request;
        expect_rejected(rpc::encoding::nanopb, malformed_request, decoded_request);

        rpc::route_attestation_handshake_response decoded_response;
        expect_rejected(rpc::encoding::nanopb, malformed_response, decoded_response);
    }
#endif
}

TEST(
    AttestationService,
    KdfGoldenVectorIsStable)
{
    auto service = make_service("enclave-a", "zone-a");
    const auto context = establish(service, identity{"enclave-b", "zone-b"});
    auto scope = make_scope(context, identity{"enclave-a", "zone-a"}, identity{"enclave-b", "zone-b"});

    auto material = service->derive_aead_key(scope);
    ASSERT_TRUE(material.has_value());

    const std::array<uint8_t, canopy::security::attestation::aead_key_size> expected_key{0x1c,
        0x32,
        0x9c,
        0xd2,
        0x64,
        0x3f,
        0x71,
        0xce,
        0xde,
        0x2d,
        0x51,
        0xf9,
        0x1c,
        0xbe,
        0xfb,
        0xc5,
        0x64,
        0x9a,
        0xc7,
        0x5d,
        0xf9,
        0x32,
        0xe3,
        0x13,
        0x63,
        0x70,
        0x0f,
        0xc6,
        0x4f,
        0x8d,
        0xb1,
        0xe1};
    const std::array<uint8_t, canopy::security::attestation::aead_nonce_prefix_size> expected_nonce_prefix{
        0xe3, 0x48, 0x7d, 0x60};
    const std::array<uint8_t, canopy::security::attestation::aead_nonce_size> expected_nonce{
        0xe3, 0x48, 0x7d, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07};

    EXPECT_EQ(material->key, expected_key);
    EXPECT_EQ(material->nonce_prefix, expected_nonce_prefix);
    EXPECT_EQ(service->make_aead_nonce(*material, 7), expected_nonce);
}

TEST(
    AttestationService,
    SessionEstablishmentDoesNotResetCountersForSameEpoch)
{
    auto service = make_service("enclave-a", "zone-a");
    const std::vector<uint8_t> shared_secret{1, 2, 3, 4};
    const auto context = establish(service, identity{"enclave-b", "zone-b"}, 99, shared_secret);
    ASSERT_TRUE(context.established);

    auto scope = make_scope(context, identity{"enclave-a", "zone-a"}, identity{"enclave-b", "zone-b"});
    auto first_counter = service->next_send_counter(scope);
    ASSERT_TRUE(first_counter.accepted);
    EXPECT_EQ(first_counter.counter, 1U);

    auto repeated_context = establish(service, identity{"enclave-b", "zone-b"}, 99, shared_secret);
    EXPECT_TRUE(repeated_context.established);

    auto second_counter = service->next_send_counter(scope);
    ASSERT_TRUE(second_counter.accepted);
    EXPECT_EQ(second_counter.counter, 2U);

    auto conflicting_context = establish(service, identity{"enclave-b", "zone-b"}, 99, {9, 9, 9, 9});
    EXPECT_FALSE(conflicting_context.established);

    establish_session_params rekey_params;
    rekey_params.peer_identity = identity{"enclave-b", "zone-b"};
    rekey_params.transcript_id = 99;
    rekey_params.local_evidence_sent = true;
    rekey_params.peer_attested = true;
    rekey_params.verified_backend_id = fake_backend_id;
    rekey_params.verified_level = security_level::development;
    rekey_params.session_epoch = 2;
    rekey_params.shared_secret = shared_secret;

    auto rekeyed_context = service->establish_session(rekey_params);
    ASSERT_TRUE(rekeyed_context.established);
    auto rekeyed_scope = make_scope(rekeyed_context, identity{"enclave-a", "zone-a"}, identity{"enclave-b", "zone-b"});
    auto rekeyed_counter = service->next_send_counter(rekeyed_scope);
    ASSERT_TRUE(rekeyed_counter.accepted);
    EXPECT_EQ(rekeyed_counter.counter, 1U);
}

TEST(
    AttestationService,
    HardwareGradeSessionRequiresExplicitSharedSecret)
{
    auto service = make_service("enclave-a", "zone-a");

    establish_session_params params;
    params.peer_identity = identity{"enclave-b", "zone-b"};
    params.transcript_id = 99;
    params.local_evidence_sent = true;
    params.peer_attested = true;
    params.verified_backend_id = sgx_epid_backend_id;
    params.verified_level = security_level::hardware_legacy;

    auto no_secret_context = service->establish_session(params);
    EXPECT_FALSE(no_secret_context.established);

    params.shared_secret = {1, 2, 3, 4};
    auto explicit_secret_context = service->establish_session(params);
    EXPECT_TRUE(explicit_secret_context.established);
}

TEST(
    AttestationService,
    AesGcmWrapperHandlesEmptyPlaintextAndAad)
{
    auto service = make_service("enclave-a", "zone-a");
    const auto context = establish(service, identity{"enclave-b", "zone-b"});
    auto scope = make_scope(context, identity{"enclave-a", "zone-a"}, identity{"enclave-b", "zone-b"});

    auto material = service->derive_aead_key(scope);
    ASSERT_TRUE(material.has_value());
    auto nonce = service->make_aead_nonce(*material, 1);

    const std::vector<uint8_t> empty_plaintext;
    const std::vector<uint8_t> empty_aad;
    auto ciphertext = encrypt_aes_256_gcm(*material, nonce, empty_plaintext, empty_aad);
    ASSERT_TRUE(ciphertext.has_value());
    EXPECT_TRUE(ciphertext->payload.empty());
    EXPECT_EQ(ciphertext->authentication_tag.size(), aead_aes_256_gcm_tag_size);

    auto decrypted = decrypt_aes_256_gcm(*material, nonce, ciphertext->payload, ciphertext->authentication_tag, empty_aad);
    ASSERT_TRUE(decrypted.has_value());
    EXPECT_TRUE(decrypted->empty());

    auto tampered_tag = ciphertext->authentication_tag;
    ASSERT_FALSE(tampered_tag.empty());
    tampered_tag.back() ^= 0x01;
    EXPECT_FALSE(decrypt_aes_256_gcm(*material, nonce, ciphertext->payload, tampered_tag, empty_aad).has_value());
}

TEST(
    AttestationService,
    DerivesMatchingDirectionalKeysForBothPeers)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");

    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"});
    const auto context_b = establish(service_b, identity{"enclave-a", "zone-a"});
    ASSERT_EQ(context_a.session_id, context_b.session_id);

    auto request_scope = make_scope(context_a, identity{"enclave-a", "zone-a"}, identity{"enclave-b", "zone-b"});
    auto a_request_key = service_a->derive_aead_key(request_scope);
    auto b_request_key = service_b->derive_aead_key(request_scope);
    ASSERT_TRUE(a_request_key.has_value());
    ASSERT_TRUE(b_request_key.has_value());
    EXPECT_EQ(a_request_key->key, b_request_key->key);
    EXPECT_EQ(a_request_key->nonce_prefix, b_request_key->nonce_prefix);
    EXPECT_EQ(a_request_key->session_epoch, 1U);

    auto response_scope = make_scope(
        context_a,
        identity{"enclave-a", "zone-a"},
        identity{"enclave-b", "zone-b"},
        protected_rpc_direction::destination_to_caller);
    auto response_key = service_a->derive_aead_key(response_scope);
    ASSERT_TRUE(response_key.has_value());
    EXPECT_TRUE(response_key->key != a_request_key->key || response_key->nonce_prefix != a_request_key->nonce_prefix);

    auto sibling_zone_scope = make_scope(context_a, identity{"enclave-a", "zone-a-2"}, identity{"enclave-b", "zone-b"});
    auto sibling_zone_key = service_a->derive_aead_key(sibling_zone_scope);
    ASSERT_TRUE(sibling_zone_key.has_value());
    EXPECT_TRUE(
        sibling_zone_key->key != a_request_key->key || sibling_zone_key->nonce_prefix != a_request_key->nonce_prefix);

    auto nonce = service_a->make_aead_nonce(*a_request_key, 7);
    EXPECT_EQ(
        std::vector<uint8_t>(nonce.begin(), nonce.begin() + a_request_key->nonce_prefix.size()),
        std::vector<uint8_t>(a_request_key->nonce_prefix.begin(), a_request_key->nonce_prefix.end()));
    EXPECT_EQ(nonce.back(), 7U);
}

TEST(
    AttestationService,
    AllocatesAndValidatesMonotonicCountersPerDerivedKey)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"});
    establish(service_b, identity{"enclave-a", "zone-a"});

    auto scope = make_scope(context_a, identity{"enclave-a", "zone-a"}, identity{"enclave-b", "zone-b"});

    auto first_send = service_a->next_send_counter(scope);
    auto second_send = service_a->next_send_counter(scope);
    ASSERT_TRUE(first_send.accepted);
    ASSERT_TRUE(second_send.accepted);
    EXPECT_EQ(first_send.counter, 1U);
    EXPECT_EQ(second_send.counter, 2U);

    EXPECT_FALSE(service_b->accept_receive_counter(scope, 0).accepted);

    auto first_receive = service_b->accept_receive_counter(scope, first_send.counter);
    auto replayed_receive = service_b->accept_receive_counter(scope, first_send.counter);
    auto second_receive = service_b->accept_receive_counter(scope, second_send.counter);
    auto out_of_order_receive = service_b->accept_receive_counter(scope, second_send.counter);
    EXPECT_TRUE(first_receive.accepted);
    EXPECT_FALSE(replayed_receive.accepted);
    EXPECT_TRUE(second_receive.accepted);
    EXPECT_FALSE(out_of_order_receive.accepted);
}

TEST(
    AttestationService,
    RejectsCounterExhaustionAndMismatchedScopes)
{
    auto service = make_service("enclave-a", "zone-a", 2);
    const auto context = establish(service, identity{"enclave-b", "zone-b"});
    auto scope = make_scope(context, identity{"enclave-a", "zone-a"}, identity{"enclave-b", "zone-b"});

    EXPECT_TRUE(service->next_send_counter(scope).accepted);
    EXPECT_TRUE(service->next_send_counter(scope).accepted);
    EXPECT_FALSE(service->next_send_counter(scope).accepted);
    EXPECT_FALSE(service->accept_receive_counter(scope, 3).accepted);

    auto attacker_scope = make_scope(context, identity{"enclave-a", "zone-a"}, identity{"attacker", "zone-b"});
    EXPECT_FALSE(service->derive_aead_key(attacker_scope).has_value());
    EXPECT_FALSE(service->next_send_counter(attacker_scope).accepted);
}

TEST(
    AttestationService,
    ProtectsSendRequestAndResponse)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"});
    const auto context_b = establish(service_b, identity{"enclave-a", "zone-a"});

    auto caller_zone = make_zone(10);
    auto destination_zone = make_zone(20);
    auto remote_object = destination_zone.with_object(rpc::object(7));
    ASSERT_TRUE(remote_object.has_value());

    rpc::send_params params;
    params.protocol_version = rpc::get_version();
    params.encoding_type = rpc::encoding::yas_binary;
    params.tag = 1234;
    params.caller_zone_id = caller_zone;
    params.remote_object_id = *remote_object;
    params.interface_id = rpc::interface_ordinal(0x445566);
    params.method_id = rpc::method(9);
    params.in_data = {'h', 'e', 'l', 'l', 'o'};
    params.in_back_channel.push_back(rpc::back_channel_entry{42, {1, 2, 3}});
    params.request_id = 77;

    auto protected_request = protect_send_request(*service_a, context_a, params);
    ASSERT_TRUE(protected_request.accepted) << protected_request.error.reason;
    EXPECT_EQ(
        protected_request.value.params.interface_id,
        canopy::security::attestation::encrypted_payload_interface_id(rpc::get_version()));
    EXPECT_EQ(protected_request.value.params.method_id, rpc::method(0));
    EXPECT_EQ(protected_request.value.params.encoding_type, params.encoding_type);
    EXPECT_EQ(protected_request.value.params.remote_object_id.get_object_id().get_val(), 0U);
    EXPECT_NE(protected_request.value.params.in_data, params.in_data);

    auto unprotected_request = unprotect_send_request(*service_b, protected_request.value.params);
    ASSERT_TRUE(unprotected_request.accepted) << unprotected_request.error.reason;
    EXPECT_EQ(unprotected_request.value.params.protocol_version, params.protocol_version);
    EXPECT_EQ(unprotected_request.value.params.encoding_type, params.encoding_type);
    EXPECT_EQ(unprotected_request.value.params.tag, 0U);
    EXPECT_EQ(unprotected_request.value.params.caller_zone_id, params.caller_zone_id);
    EXPECT_EQ(unprotected_request.value.params.remote_object_id, params.remote_object_id);
    EXPECT_EQ(unprotected_request.value.params.interface_id, params.interface_id);
    EXPECT_EQ(unprotected_request.value.params.method_id, params.method_id);
    EXPECT_EQ(unprotected_request.value.params.in_data, params.in_data);
    ASSERT_EQ(unprotected_request.value.params.in_back_channel.size(), 1U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].type_id, 42U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].payload, std::vector<uint8_t>({1, 2, 3}));
    EXPECT_EQ(unprotected_request.value.params.request_id, params.request_id);

    auto replayed_request = unprotect_send_request(*service_b, protected_request.value.params);
    EXPECT_FALSE(replayed_request.accepted);

    rpc::send_result response;
    response.error_code = rpc::error::INVALID_METHOD_ID();
    response.out_buf = {'o', 'k'};
    response.out_back_channel.push_back(rpc::back_channel_entry{99, {4, 5, 6}});

    auto protected_response = protect_send_response(
        *service_b, context_b, protected_request.value.params, unprotected_request.value.request_counter, std::move(response));
    ASSERT_TRUE(protected_response.accepted) << protected_response.error.reason;
    EXPECT_EQ(protected_response.value.error_code, rpc::error::OK());
    EXPECT_FALSE(protected_response.value.out_buf.empty());

    auto unprotected_response = unprotect_send_response(
        *service_a,
        context_a,
        protected_request.value.params,
        protected_request.value.request_counter,
        std::move(protected_response.value));
    ASSERT_TRUE(unprotected_response.accepted) << unprotected_response.error.reason;
    EXPECT_EQ(unprotected_response.value.error_code, rpc::error::INVALID_METHOD_ID());
    EXPECT_EQ(unprotected_response.value.out_buf, std::vector<char>({'o', 'k'}));
    ASSERT_EQ(unprotected_response.value.out_back_channel.size(), 1U);
    EXPECT_EQ(unprotected_response.value.out_back_channel[0].type_id, 99U);
    EXPECT_EQ(unprotected_response.value.out_back_channel[0].payload, std::vector<uint8_t>({4, 5, 6}));
}

TEST(
    AttestationService,
    ProtectsPostRequestWithAgreedEncoding)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"}, 108);
    establish(service_b, identity{"enclave-a", "zone-a"}, 108);

    auto caller_zone = make_zone(18);
    auto destination_zone = make_zone(28);
    auto remote_object = destination_zone.with_object(rpc::object(12));
    ASSERT_TRUE(remote_object.has_value());

    rpc::post_params params;
    params.protocol_version = rpc::get_version();
    params.encoding_type = rpc::encoding::yas_binary;
    params.tag = 0x5151;
    params.caller_zone_id = caller_zone;
    params.remote_object_id = *remote_object;
    params.interface_id = rpc::interface_ordinal(0x1113);
    params.method_id = rpc::method(11);
    params.in_data = {'p', 'o', 's', 't'};
    params.in_back_channel.push_back(rpc::back_channel_entry{52, {5, 2}});

    auto protected_request = protect_post_request(*service_a, context_a, params);
    ASSERT_TRUE(protected_request.accepted) << protected_request.error.reason;
    EXPECT_EQ(protected_request.value.params.encoding_type, params.encoding_type);
    EXPECT_EQ(
        protected_request.value.params.interface_id,
        canopy::security::attestation::encrypted_payload_interface_id(rpc::get_version()));
    EXPECT_EQ(protected_request.value.params.method_id, rpc::method(0));
    EXPECT_NE(protected_request.value.params.in_data, params.in_data);

    auto unprotected_request = unprotect_post_request(*service_b, protected_request.value.params);
    ASSERT_TRUE(unprotected_request.accepted) << unprotected_request.error.reason;
    EXPECT_EQ(unprotected_request.value.params.protocol_version, params.protocol_version);
    EXPECT_EQ(unprotected_request.value.params.encoding_type, params.encoding_type);
    EXPECT_EQ(unprotected_request.value.params.tag, 0U);
    EXPECT_EQ(unprotected_request.value.params.caller_zone_id, params.caller_zone_id);
    EXPECT_EQ(unprotected_request.value.params.remote_object_id, params.remote_object_id);
    EXPECT_EQ(unprotected_request.value.params.interface_id, params.interface_id);
    EXPECT_EQ(unprotected_request.value.params.method_id, params.method_id);
    EXPECT_EQ(unprotected_request.value.params.in_data, params.in_data);
    ASSERT_EQ(unprotected_request.value.params.in_back_channel.size(), 1U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].type_id, 52U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].payload, std::vector<uint8_t>({5, 2}));
}

#ifdef CANOPY_BUILD_PROTOCOL_BUFFERS
TEST(
    AttestationService,
    ProtectsSendAndPostUsingProtocolBuffersCarrier)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"}, 109);
    const auto context_b = establish(service_b, identity{"enclave-a", "zone-a"}, 109);

    auto caller_zone = make_zone(19);
    auto destination_zone = make_zone(29);
    auto remote_object = destination_zone.with_object(rpc::object(13));
    ASSERT_TRUE(remote_object.has_value());

    rpc::send_params send_params;
    send_params.protocol_version = rpc::get_version();
    send_params.encoding_type = rpc::encoding::protocol_buffers;
    send_params.tag = 0x6262;
    send_params.caller_zone_id = caller_zone;
    send_params.remote_object_id = *remote_object;
    send_params.interface_id = rpc::interface_ordinal(0x1114);
    send_params.method_id = rpc::method(12);
    send_params.in_data = {'p', 'b'};
    send_params.request_id = 19;

    auto protected_send = protect_send_request(*service_a, context_a, send_params);
    ASSERT_TRUE(protected_send.accepted) << protected_send.error.reason;
    EXPECT_EQ(protected_send.value.params.encoding_type, rpc::encoding::protocol_buffers);

    auto unprotected_send = unprotect_send_request(*service_b, protected_send.value.params);
    ASSERT_TRUE(unprotected_send.accepted) << unprotected_send.error.reason;
    EXPECT_EQ(unprotected_send.value.params.encoding_type, send_params.encoding_type);
    EXPECT_EQ(unprotected_send.value.params.in_data, send_params.in_data);

    rpc::send_result response;
    response.error_code = rpc::error::OK();
    response.out_buf = {'o', 'u', 't'};
    auto protected_response = protect_send_response(
        *service_b, context_b, protected_send.value.params, unprotected_send.value.request_counter, std::move(response));
    ASSERT_TRUE(protected_response.accepted) << protected_response.error.reason;

    auto unprotected_response = unprotect_send_response(
        *service_a,
        context_a,
        protected_send.value.params,
        protected_send.value.request_counter,
        std::move(protected_response.value));
    ASSERT_TRUE(unprotected_response.accepted) << unprotected_response.error.reason;
    EXPECT_EQ(unprotected_response.value.out_buf, std::vector<char>({'o', 'u', 't'}));

    rpc::post_params post_params;
    post_params.protocol_version = rpc::get_version();
    post_params.encoding_type = rpc::encoding::protocol_buffers;
    post_params.tag = 0x6363;
    post_params.caller_zone_id = caller_zone;
    post_params.remote_object_id = *remote_object;
    post_params.interface_id = rpc::interface_ordinal(0x1115);
    post_params.method_id = rpc::method(13);
    post_params.in_data = {'p', 'b', 'p'};

    auto protected_post = protect_post_request(*service_a, context_a, post_params);
    ASSERT_TRUE(protected_post.accepted) << protected_post.error.reason;
    EXPECT_EQ(protected_post.value.params.encoding_type, rpc::encoding::protocol_buffers);

    auto unprotected_post = unprotect_post_request(*service_b, protected_post.value.params);
    ASSERT_TRUE(unprotected_post.accepted) << unprotected_post.error.reason;
    EXPECT_EQ(unprotected_post.value.params.encoding_type, post_params.encoding_type);
    EXPECT_EQ(unprotected_post.value.params.in_data, post_params.in_data);
}
#endif

TEST(
    AttestationService,
    ProtectsPositiveApplicationSendResult)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"}, 106);
    const auto context_b = establish(service_b, identity{"enclave-a", "zone-a"}, 106);

    auto caller_zone = make_zone(16);
    auto destination_zone = make_zone(26);
    auto remote_object = destination_zone.with_object(rpc::object(11));
    ASSERT_TRUE(remote_object.has_value());

    rpc::send_params params;
    params.protocol_version = rpc::get_version();
    params.encoding_type = rpc::encoding::yas_binary;
    params.tag = 0x4242;
    params.caller_zone_id = caller_zone;
    params.remote_object_id = *remote_object;
    params.interface_id = rpc::interface_ordinal(0x1111);
    params.method_id = rpc::method(9);
    params.in_data = {'a', 'p', 'p'};
    params.request_id = 16;

    auto protected_request = protect_send_request(*service_a, context_a, params);
    ASSERT_TRUE(protected_request.accepted) << protected_request.error.reason;

    auto unprotected_request = unprotect_send_request(*service_b, protected_request.value.params);
    ASSERT_TRUE(unprotected_request.accepted) << unprotected_request.error.reason;

    constexpr int application_result_code = 42;
    rpc::send_result response;
    response.error_code = application_result_code;
    response.out_buf = {'r', 'e', 's'};

    auto protected_response = protect_send_response(
        *service_b, context_b, protected_request.value.params, unprotected_request.value.request_counter, std::move(response));
    ASSERT_TRUE(protected_response.accepted) << protected_response.error.reason;
    EXPECT_EQ(protected_response.value.error_code, rpc::error::OK());
    EXPECT_NE(protected_response.value.out_buf, std::vector<char>({'r', 'e', 's'}));

    auto unprotected_response = unprotect_send_response(
        *service_a,
        context_a,
        protected_request.value.params,
        protected_request.value.request_counter,
        std::move(protected_response.value));
    ASSERT_TRUE(unprotected_response.accepted) << unprotected_response.error.reason;
    EXPECT_EQ(unprotected_response.value.error_code, application_result_code);
    EXPECT_EQ(unprotected_response.value.out_buf, std::vector<char>({'r', 'e', 's'}));
}

TEST(
    AttestationService,
    RejectsPositiveProtectedSendCarrierStatus)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"}, 107);
    establish(service_b, identity{"enclave-a", "zone-a"}, 107);

    auto caller_zone = make_zone(17);
    auto destination_zone = make_zone(27);
    auto remote_object = destination_zone.with_object(rpc::object(12));
    ASSERT_TRUE(remote_object.has_value());

    rpc::send_params params;
    params.protocol_version = rpc::get_version();
    params.encoding_type = rpc::encoding::yas_binary;
    params.tag = 0x4243;
    params.caller_zone_id = caller_zone;
    params.remote_object_id = *remote_object;
    params.interface_id = rpc::interface_ordinal(0x1112);
    params.method_id = rpc::method(10);
    params.in_data = {'a', 'p', 'p'};
    params.request_id = 17;

    auto protected_request = protect_send_request(*service_a, context_a, params);
    ASSERT_TRUE(protected_request.accepted) << protected_request.error.reason;

    rpc::send_result route_failure;
    route_failure.error_code = rpc::error::ZONE_NOT_FOUND();
    auto accepted_route_failure = unprotect_send_response(
        *service_a, context_a, protected_request.value.params, protected_request.value.request_counter, std::move(route_failure));
    ASSERT_TRUE(accepted_route_failure.accepted) << accepted_route_failure.error.reason;
    EXPECT_EQ(accepted_route_failure.value.error_code, rpc::error::ZONE_NOT_FOUND());

    constexpr int application_result_code = 42;
    rpc::send_result exposed_application_result;
    exposed_application_result.error_code = application_result_code;
    exposed_application_result.out_buf = {'r', 'e', 's'};

    auto rejected_application_result = unprotect_send_response(
        *service_a,
        context_a,
        protected_request.value.params,
        protected_request.value.request_counter,
        std::move(exposed_application_result));
    EXPECT_FALSE(rejected_application_result.accepted);
    EXPECT_EQ(rejected_application_result.error.error_code, rpc::error::PROTOCOL_ERROR());
}

TEST(
    AttestationService,
    ProtectedSendRejectsTamperedCiphertext)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"}, 101);
    establish(service_b, identity{"enclave-a", "zone-a"}, 101);

    auto caller_zone = make_zone(11);
    auto destination_zone = make_zone(22);
    auto remote_object = destination_zone.with_object(rpc::object(8));
    ASSERT_TRUE(remote_object.has_value());

    rpc::send_params params;
    params.protocol_version = rpc::get_version();
    params.encoding_type = rpc::encoding::yas_binary;
    params.tag = 5678;
    params.caller_zone_id = caller_zone;
    params.remote_object_id = *remote_object;
    params.interface_id = rpc::interface_ordinal(0x112233);
    params.method_id = rpc::method(4);
    params.in_data = {'x'};
    params.request_id = 12;

    auto protected_request = protect_send_request(*service_a, context_a, params);
    ASSERT_TRUE(protected_request.accepted) << protected_request.error.reason;
    ASSERT_FALSE(protected_request.value.params.in_data.empty());

    protected_request.value.params.in_data.back() ^= 0x01;
    auto unprotected_request = unprotect_send_request(*service_b, protected_request.value.params);
    EXPECT_FALSE(unprotected_request.accepted);
    EXPECT_EQ(unprotected_request.error.error_code, rpc::error::FRAUDULANT_REQUEST());
}

TEST(
    AttestationService,
    ProtectsAddRefRequest)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"}, 202);
    establish(service_b, identity{"enclave-a", "zone-a"}, 202);

    auto caller_zone = make_zone(31);
    auto destination_zone = make_zone(41);
    auto requesting_zone = make_zone(51);
    auto remote_object = destination_zone.with_object(rpc::object(12));
    ASSERT_TRUE(remote_object.has_value());

    rpc::add_ref_params params;
    params.protocol_version = rpc::get_version();
    params.remote_object_id = *remote_object;
    params.caller_zone_id = caller_zone;
    params.requesting_zone_id = requesting_zone;
    params.build_out_param_channel = rpc::add_ref_options::build_caller_route | rpc::add_ref_options::optimistic;
    params.in_back_channel.push_back(rpc::back_channel_entry{12, {7, 8, 9}});
    params.request_id = 88;
    const auto payload_type_id = 0xabcdefU;
    const auto payload_encoding = rpc::encoding::yas_binary;
    const auto payload_bytes = std::vector<char>{'a', 'd', 'd'};
    params.payload = make_raw_payload(payload_type_id, payload_encoding, payload_bytes);

    auto protected_request = protect_add_ref_request(*service_a, context_a, params);
    ASSERT_TRUE(protected_request.accepted) << protected_request.error.reason;
    expect_protected_payload(protected_request.value.params.payload, payload_encoding);
    EXPECT_EQ(protected_request.value.params.remote_object_id.get_object_id().get_val(), 0U);
    EXPECT_NE(protected_request.value.params.payload->get_payload(), payload_bytes);

    auto tampered_options = protected_request.value.params;
    tampered_options.build_out_param_channel = rpc::add_ref_options::build_caller_route;
    auto tampered_options_result = unprotect_add_ref_request(*service_b, tampered_options);
    EXPECT_FALSE(tampered_options_result.accepted);
    EXPECT_EQ(tampered_options_result.error.error_code, rpc::error::FRAUDULANT_REQUEST());

    auto tampered_requesting_zone = protected_request.value.params;
    tampered_requesting_zone.requesting_zone_id = make_zone(52);
    auto tampered_requesting_zone_result = unprotect_add_ref_request(*service_b, tampered_requesting_zone);
    EXPECT_FALSE(tampered_requesting_zone_result.accepted);
    EXPECT_EQ(tampered_requesting_zone_result.error.error_code, rpc::error::FRAUDULANT_REQUEST());

    auto unprotected_request = unprotect_add_ref_request(*service_b, protected_request.value.params);
    ASSERT_TRUE(unprotected_request.accepted) << unprotected_request.error.reason;
    EXPECT_EQ(unprotected_request.value.params.protocol_version, params.protocol_version);
    EXPECT_EQ(unprotected_request.value.params.remote_object_id, params.remote_object_id);
    EXPECT_EQ(unprotected_request.value.params.caller_zone_id, params.caller_zone_id);
    EXPECT_EQ(unprotected_request.value.params.requesting_zone_id, params.requesting_zone_id);
    EXPECT_EQ(unprotected_request.value.params.build_out_param_channel, params.build_out_param_channel);
    ASSERT_EQ(unprotected_request.value.params.in_back_channel.size(), 1U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].type_id, 12U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].payload, std::vector<uint8_t>({7, 8, 9}));
    EXPECT_EQ(unprotected_request.value.params.request_id, params.request_id);
    expect_payload(unprotected_request.value.params.payload, payload_type_id, payload_encoding, payload_bytes);

    auto replayed_request = unprotect_add_ref_request(*service_b, protected_request.value.params);
    EXPECT_FALSE(replayed_request.accepted);
}

TEST(
    AttestationService,
    ProtectsReleaseRequest)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"}, 222);
    establish(service_b, identity{"enclave-a", "zone-a"}, 222);

    auto caller_zone = make_zone(32);
    auto destination_zone = make_zone(42);
    auto remote_object = destination_zone.with_object(rpc::object(22));
    ASSERT_TRUE(remote_object.has_value());

    rpc::release_params params;
    params.protocol_version = rpc::get_version();
    params.remote_object_id = *remote_object;
    params.caller_zone_id = caller_zone;
    params.options = rpc::release_options::optimistic;
    params.in_back_channel.push_back(rpc::back_channel_entry{21, {1, 3, 5}});
    const auto payload_type_id = 0xbcdef0U;
    const auto payload_encoding = rpc::encoding::yas_binary;
    const auto payload_bytes = std::vector<char>{'r', 'e', 'l'};
    params.payload = make_raw_payload(payload_type_id, payload_encoding, payload_bytes);

    auto protected_request = protect_release_request(*service_a, context_a, params);
    ASSERT_TRUE(protected_request.accepted) << protected_request.error.reason;
    expect_protected_payload(protected_request.value.params.payload, payload_encoding);
    EXPECT_EQ(protected_request.value.params.remote_object_id.get_object_id().get_val(), 0U);
    EXPECT_NE(protected_request.value.params.payload->get_payload(), payload_bytes);

    auto tampered_options = protected_request.value.params;
    tampered_options.options = rpc::release_options::normal;
    auto tampered_options_result = unprotect_release_request(*service_b, tampered_options);
    EXPECT_FALSE(tampered_options_result.accepted);
    EXPECT_EQ(tampered_options_result.error.error_code, rpc::error::FRAUDULANT_REQUEST());

    auto unprotected_request = unprotect_release_request(*service_b, protected_request.value.params);
    ASSERT_TRUE(unprotected_request.accepted) << unprotected_request.error.reason;
    EXPECT_EQ(unprotected_request.value.params.protocol_version, params.protocol_version);
    EXPECT_EQ(unprotected_request.value.params.remote_object_id, params.remote_object_id);
    EXPECT_EQ(unprotected_request.value.params.caller_zone_id, params.caller_zone_id);
    EXPECT_EQ(unprotected_request.value.params.options, params.options);
    ASSERT_EQ(unprotected_request.value.params.in_back_channel.size(), 1U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].type_id, 21U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].payload, std::vector<uint8_t>({1, 3, 5}));
    expect_payload(unprotected_request.value.params.payload, payload_type_id, payload_encoding, payload_bytes);

    auto replayed_request = unprotect_release_request(*service_b, protected_request.value.params);
    EXPECT_FALSE(replayed_request.accepted);
}

TEST(
    AttestationService,
    ProtectsTryCastRequest)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"}, 232);
    establish(service_b, identity{"enclave-a", "zone-a"}, 232);

    auto caller_zone = make_zone(33);
    auto destination_zone = make_zone(43);
    auto remote_object = destination_zone.with_object(rpc::object(23));
    ASSERT_TRUE(remote_object.has_value());

    rpc::try_cast_params params;
    params.protocol_version = rpc::get_version();
    params.caller_zone_id = caller_zone;
    params.remote_object_id = *remote_object;
    params.interface_id = rpc::interface_ordinal(0x1020304050ULL);
    params.in_back_channel.push_back(rpc::back_channel_entry{31, {2, 4, 6}});

    auto protected_request = protect_try_cast_request(*service_a, context_a, params, rpc::encoding::yas_binary);
    ASSERT_TRUE(protected_request.accepted) << protected_request.error.reason;
    EXPECT_EQ(
        protected_request.value.params.interface_id,
        canopy::security::attestation::encrypted_payload_interface_id(rpc::get_version()));
    expect_protected_payload(protected_request.value.params.payload, rpc::encoding::yas_binary);
    EXPECT_EQ(protected_request.value.params.remote_object_id.get_object_id().get_val(), 0U);
    EXPECT_NE(protected_request.value.params.interface_id, params.interface_id);

    auto unprotected_request = unprotect_try_cast_request(*service_b, protected_request.value.params);
    ASSERT_TRUE(unprotected_request.accepted) << unprotected_request.error.reason;
    EXPECT_EQ(unprotected_request.value.params.protocol_version, params.protocol_version);
    EXPECT_EQ(unprotected_request.value.params.caller_zone_id, params.caller_zone_id);
    EXPECT_EQ(unprotected_request.value.params.remote_object_id, params.remote_object_id);
    EXPECT_EQ(unprotected_request.value.params.interface_id, params.interface_id);
    EXPECT_FALSE(unprotected_request.value.params.payload.has_value());
    ASSERT_EQ(unprotected_request.value.params.in_back_channel.size(), 1U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].type_id, 31U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].payload, std::vector<uint8_t>({2, 4, 6}));

    auto replayed_request = unprotect_try_cast_request(*service_b, protected_request.value.params);
    EXPECT_FALSE(replayed_request.accepted);
}

TEST(
    AttestationService,
    ProtectsObjectReleasedRequest)
{
    auto service_owner = make_service("enclave-owner", "zone-owner");
    auto service_caller = make_service("enclave-caller", "zone-caller");
    const auto owner_context = establish(service_owner, identity{"enclave-caller", "zone-caller"}, 242);
    establish(service_caller, identity{"enclave-owner", "zone-owner"}, 242);

    auto owner_zone = make_zone(44);
    auto caller_zone = make_zone(34);
    auto remote_object = owner_zone.with_object(rpc::object(24));
    ASSERT_TRUE(remote_object.has_value());

    rpc::object_released_params params;
    params.protocol_version = rpc::get_version();
    params.remote_object_id = *remote_object;
    params.caller_zone_id = caller_zone;
    params.in_back_channel.push_back(rpc::back_channel_entry{41, {3, 5, 7}});
    const auto payload_type_id = 0xcafe01U;
    const auto payload_encoding = rpc::encoding::yas_binary;
    const auto payload_bytes = std::vector<char>{'o', 'b', 'j'};
    params.payload = make_raw_payload(payload_type_id, payload_encoding, payload_bytes);

    auto protected_request = protect_object_released_request(*service_owner, owner_context, params);
    ASSERT_TRUE(protected_request.accepted) << protected_request.error.reason;
    expect_protected_payload(protected_request.value.params.payload, payload_encoding);
    EXPECT_EQ(protected_request.value.params.remote_object_id.get_object_id().get_val(), 0U);
    EXPECT_NE(protected_request.value.params.payload->get_payload(), payload_bytes);

    auto unprotected_request = unprotect_object_released_request(*service_caller, protected_request.value.params);
    ASSERT_TRUE(unprotected_request.accepted) << unprotected_request.error.reason;
    EXPECT_EQ(unprotected_request.value.params.protocol_version, params.protocol_version);
    EXPECT_EQ(unprotected_request.value.params.remote_object_id, params.remote_object_id);
    EXPECT_EQ(unprotected_request.value.params.caller_zone_id, params.caller_zone_id);
    ASSERT_EQ(unprotected_request.value.params.in_back_channel.size(), 1U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].type_id, 41U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].payload, std::vector<uint8_t>({3, 5, 7}));
    expect_payload(unprotected_request.value.params.payload, payload_type_id, payload_encoding, payload_bytes);

    auto replayed_request = unprotect_object_released_request(*service_caller, protected_request.value.params);
    EXPECT_FALSE(replayed_request.accepted);
}

TEST(
    AttestationService,
    ProtectsTransportDownRequest)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"}, 252);
    establish(service_b, identity{"enclave-a", "zone-a"}, 252);

    auto caller_zone = make_zone(35);
    auto destination_zone = make_zone(45);

    rpc::transport_down_params params;
    params.protocol_version = rpc::get_version();
    params.destination_zone_id = destination_zone;
    params.caller_zone_id = caller_zone;
    params.in_back_channel.push_back(rpc::back_channel_entry{42, {4, 6, 8}});
    const auto payload_type_id = 0xcafe02U;
    const auto payload_encoding = rpc::encoding::yas_binary;
    const auto payload_bytes = std::vector<char>{'d', 'o', 'w', 'n'};
    params.payload = make_raw_payload(payload_type_id, payload_encoding, payload_bytes);

    auto protected_request = protect_transport_down_request(*service_a, context_a, params);
    ASSERT_TRUE(protected_request.accepted) << protected_request.error.reason;
    expect_protected_payload(protected_request.value.params.payload, payload_encoding);
    EXPECT_NE(protected_request.value.params.payload->get_payload(), payload_bytes);

    auto unprotected_request = unprotect_transport_down_request(*service_b, protected_request.value.params);
    ASSERT_TRUE(unprotected_request.accepted) << unprotected_request.error.reason;
    EXPECT_EQ(unprotected_request.value.params.protocol_version, params.protocol_version);
    EXPECT_EQ(unprotected_request.value.params.destination_zone_id, params.destination_zone_id);
    EXPECT_EQ(unprotected_request.value.params.caller_zone_id, params.caller_zone_id);
    ASSERT_EQ(unprotected_request.value.params.in_back_channel.size(), 1U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].type_id, 42U);
    EXPECT_EQ(unprotected_request.value.params.in_back_channel[0].payload, std::vector<uint8_t>({4, 6, 8}));
    expect_payload(unprotected_request.value.params.payload, payload_type_id, payload_encoding, payload_bytes);

    auto replayed_request = unprotect_transport_down_request(*service_b, protected_request.value.params);
    EXPECT_FALSE(replayed_request.accepted);
}

TEST(
    AttestationService,
    ProtectsControlRequestsUsingAgreedPayloadEncodings)
{
    auto payload_type_id = rpc::id<rpc::attestation_identity>::get(rpc::get_version());
    ASSERT_NE(payload_type_id, 0U);

    uint64_t transcript_id = 260;
    for (auto encoding : agreed_payload_encodings())
    {
        SCOPED_TRACE(static_cast<uint64_t>(encoding));

        auto service_a = make_service("enclave-a", "zone-a");
        auto service_b = make_service("enclave-b", "zone-b");
        const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"}, transcript_id);
        establish(service_b, identity{"enclave-a", "zone-a"}, transcript_id);
        ++transcript_id;

        auto caller_zone = make_zone(40 + transcript_id);
        auto destination_zone = make_zone(50 + transcript_id);
        auto requesting_zone = make_zone(60 + transcript_id);
        auto remote_object = destination_zone.with_object(rpc::object(0x1234));
        ASSERT_TRUE(remote_object.has_value());

        rpc::add_ref_params add_ref_params;
        add_ref_params.protocol_version = rpc::get_version();
        add_ref_params.remote_object_id = *remote_object;
        add_ref_params.caller_zone_id = caller_zone;
        add_ref_params.requesting_zone_id = requesting_zone;
        add_ref_params.build_out_param_channel = rpc::add_ref_options::build_caller_route;
        add_ref_params.request_id = 901;
        add_ref_params.payload = make_raw_payload(payload_type_id, encoding, make_identity_payload(encoding, "add-ref"));

        auto protected_add_ref = protect_add_ref_request(*service_a, context_a, add_ref_params);
        ASSERT_TRUE(protected_add_ref.accepted) << protected_add_ref.error.reason;
        expect_protected_payload(protected_add_ref.value.params.payload, encoding);
        EXPECT_EQ(protected_add_ref.value.params.remote_object_id.get_object_id().get_val(), 0U);
        EXPECT_NE(protected_add_ref.value.params.payload->get_payload(), add_ref_params.payload->get_payload());

        auto unprotected_add_ref = unprotect_add_ref_request(*service_b, protected_add_ref.value.params);
        ASSERT_TRUE(unprotected_add_ref.accepted) << unprotected_add_ref.error.reason;
        EXPECT_EQ(unprotected_add_ref.value.params.remote_object_id, add_ref_params.remote_object_id);
        expect_identity_typed_payload(unprotected_add_ref.value.params.payload, payload_type_id, encoding, "add-ref");

        rpc::release_params release_params;
        release_params.protocol_version = rpc::get_version();
        release_params.remote_object_id = *remote_object;
        release_params.caller_zone_id = caller_zone;
        release_params.options = rpc::release_options::normal;
        release_params.payload = make_raw_payload(payload_type_id, encoding, make_identity_payload(encoding, "release"));

        auto protected_release = protect_release_request(*service_a, context_a, release_params);
        ASSERT_TRUE(protected_release.accepted) << protected_release.error.reason;
        expect_protected_payload(protected_release.value.params.payload, encoding);
        EXPECT_EQ(protected_release.value.params.remote_object_id.get_object_id().get_val(), 0U);
        EXPECT_NE(protected_release.value.params.payload->get_payload(), release_params.payload->get_payload());

        auto unprotected_release = unprotect_release_request(*service_b, protected_release.value.params);
        ASSERT_TRUE(unprotected_release.accepted) << unprotected_release.error.reason;
        EXPECT_EQ(unprotected_release.value.params.remote_object_id, release_params.remote_object_id);
        expect_identity_typed_payload(unprotected_release.value.params.payload, payload_type_id, encoding, "release");

        rpc::try_cast_params try_cast_params;
        try_cast_params.protocol_version = rpc::get_version();
        try_cast_params.caller_zone_id = caller_zone;
        try_cast_params.remote_object_id = *remote_object;
        try_cast_params.interface_id = rpc::interface_ordinal(0xabc123);
        try_cast_params.payload = make_raw_payload(payload_type_id, encoding, make_identity_payload(encoding, "try-cast"));

        auto protected_try_cast = protect_try_cast_request(*service_a, context_a, try_cast_params);
        ASSERT_TRUE(protected_try_cast.accepted) << protected_try_cast.error.reason;
        expect_protected_payload(protected_try_cast.value.params.payload, encoding);
        EXPECT_EQ(
            protected_try_cast.value.params.interface_id,
            canopy::security::attestation::encrypted_payload_interface_id(rpc::get_version()));
        EXPECT_EQ(protected_try_cast.value.params.remote_object_id.get_object_id().get_val(), 0U);
        EXPECT_NE(protected_try_cast.value.params.payload->get_payload(), try_cast_params.payload->get_payload());

        auto unprotected_try_cast = unprotect_try_cast_request(*service_b, protected_try_cast.value.params);
        ASSERT_TRUE(unprotected_try_cast.accepted) << unprotected_try_cast.error.reason;
        EXPECT_EQ(unprotected_try_cast.value.params.remote_object_id, try_cast_params.remote_object_id);
        EXPECT_EQ(unprotected_try_cast.value.params.interface_id, try_cast_params.interface_id);
        expect_identity_typed_payload(unprotected_try_cast.value.params.payload, payload_type_id, encoding, "try-cast");

        rpc::object_released_params object_released_params;
        object_released_params.protocol_version = rpc::get_version();
        object_released_params.remote_object_id = *remote_object;
        object_released_params.caller_zone_id = caller_zone;
        object_released_params.payload
            = make_raw_payload(payload_type_id, encoding, make_identity_payload(encoding, "object-released"));

        auto protected_object_released = protect_object_released_request(*service_a, context_a, object_released_params);
        ASSERT_TRUE(protected_object_released.accepted) << protected_object_released.error.reason;
        expect_protected_payload(protected_object_released.value.params.payload, encoding);
        EXPECT_EQ(protected_object_released.value.params.remote_object_id.get_object_id().get_val(), 0U);
        EXPECT_NE(
            protected_object_released.value.params.payload->get_payload(), object_released_params.payload->get_payload());

        auto unprotected_object_released
            = unprotect_object_released_request(*service_b, protected_object_released.value.params);
        ASSERT_TRUE(unprotected_object_released.accepted) << unprotected_object_released.error.reason;
        EXPECT_EQ(unprotected_object_released.value.params.remote_object_id, object_released_params.remote_object_id);
        expect_identity_typed_payload(
            unprotected_object_released.value.params.payload, payload_type_id, encoding, "object-released");

        rpc::transport_down_params transport_down_params;
        transport_down_params.protocol_version = rpc::get_version();
        transport_down_params.destination_zone_id = destination_zone;
        transport_down_params.caller_zone_id = caller_zone;
        transport_down_params.payload
            = make_raw_payload(payload_type_id, encoding, make_identity_payload(encoding, "transport-down"));

        auto protected_transport_down = protect_transport_down_request(*service_a, context_a, transport_down_params);
        ASSERT_TRUE(protected_transport_down.accepted) << protected_transport_down.error.reason;
        expect_protected_payload(protected_transport_down.value.params.payload, encoding);
        EXPECT_NE(
            protected_transport_down.value.params.payload->get_payload(), transport_down_params.payload->get_payload());

        auto unprotected_transport_down
            = unprotect_transport_down_request(*service_b, protected_transport_down.value.params);
        ASSERT_TRUE(unprotected_transport_down.accepted) << unprotected_transport_down.error.reason;
        EXPECT_EQ(unprotected_transport_down.value.params.destination_zone_id, transport_down_params.destination_zone_id);
        expect_identity_typed_payload(
            unprotected_transport_down.value.params.payload, payload_type_id, encoding, "transport-down");
    }
}

TEST(
    AttestationService,
    ProtectedRequestsAllowMutablePublicBackChannels)
{
    auto service_a = make_service("enclave-a", "zone-a");
    auto service_b = make_service("enclave-b", "zone-b");
    const auto context_a = establish(service_a, identity{"enclave-b", "zone-b"}, 303);
    const auto context_b = establish(service_b, identity{"enclave-a", "zone-a"}, 303);

    auto caller_zone = make_zone(61);
    auto destination_zone = make_zone(71);
    auto remote_object = destination_zone.with_object(rpc::object(13));
    ASSERT_TRUE(remote_object.has_value());

    rpc::send_params send_params;
    send_params.protocol_version = rpc::get_version();
    send_params.encoding_type = rpc::encoding::yas_binary;
    send_params.tag = 4321;
    send_params.caller_zone_id = caller_zone;
    send_params.remote_object_id = *remote_object;
    send_params.interface_id = rpc::interface_ordinal(0x778899);
    send_params.method_id = rpc::method(6);
    send_params.in_data = {'s'};
    send_params.in_back_channel.push_back(rpc::back_channel_entry{1, {2}});
    send_params.request_id = 14;

    auto protected_send = protect_send_request(*service_a, context_a, send_params);
    ASSERT_TRUE(protected_send.accepted) << protected_send.error.reason;

    auto receiver_send = protected_send.value.params;
    receiver_send.in_back_channel.push_back(rpc::back_channel_entry{99, {8, 7, 6}});

    auto unprotected_send = unprotect_send_request(*service_b, receiver_send);
    ASSERT_TRUE(unprotected_send.accepted) << unprotected_send.error.reason;
    ASSERT_EQ(unprotected_send.value.params.in_back_channel.size(), 2U);
    EXPECT_EQ(unprotected_send.value.params.in_back_channel[0].type_id, 1U);
    EXPECT_EQ(unprotected_send.value.params.in_back_channel[1].type_id, 99U);
    EXPECT_EQ(unprotected_send.value.params.in_back_channel[1].payload, std::vector<uint8_t>({8, 7, 6}));

    rpc::send_result response;
    response.error_code = rpc::error::OK();
    response.out_buf = {'r'};
    response.out_back_channel.push_back(rpc::back_channel_entry{5, {6}});
    auto protected_response = protect_send_response(
        *service_b, context_b, receiver_send, unprotected_send.value.request_counter, std::move(response));
    ASSERT_TRUE(protected_response.accepted) << protected_response.error.reason;
    protected_response.value.out_back_channel.push_back(rpc::back_channel_entry{101, {10, 11}});

    auto unprotected_response = unprotect_send_response(
        *service_a,
        context_a,
        protected_send.value.params,
        protected_send.value.request_counter,
        std::move(protected_response.value));
    ASSERT_TRUE(unprotected_response.accepted) << unprotected_response.error.reason;
    EXPECT_EQ(unprotected_response.value.out_buf, std::vector<char>({'r'}));
    ASSERT_EQ(unprotected_response.value.out_back_channel.size(), 2U);
    EXPECT_EQ(unprotected_response.value.out_back_channel[0].type_id, 5U);
    EXPECT_EQ(unprotected_response.value.out_back_channel[1].type_id, 101U);
    EXPECT_EQ(unprotected_response.value.out_back_channel[1].payload, std::vector<uint8_t>({10, 11}));

    auto requesting_zone = make_zone(81);
    rpc::add_ref_params add_ref_params;
    add_ref_params.protocol_version = rpc::get_version();
    add_ref_params.remote_object_id = *remote_object;
    add_ref_params.caller_zone_id = caller_zone;
    add_ref_params.requesting_zone_id = requesting_zone;
    add_ref_params.build_out_param_channel = rpc::add_ref_options::normal;
    add_ref_params.in_back_channel.push_back(rpc::back_channel_entry{3, {4}});
    add_ref_params.request_id = 15;

    auto protected_add_ref = protect_add_ref_request(*service_a, context_a, add_ref_params, rpc::encoding::yas_binary);
    ASSERT_TRUE(protected_add_ref.accepted) << protected_add_ref.error.reason;

    auto receiver_add_ref = protected_add_ref.value.params;
    receiver_add_ref.in_back_channel.push_back(rpc::back_channel_entry{100, {9, 8, 7}});

    auto unprotected_add_ref = unprotect_add_ref_request(*service_b, receiver_add_ref);
    ASSERT_TRUE(unprotected_add_ref.accepted) << unprotected_add_ref.error.reason;
    ASSERT_EQ(unprotected_add_ref.value.params.in_back_channel.size(), 2U);
    EXPECT_EQ(unprotected_add_ref.value.params.in_back_channel[0].type_id, 3U);
    EXPECT_EQ(unprotected_add_ref.value.params.in_back_channel[1].type_id, 100U);
    EXPECT_EQ(unprotected_add_ref.value.params.in_back_channel[1].payload, std::vector<uint8_t>({9, 8, 7}));

    rpc::release_params release_params;
    release_params.protocol_version = rpc::get_version();
    release_params.remote_object_id = *remote_object;
    release_params.caller_zone_id = caller_zone;
    release_params.options = rpc::release_options::normal;
    release_params.in_back_channel.push_back(rpc::back_channel_entry{4, {5}});

    auto protected_release = protect_release_request(*service_a, context_a, release_params, rpc::encoding::yas_binary);
    ASSERT_TRUE(protected_release.accepted) << protected_release.error.reason;

    auto receiver_release = protected_release.value.params;
    receiver_release.in_back_channel.push_back(rpc::back_channel_entry{102, {11, 10, 9}});

    auto unprotected_release = unprotect_release_request(*service_b, receiver_release);
    ASSERT_TRUE(unprotected_release.accepted) << unprotected_release.error.reason;
    ASSERT_EQ(unprotected_release.value.params.in_back_channel.size(), 2U);
    EXPECT_EQ(unprotected_release.value.params.in_back_channel[0].type_id, 4U);
    EXPECT_EQ(unprotected_release.value.params.in_back_channel[1].type_id, 102U);
    EXPECT_EQ(unprotected_release.value.params.in_back_channel[1].payload, std::vector<uint8_t>({11, 10, 9}));

    rpc::try_cast_params try_cast_params;
    try_cast_params.protocol_version = rpc::get_version();
    try_cast_params.caller_zone_id = caller_zone;
    try_cast_params.remote_object_id = *remote_object;
    try_cast_params.interface_id = rpc::interface_ordinal(0x998877);
    try_cast_params.in_back_channel.push_back(rpc::back_channel_entry{6, {7}});

    auto protected_try_cast = protect_try_cast_request(*service_a, context_a, try_cast_params, rpc::encoding::yas_binary);
    ASSERT_TRUE(protected_try_cast.accepted) << protected_try_cast.error.reason;

    auto receiver_try_cast = protected_try_cast.value.params;
    receiver_try_cast.in_back_channel.push_back(rpc::back_channel_entry{103, {12, 13, 14}});

    auto unprotected_try_cast = unprotect_try_cast_request(*service_b, receiver_try_cast);
    ASSERT_TRUE(unprotected_try_cast.accepted) << unprotected_try_cast.error.reason;
    ASSERT_EQ(unprotected_try_cast.value.params.in_back_channel.size(), 2U);
    EXPECT_EQ(unprotected_try_cast.value.params.in_back_channel[0].type_id, 6U);
    EXPECT_EQ(unprotected_try_cast.value.params.in_back_channel[1].type_id, 103U);
    EXPECT_EQ(unprotected_try_cast.value.params.in_back_channel[1].payload, std::vector<uint8_t>({12, 13, 14}));

    rpc::object_released_params object_released_params;
    object_released_params.protocol_version = rpc::get_version();
    object_released_params.remote_object_id = *remote_object;
    object_released_params.caller_zone_id = caller_zone;
    object_released_params.in_back_channel.push_back(rpc::back_channel_entry{7, {8}});

    auto protected_object_released
        = protect_object_released_request(*service_b, context_b, object_released_params, rpc::encoding::yas_binary);
    ASSERT_TRUE(protected_object_released.accepted) << protected_object_released.error.reason;

    auto receiver_object_released = protected_object_released.value.params;
    receiver_object_released.in_back_channel.push_back(rpc::back_channel_entry{104, {15, 16, 17}});

    auto unprotected_object_released = unprotect_object_released_request(*service_a, receiver_object_released);
    ASSERT_TRUE(unprotected_object_released.accepted) << unprotected_object_released.error.reason;
    ASSERT_EQ(unprotected_object_released.value.params.in_back_channel.size(), 2U);
    EXPECT_EQ(unprotected_object_released.value.params.in_back_channel[0].type_id, 7U);
    EXPECT_EQ(unprotected_object_released.value.params.in_back_channel[1].type_id, 104U);
    EXPECT_EQ(unprotected_object_released.value.params.in_back_channel[1].payload, std::vector<uint8_t>({15, 16, 17}));

    rpc::transport_down_params transport_down_params;
    transport_down_params.protocol_version = rpc::get_version();
    transport_down_params.destination_zone_id = destination_zone;
    transport_down_params.caller_zone_id = caller_zone;
    transport_down_params.in_back_channel.push_back(rpc::back_channel_entry{8, {9}});

    auto protected_transport_down
        = protect_transport_down_request(*service_a, context_a, transport_down_params, rpc::encoding::yas_binary);
    ASSERT_TRUE(protected_transport_down.accepted) << protected_transport_down.error.reason;

    auto receiver_transport_down = protected_transport_down.value.params;
    receiver_transport_down.in_back_channel.push_back(rpc::back_channel_entry{105, {18, 19, 20}});

    auto unprotected_transport_down = unprotect_transport_down_request(*service_b, receiver_transport_down);
    ASSERT_TRUE(unprotected_transport_down.accepted) << unprotected_transport_down.error.reason;
    ASSERT_EQ(unprotected_transport_down.value.params.in_back_channel.size(), 2U);
    EXPECT_EQ(unprotected_transport_down.value.params.in_back_channel[0].type_id, 8U);
    EXPECT_EQ(unprotected_transport_down.value.params.in_back_channel[1].type_id, 105U);
    EXPECT_EQ(unprotected_transport_down.value.params.in_back_channel[1].payload, std::vector<uint8_t>({18, 19, 20}));
}
