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
#  include <security/attestation/fake_backend.h>
#  include <streaming/attestation/stream.h>
#  include <streaming/spsc_queue/stream.h>
#  include <transport/tests/streaming_setup_base.h>
#endif

#ifdef CANOPY_BUILD_COROUTINE
namespace
{
    using canopy::security::attestation::attestation_policy;
    using canopy::security::attestation::fake_backend;
    using canopy::security::attestation::fake_backend_id;
    using canopy::security::attestation::identity;
    using canopy::security::attestation::security_context;
    using streaming::attestation::stream_options;

    class attested_streaming_spsc_setup_base : public streaming_setup_base<false, false, false>
    {
        std::shared_ptr<streaming::spsc_queue::queue_type> send_spsc_queue_;
        std::shared_ptr<streaming::spsc_queue::queue_type> receive_spsc_queue_;
        std::shared_ptr<streaming::attestation::stream> initiator_stream_;
        std::shared_ptr<streaming::attestation::stream> responder_stream_;
        bool initiator_sends_evidence_{true};
        bool initiator_requires_peer_evidence_{true};
        bool responder_sends_evidence_{true};
        bool responder_requires_peer_evidence_{true};

        static auto make_options(
            std::shared_ptr<fake_backend> backend,
            std::string enclave_id,
            std::string zone_id,
            bool send_evidence,
            bool require_peer_evidence) -> stream_options
        {
            stream_options options;
            options.local_identity = identity{std::move(enclave_id), std::move(zone_id)};
            options.backend = std::move(backend);
            options.transcript_id = 1001;
            options.policy = attestation_policy{};
            options.policy.send_local_evidence = send_evidence;
            options.policy.require_peer_evidence = require_peer_evidence;
            options.policy.allow_development_evidence = true;
            options.policy.required_backend_id = fake_backend_id;
            options.handshake_timeout = std::chrono::milliseconds{2000};
            return options;
        }

    protected:
        attested_streaming_spsc_setup_base(
            bool initiator_sends_evidence,
            bool initiator_requires_peer_evidence,
            bool responder_sends_evidence,
            bool responder_requires_peer_evidence)
            : initiator_sends_evidence_(initiator_sends_evidence)
            , initiator_requires_peer_evidence_(initiator_requires_peer_evidence)
            , responder_sends_evidence_(responder_sends_evidence)
            , responder_requires_peer_evidence_(responder_requires_peer_evidence)
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

            initiator_stream_ = std::make_shared<streaming::attestation::stream>(
                std::move(initiator_raw),
                make_options(
                    backend, "initiator-enclave", "initiator-zone", initiator_sends_evidence_, initiator_requires_peer_evidence_));
            responder_stream_ = std::make_shared<streaming::attestation::stream>(
                std::move(responder_raw),
                make_options(
                    backend, "responder-enclave", "responder-zone", responder_sends_evidence_, responder_requires_peer_evidence_));

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
                  false)
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
    CORO_ASSERT_EQ(initiator_context.peer_attested, lib.initiator_requires_peer_evidence());
    CORO_ASSERT_EQ(responder_context.peer_attested, lib.responder_requires_peer_evidence());
    CORO_ASSERT_EQ(initiator_context.local_identity.enclave_id, std::string{"initiator-enclave"});
    CORO_ASSERT_EQ(initiator_context.peer_identity.enclave_id, std::string{"responder-enclave"});
    CORO_ASSERT_EQ(responder_context.local_identity.enclave_id, std::string{"responder-enclave"});
    CORO_ASSERT_EQ(responder_context.peer_identity.enclave_id, std::string{"initiator-enclave"});
    CORO_ASSERT_EQ(initiator_context.backend_id, std::string{fake_backend_id});
    CORO_ASSERT_EQ(responder_context.backend_id, std::string{fake_backend_id});

    int result = 0;
    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->add(20, 22, result), rpc::error::OK());
    CORO_ASSERT_EQ(result, 42);

    std::vector<uint8_t> input(4096);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<uint8_t>(i & 0xffU);

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
