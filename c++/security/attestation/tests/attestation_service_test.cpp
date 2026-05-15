/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <gtest/gtest.h>

#include <security/attestation/fake_backend.h>
#include <security/attestation/protected_rpc.h>
#include <security/attestation/service.h>

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using canopy::security::attestation::attestation_policy;
    using canopy::security::attestation::attestation_service;
    using canopy::security::attestation::attestation_service_options;
    using canopy::security::attestation::cmw;
    using canopy::security::attestation::establish_session_params;
    using canopy::security::attestation::evaluate_route_attestation_state;
    using canopy::security::attestation::evidence_binding;
    using canopy::security::attestation::fake_backend;
    using canopy::security::attestation::fake_backend_id;
    using canopy::security::attestation::fake_evidence_media_type;
    using canopy::security::attestation::identity;
    using canopy::security::attestation::make_session_id;
    using canopy::security::attestation::protect_send_request;
    using canopy::security::attestation::protect_send_response;
    using canopy::security::attestation::protected_key_scope;
    using canopy::security::attestation::protected_rpc_direction;
    using canopy::security::attestation::route_attestation_action;
    using canopy::security::attestation::route_attestation_state;
    using canopy::security::attestation::route_attestation_status;
    using canopy::security::attestation::security_context;
    using canopy::security::attestation::security_level;
    using canopy::security::attestation::unprotect_send_request;
    using canopy::security::attestation::unprotect_send_response;

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

    cmw truncated;
    truncated.media_type = fake_evidence_media_type;
    truncated.content_format = "canopy.fake.v1";
    truncated.payload = {4, 0, 0, 0, 'f'};
    EXPECT_FALSE(backend.verify_evidence(truncated, expected_binding, policy).accepted);

    cmw oversized;
    oversized.media_type = fake_evidence_media_type;
    oversized.content_format = "canopy.fake.v1";
    oversized.payload = {0xff, 0xff, 0xff, 0xff};
    EXPECT_FALSE(backend.verify_evidence(oversized, expected_binding, policy).accepted);
}

TEST(
    AttestationService,
    SessionIdsAreScopedToEnclavePairs)
{
    auto first = make_session_id(identity{"enclave-a", "zone-a"}, identity{"enclave-b", "zone-b"}, 99);
    auto second = make_session_id(identity{"enclave-a", "zone-a-2"}, identity{"enclave-b", "zone-b-2"}, 99);
    auto third = make_session_id(identity{"enclave-a", "zone-a"}, identity{"enclave-c", "zone-c"}, 99);

    EXPECT_EQ(first, second);
    EXPECT_NE(first, third);
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
    KdfGoldenVectorIsStable)
{
    auto service = make_service("enclave-a", "zone-a");
    const auto context = establish(service, identity{"enclave-b", "zone-b"});
    auto scope = make_scope(context, identity{"enclave-a", "zone-a"}, identity{"enclave-b", "zone-b"});

    auto material = service->derive_aead_key(scope);
    ASSERT_TRUE(material.has_value());

    const std::array<uint8_t, canopy::security::attestation::aead_key_size> expected_key{0xb2,
        0x37,
        0x48,
        0x74,
        0x02,
        0x03,
        0xdb,
        0x3b,
        0x04,
        0x03,
        0x5c,
        0xf3,
        0x49,
        0xd8,
        0x4e,
        0x64,
        0xfe,
        0x43,
        0xd9,
        0x1a,
        0x41,
        0xc2,
        0x72,
        0xcb,
        0x19,
        0xf6,
        0x01,
        0xe7,
        0x51,
        0x6a,
        0x3f,
        0x81};
    const std::array<uint8_t, canopy::security::attestation::aead_nonce_prefix_size> expected_nonce_prefix{
        0x47, 0x52, 0x4b, 0xd5};
    const std::array<uint8_t, canopy::security::attestation::aead_nonce_size> expected_nonce{
        0x47, 0x52, 0x4b, 0xd5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07};

    EXPECT_EQ(material->key, expected_key);
    EXPECT_EQ(material->nonce_prefix, expected_nonce_prefix);
    EXPECT_EQ(service->make_aead_nonce(*material, 7), expected_nonce);
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
    EXPECT_EQ(protected_request.value.params.encoding_type, rpc::encoding::yas_binary);
    EXPECT_NE(protected_request.value.params.in_data, params.in_data);

    auto unprotected_request = unprotect_send_request(*service_b, protected_request.value.params);
    ASSERT_TRUE(unprotected_request.accepted) << unprotected_request.error.reason;
    EXPECT_EQ(unprotected_request.value.params.protocol_version, params.protocol_version);
    EXPECT_EQ(unprotected_request.value.params.encoding_type, params.encoding_type);
    EXPECT_EQ(unprotected_request.value.params.tag, params.tag);
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
    EXPECT_EQ(unprotected_request.error.error_code, rpc::error::SECURITY_ERROR());
}
