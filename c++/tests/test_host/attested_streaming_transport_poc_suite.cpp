/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <common/tests.h>

#include "gtest/gtest.h"
#include "test_globals.h"
#include "test_host.h"
#include "type_test_fixture.h"

#ifdef CANOPY_BUILD_COROUTINE
#  include <attestation/route_attestation_protocol.h>
#  include <security/attestation/backends/fake/fake_backend.h>
#  include <security/attestation/protected_rpc.h>
#  include <streaming/attestation/stream.h>
#  include <streaming/spsc_queue/stream.h>
#  include <transports/local/transport.h>
#  include <transport/tests/streaming_setup_base.h>
#  include <optional>
#  include <unordered_map>
#endif

#ifdef CANOPY_BUILD_COROUTINE
namespace
{
    using canopy::security::attestation::attestation_backend;
    using canopy::security::attestation::attestation_policy;
    using canopy::security::attestation::attestation_service;
    using canopy::security::attestation::attestation_service_options;
    using canopy::security::attestation::attestation_verdict;
    using canopy::security::attestation::cmw;
    using canopy::security::attestation::evidence_binding;
    using canopy::security::attestation::fake_backend;
    using canopy::security::attestation::fake_backend_id;
    using canopy::security::attestation::identity;
    using canopy::security::attestation::route_attestation_status;
    using canopy::security::attestation::security_context;
    using streaming::attestation::stream_options;

    constexpr uint64_t attestation_stream_transcript_id = 1001;
    constexpr std::chrono::milliseconds attestation_stream_handshake_timeout{2000};
    constexpr uint64_t service_level_route_initiator_subnet = 4096;
    constexpr uint64_t service_level_route_responder_subnet = 4097;
    constexpr std::chrono::milliseconds service_level_route_call_timeout{30000};
    constexpr std::chrono::milliseconds service_level_route_call_timeout_sweep{1};
    constexpr size_t service_level_route_cleanup_drain_iterations = 16;
    constexpr std::chrono::seconds service_level_route_test_timeout{10};
    constexpr int arithmetic_test_left_value = 20;
    constexpr int arithmetic_test_right_value = 22;
    constexpr int arithmetic_test_expected_result = 42;
    constexpr size_t blob_round_trip_test_size = 4096;
    constexpr uint8_t blob_round_trip_byte_mask = 0xffU;
    constexpr uint64_t protected_rpc_generated_runtime_transcript_id = 7001;
    constexpr size_t protected_rpc_generated_runtime_post_count = 4;
    constexpr size_t protected_rpc_generated_runtime_min_send_count = 4;
    constexpr size_t protected_rpc_generated_runtime_disconnect_drain_iterations = 128;
    constexpr uint64_t domain_local_subject_zone_subnet = 8192;
    constexpr uint64_t domain_local_adjacent_zone_subnet = 8193;
    constexpr uint64_t domain_local_referenced_zone_subnet = 8194;
    constexpr uint64_t add_ref_route_subject_local_subnet = 8300;
    constexpr uint64_t add_ref_route_subject_adjacent_subnet = 8301;
    constexpr uint64_t add_ref_route_subject_destination_subnet = 8302;
    constexpr uint64_t add_ref_route_subject_caller_subnet = 8303;
    constexpr uint64_t concurrent_route_first_object_id = 201;
    constexpr uint64_t concurrent_route_second_object_id = 202;
    constexpr int domain_local_post_message_value = 77;
    constexpr uint8_t verifier_challenge_test_magic = 0xa7U;
    constexpr const char* verifier_challenge_test_media_type = "application/canopy-test-verifier-challenge";
    constexpr const char* verifier_challenge_test_content_format = "canopy.test.verifier-challenge.v1";

    [[nodiscard]] auto make_verifier_challenge_test_payload(const evidence_binding& binding) -> std::vector<uint8_t>
    {
        std::vector<uint8_t> payload;
        payload.reserve(1 + sizeof(binding.transcript_id) + binding.nonce.size());
        payload.push_back(verifier_challenge_test_magic);
        for (auto shift = 56; shift >= 0; shift -= 8)
            payload.push_back(static_cast<uint8_t>((binding.transcript_id >> shift) & 0xffU));
        payload.insert(payload.end(), binding.nonce.begin(), binding.nonce.end());
        return payload;
    }

    [[nodiscard]] auto verifier_challenge_test_matches(
        const cmw& challenge,
        const evidence_binding& binding) -> bool
    {
        return challenge.media_type == verifier_challenge_test_media_type
               && challenge.content_format == verifier_challenge_test_content_format
               && challenge.payload == make_verifier_challenge_test_payload(binding);
    }

    struct verifier_challenge_test_counts
    {
        size_t challenges_made{0};
        size_t challenge_bound_evidence_made{0};
        size_t challenge_bound_evidence_verified{0};
    };

    class fake_backend_with_verifier_challenge final : public fake_backend
    {
    public:
        explicit fake_backend_with_verifier_challenge(std::shared_ptr<verifier_challenge_test_counts> counts)
            : counts_(std::move(counts))
        {
        }

        [[nodiscard]] auto supports_verifier_challenge() const -> bool override { return true; }

        [[nodiscard]] auto make_verifier_challenge(const evidence_binding& binding) const -> std::optional<cmw> override
        {
            if (!counts_ || binding.transcript_id == 0 || binding.nonce.empty())
                return std::nullopt;

            ++counts_->challenges_made;
            cmw challenge;
            challenge.media_type = verifier_challenge_test_media_type;
            challenge.content_format = verifier_challenge_test_content_format;
            challenge.payload = make_verifier_challenge_test_payload(binding);
            return challenge;
        }

        [[nodiscard]] auto produce_evidence_for_challenge(
            const cmw& verifier_challenge,
            const evidence_binding& binding) const -> std::optional<cmw> override
        {
            if (!counts_ || !verifier_challenge_test_matches(verifier_challenge, binding))
                return std::nullopt;

            ++counts_->challenge_bound_evidence_made;
            return fake_backend::produce_evidence(binding);
        }

        [[nodiscard]] auto verify_evidence_for_challenge(
            const cmw& evidence,
            const cmw& verifier_challenge,
            const evidence_binding& expected_binding,
            const attestation_policy& policy) const -> attestation_verdict override
        {
            if (!counts_ || !verifier_challenge_test_matches(verifier_challenge, expected_binding))
            {
                attestation_verdict rejected;
                rejected.reason = "verifier challenge did not match expected binding";
                return rejected;
            }

            ++counts_->challenge_bound_evidence_verified;
            return fake_backend::verify_evidence(evidence, expected_binding, policy);
        }

    private:
        std::shared_ptr<verifier_challenge_test_counts> counts_;
    };

    auto make_test_attestation_service(
        const std::shared_ptr<attestation_backend>& backend,
        std::string security_domain_id,
        std::string zone_id,
        bool send_evidence,
        bool require_peer_evidence,
        bool allow_unattested_peer = false) -> std::shared_ptr<attestation_service>
    {
        attestation_service_options options;
        options.local_identity = identity{std::move(security_domain_id), std::move(zone_id)};
        options.backend = backend;
        options.policy = attestation_policy{};
        options.policy.send_local_evidence = send_evidence;
        options.policy.require_peer_evidence = require_peer_evidence;
        options.policy.allow_unattested_peer = allow_unattested_peer;
        options.policy.allow_development_evidence = true;
        options.policy.required_backend_id = fake_backend_id;
        return std::make_shared<attestation_service>(std::move(options));
    }

    class attested_streaming_spsc_setup_base : public streaming_setup_base<false, false, false>
    {
        std::shared_ptr<streaming::spsc_queue::queue_type> send_spsc_queue_;
        std::shared_ptr<streaming::spsc_queue::queue_type> receive_spsc_queue_;
        std::shared_ptr<streaming::attestation::stream> initiator_stream_;
        std::shared_ptr<streaming::attestation::stream> responder_stream_;
        std::shared_ptr<attestation_service> initiator_attestation_service_;
        std::shared_ptr<attestation_service> responder_attestation_service_;
        bool initiator_sends_evidence_{true};
        bool initiator_requires_peer_evidence_{true};
        bool initiator_allows_unattested_peer_{false};
        bool responder_sends_evidence_{true};
        bool responder_requires_peer_evidence_{true};
        bool responder_allows_unattested_peer_{false};

        static auto make_attestation_service(
            std::shared_ptr<fake_backend> backend,
            std::string security_domain_id,
            std::string zone_id,
            bool send_evidence,
            bool require_peer_evidence,
            bool allow_unattested_peer) -> std::shared_ptr<attestation_service>
        {
            return make_test_attestation_service(
                backend,
                std::move(security_domain_id),
                std::move(zone_id),
                send_evidence,
                require_peer_evidence,
                allow_unattested_peer);
        }

        static auto make_options(std::shared_ptr<attestation_service> service) -> stream_options
        {
            stream_options options;
            options.service = std::move(service);
            options.transcript_id = attestation_stream_transcript_id;
            options.handshake_timeout = attestation_stream_handshake_timeout;
            return options;
        }

    protected:
        attested_streaming_spsc_setup_base(
            bool initiator_sends_evidence,
            bool initiator_requires_peer_evidence,
            bool responder_sends_evidence,
            bool responder_requires_peer_evidence,
            bool initiator_allows_unattested_peer = false,
            bool responder_allows_unattested_peer = false)
            : initiator_sends_evidence_(initiator_sends_evidence)
            , initiator_requires_peer_evidence_(initiator_requires_peer_evidence)
            , initiator_allows_unattested_peer_(initiator_allows_unattested_peer)
            , responder_sends_evidence_(responder_sends_evidence)
            , responder_requires_peer_evidence_(responder_requires_peer_evidence)
            , responder_allows_unattested_peer_(responder_allows_unattested_peer)
        {
        }

        CORO_TASK(bool) do_coro_setup() override
        {
            auto root_zone_id = rpc::DEFAULT_PREFIX;
            auto peer_zone_id = make_peer_zone_id();
            root_service_ = rpc::root_service::create("host", root_zone_id, io_scheduler_);
            peer_service_ = rpc::root_service::create("peer", peer_zone_id, io_scheduler_);
            current_host_service = root_service_;

            auto backend = std::make_shared<fake_backend>();
            send_spsc_queue_ = std::make_shared<streaming::spsc_queue::queue_type>();
            receive_spsc_queue_ = std::make_shared<streaming::spsc_queue::queue_type>();

            auto initiator_raw
                = std::make_shared<streaming::spsc_queue::stream>(send_spsc_queue_, receive_spsc_queue_, io_scheduler_);
            auto responder_raw
                = std::make_shared<streaming::spsc_queue::stream>(receive_spsc_queue_, send_spsc_queue_, io_scheduler_);

            initiator_attestation_service_ = make_attestation_service(
                backend,
                "initiator-domain",
                "initiator-zone",
                initiator_sends_evidence_,
                initiator_requires_peer_evidence_,
                initiator_allows_unattested_peer_);
            responder_attestation_service_ = make_attestation_service(
                backend,
                "responder-domain",
                "responder-zone",
                responder_sends_evidence_,
                responder_requires_peer_evidence_,
                responder_allows_unattested_peer_);

            initiator_stream_ = std::make_shared<streaming::attestation::stream>(
                std::move(initiator_raw), make_options(initiator_attestation_service_));
            responder_stream_ = std::make_shared<streaming::attestation::stream>(
                std::move(responder_raw), make_options(responder_attestation_service_));

            bool initiator_handshake_complete = false;
            bool responder_handshake_complete = false;
            CO_AWAIT coro::when_all(
                [&]() -> coro::task<void>
                {
                    initiator_handshake_complete = CO_AWAIT initiator_stream_->client_handshake();
                    CO_RETURN;
                }(),
                [&]() -> coro::task<void>
                {
                    responder_handshake_complete = CO_AWAIT responder_stream_->server_handshake();
                    CO_RETURN;
                }());

            if (!initiator_handshake_complete || !responder_handshake_complete)
                CO_RETURN false;

            responder_transport_ = std::static_pointer_cast<rpc::stream_transport::transport>(
                CO_AWAIT peer_service_->make_acceptor<yyy::i_host, yyy::i_example>(
                    "responder_transport",
                    rpc::stream_transport::transport_factory(responder_stream_, test_transport_options_),
                    make_interface_setup_factory()));
            CO_AWAIT responder_transport_->accept();

            rpc::shared_ptr<yyy::i_host> local_host(new host());
            local_host_ptr_ = local_host;

            initiator_transport_ = rpc::stream_transport::make_client(
                "initiator_transport", root_service_, initiator_stream_, test_transport_options_);

            auto connect_result = CO_AWAIT root_service_->connect_to_zone<yyy::i_host, yyy::i_example>(
                "attested child", initiator_transport_, local_host);
            i_example_ptr_ = std::move(connect_result.output_interface);

            if (connect_result.error_code != rpc::error::OK())
                CO_RETURN false;
            CO_RETURN true;
        }

    public:
        ~attested_streaming_spsc_setup_base() override = default;

        [[nodiscard]] auto initiator_security_context() const -> security_context
        {
            return initiator_stream_ ? initiator_stream_->security_context() : security_context{};
        }

        [[nodiscard]] auto responder_security_context() const -> security_context
        {
            return responder_stream_ ? responder_stream_->security_context() : security_context{};
        }

        [[nodiscard]] auto initiator_sends_evidence() const -> bool { return initiator_sends_evidence_; }
        [[nodiscard]] auto initiator_requires_peer_evidence() const -> bool
        {
            return initiator_requires_peer_evidence_;
        }
        [[nodiscard]] auto responder_sends_evidence() const -> bool { return responder_sends_evidence_; }
        [[nodiscard]] auto responder_requires_peer_evidence() const -> bool
        {
            return responder_requires_peer_evidence_;
        }
        [[nodiscard]] auto initiator_session_count() const -> size_t
        {
            return initiator_attestation_service_ ? initiator_attestation_service_->session_count() : 0;
        }
        [[nodiscard]] auto responder_session_count() const -> size_t
        {
            return responder_attestation_service_ ? responder_attestation_service_->session_count() : 0;
        }
    };

    class mutually_attested_streaming_spsc_setup final : public attested_streaming_spsc_setup_base
    {
    public:
        mutually_attested_streaming_spsc_setup()
            : attested_streaming_spsc_setup_base(
                  true,
                  true,
                  true,
                  true)
        {
        }
    };

    class unattested_client_to_attested_server_streaming_spsc_setup final : public attested_streaming_spsc_setup_base
    {
    public:
        unattested_client_to_attested_server_streaming_spsc_setup()
            : attested_streaming_spsc_setup_base(
                  false,
                  true,
                  true,
                  false,
                  false,
                  true)
        {
        }
    };

} // namespace

template<class T> using attested_streaming_transport_poc_test = type_test<T>;

using attested_streaming_transport_poc_implementations
    = ::testing::Types<mutually_attested_streaming_spsc_setup, unattested_client_to_attested_server_streaming_spsc_setup>;

TYPED_TEST_SUITE(
    attested_streaming_transport_poc_test,
    attested_streaming_transport_poc_implementations);

template<class T> CORO_TASK(bool) coro_rpc_round_trip_over_attested_stream(T& lib)
{
    const auto initiator_context = lib.initiator_security_context();
    const auto responder_context = lib.responder_security_context();

    CORO_ASSERT_EQ(initiator_context.established, true);
    CORO_ASSERT_EQ(responder_context.established, true);
    CORO_ASSERT_EQ(initiator_context.local_evidence_sent, lib.initiator_sends_evidence());
    CORO_ASSERT_EQ(responder_context.local_evidence_sent, lib.responder_sends_evidence());
    CORO_ASSERT_EQ(initiator_context.peer_attested, lib.responder_sends_evidence());
    CORO_ASSERT_EQ(responder_context.peer_attested, lib.initiator_sends_evidence());
    CORO_ASSERT_EQ(initiator_context.local_identity.security_domain_id, std::string{"initiator-domain"});
    CORO_ASSERT_EQ(initiator_context.peer_identity.security_domain_id, std::string{"responder-domain"});
    CORO_ASSERT_EQ(responder_context.local_identity.security_domain_id, std::string{"responder-domain"});
    CORO_ASSERT_EQ(responder_context.peer_identity.security_domain_id, std::string{"initiator-domain"});
    CORO_ASSERT_EQ(initiator_context.backend_id, std::string{fake_backend_id});
    CORO_ASSERT_EQ(responder_context.backend_id, std::string{fake_backend_id});
    CORO_ASSERT_EQ(lib.initiator_session_count(), 1U);
    CORO_ASSERT_EQ(lib.responder_session_count(), 1U);

    int result = 0;
    CORO_ASSERT_EQ(
        CO_AWAIT lib.get_example()->add(arithmetic_test_left_value, arithmetic_test_right_value, result), rpc::error::OK());
    CORO_ASSERT_EQ(result, arithmetic_test_expected_result);

    std::vector<uint8_t> input(blob_round_trip_test_size);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<uint8_t>(i & blob_round_trip_byte_mask);

    rpc::shared_ptr<xxx::i_baz> baz_ptr;
    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->create_baz(baz_ptr), rpc::error::OK());
    CORO_ASSERT_NE(baz_ptr, nullptr);

    std::vector<uint8_t> output;
    CORO_ASSERT_EQ(CO_AWAIT baz_ptr->blob_test(input, output), rpc::error::OK());
    CORO_ASSERT_EQ(output, input);

    baz_ptr = nullptr;
    CO_RETURN true;
}

TYPED_TEST(
    attested_streaming_transport_poc_test,
    rpc_round_trip_over_attested_stream)
{
    run_coro_test(*this, [](auto& lib) { return coro_rpc_round_trip_over_attested_stream<TypeParam>(lib); });
}

#endif
