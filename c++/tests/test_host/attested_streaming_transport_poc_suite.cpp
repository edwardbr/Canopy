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
#  include <security/attestation/protected_rpc.h>
#  include <streaming/attestation/stream.h>
#  include <streaming/spsc_queue/stream.h>
#  include <transport/tests/streaming_setup_base.h>
#  ifdef CANOPY_BUILD_ENCLAVE
#    include <transports/sgx_coroutine/enclave/service.h>
#  endif
#endif

#ifdef CANOPY_BUILD_COROUTINE
namespace
{
    using canopy::security::attestation::attestation_policy;
    using canopy::security::attestation::attestation_service;
    using canopy::security::attestation::attestation_service_options;
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

    auto make_test_attestation_service(
        const std::shared_ptr<fake_backend>& backend,
        std::string enclave_id,
        std::string zone_id,
        bool send_evidence,
        bool require_peer_evidence) -> std::shared_ptr<attestation_service>
    {
        attestation_service_options options;
        options.local_identity = identity{std::move(enclave_id), std::move(zone_id)};
        options.backend = backend;
        options.policy = attestation_policy{};
        options.policy.send_local_evidence = send_evidence;
        options.policy.require_peer_evidence = require_peer_evidence;
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
        bool responder_sends_evidence_{true};
        bool responder_requires_peer_evidence_{true};

        static auto make_attestation_service(
            std::shared_ptr<fake_backend> backend,
            std::string enclave_id,
            std::string zone_id,
            bool send_evidence,
            bool require_peer_evidence) -> std::shared_ptr<attestation_service>
        {
            return make_test_attestation_service(
                backend, std::move(enclave_id), std::move(zone_id), send_evidence, require_peer_evidence);
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

            initiator_attestation_service_ = make_attestation_service(
                backend, "initiator-enclave", "initiator-zone", initiator_sends_evidence_, initiator_requires_peer_evidence_);
            responder_attestation_service_ = make_attestation_service(
                backend, "responder-enclave", "responder-zone", responder_sends_evidence_, responder_requires_peer_evidence_);

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
                  false)
        {
        }
    };

#  ifdef CANOPY_BUILD_ENCLAVE
    auto make_service_level_route_zone(uint64_t subnet) -> rpc::zone
    {
        auto zone = rpc::DEFAULT_PREFIX;
        auto set_result = zone.set_subnet(subnet);
        EXPECT_TRUE(set_result.has_value());
        return zone;
    }

    auto make_attested_security_context(
        attestation_service& service,
        identity peer_identity,
        uint64_t transcript_id) -> security_context
    {
        canopy::security::attestation::establish_session_params params;
        params.peer_identity = std::move(peer_identity);
        params.transcript_id = transcript_id;
        params.local_evidence_sent = true;
        params.peer_attested = true;
        params.verified_backend_id = fake_backend_id;
        params.verified_level = canopy::security::attestation::security_level::development;
        return service.establish_session(params);
    }

    auto is_valid_encrypted_payload(const std::vector<char>& payload) -> bool
    {
        rpc::encrypted_payload encrypted_payload;
        auto err = rpc::from_yas_binary(rpc::byte_span(payload), encrypted_payload);
        return err.empty() && !encrypted_payload.session_id.empty() && encrypted_payload.session_epoch != 0
               && encrypted_payload.e2e_counter != 0 && !encrypted_payload.payload.empty()
               && !encrypted_payload.authentication_tag.empty();
    }

    struct protected_rpc_runtime_observer
    {
        size_t protected_send_count{0};
        size_t plaintext_send_count{0};
        size_t protected_send_response_count{0};
        size_t plaintext_send_response_count{0};
        size_t protected_post_count{0};
        size_t plaintext_post_count{0};
        size_t protected_add_ref_count{0};
        size_t plaintext_add_ref_count{0};
        bool malformed_encrypted_payload{false};
    };

    void install_protected_rpc_runtime_observers(
        const std::shared_ptr<rpc::stream_transport::transport>& initiator_transport,
        const std::shared_ptr<rpc::stream_transport::transport>& responder_transport,
        protected_rpc_runtime_observer& observer)
    {
        responder_transport->add_typed_message_handler<rpc::stream_transport::call_send>(
            [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::call_send& request)
                -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
            {
                if (canopy::security::attestation::is_protected_rpc_envelope(
                        request.interface_id, request.method_id, prefix.version))
                {
                    ++observer.protected_send_count;
                    observer.malformed_encrypted_payload
                        = observer.malformed_encrypted_payload || !is_valid_encrypted_payload(request.payload);
                }
                else
                {
                    ++observer.plaintext_send_count;
                }
                CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
            });

        responder_transport->add_typed_message_handler<rpc::stream_transport::post_send>(
            [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::post_send& request)
                -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
            {
                if (canopy::security::attestation::is_protected_rpc_envelope(
                        request.interface_id, request.method_id, prefix.version))
                {
                    ++observer.protected_post_count;
                    observer.malformed_encrypted_payload
                        = observer.malformed_encrypted_payload || !is_valid_encrypted_payload(request.payload);
                }
                else
                {
                    ++observer.plaintext_post_count;
                }
                CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
            });

        initiator_transport->add_typed_message_handler<rpc::stream_transport::call_receive>(
            [&observer](auto, const auto&, const auto&, const rpc::stream_transport::call_receive& response)
                -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
            {
                if (is_valid_encrypted_payload(response.payload))
                {
                    ++observer.protected_send_response_count;
                }
                else
                {
                    ++observer.plaintext_send_response_count;
                }
                CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
            });

        auto add_ref_handler
            = [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::addref_send& request)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            if (canopy::security::attestation::is_protected_rpc_payload(request.payload_type_id, prefix.version))
            {
                ++observer.protected_add_ref_count;
                observer.malformed_encrypted_payload
                    = observer.malformed_encrypted_payload || !is_valid_encrypted_payload(request.payload);
            }
            else
            {
                ++observer.plaintext_add_ref_count;
            }
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::addref_send>(add_ref_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::addref_send>(std::move(add_ref_handler));
    }

    struct service_level_route_handshake_expectation
    {
        route_attestation_status initiator_route_status{route_attestation_status::attested};
        route_attestation_status responder_route_status{route_attestation_status::attested};
        bool initiator_context_established{true};
        bool responder_context_established{true};
    };

    CORO_TASK(bool)
    coro_add_ref_drives_service_level_route_handshake(
        const std::shared_ptr<coro::scheduler>& scheduler,
        bool initiator_sends_evidence,
        bool initiator_requires_peer_evidence,
        bool responder_sends_evidence,
        bool responder_requires_peer_evidence,
        service_level_route_handshake_expectation expected)
    {
        auto initiator_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto responder_zone = make_service_level_route_zone(service_level_route_responder_subnet);

        auto initiator_service = std::make_shared<rpc::enclave_service>(
            "service-level-route-initiator", initiator_zone, responder_zone, scheduler);
        auto responder_service = std::make_shared<rpc::enclave_service>(
            "service-level-route-responder", responder_zone, initiator_zone, scheduler);

        auto backend = std::make_shared<fake_backend>();
        initiator_service->set_attestation_service(make_test_attestation_service(
            backend,
            "service-level-initiator-enclave",
            "service-level-initiator-zone",
            initiator_sends_evidence,
            initiator_requires_peer_evidence));
        responder_service->set_attestation_service(make_test_attestation_service(
            backend,
            "service-level-responder-enclave",
            "service-level-responder-zone",
            responder_sends_evidence,
            responder_requires_peer_evidence));
        initiator_service->set_add_ref_attestation_required(true);
        responder_service->set_add_ref_attestation_required(true);

        CORO_ASSERT_EQ(
            initiator_service->get_attestation_route_state(responder_zone).status, route_attestation_status::unknown);
        CORO_ASSERT_EQ(
            responder_service->get_attestation_route_state(initiator_zone).status, route_attestation_status::unknown);

        auto send_queue = std::make_shared<streaming::spsc_queue::queue_type>();
        auto receive_queue = std::make_shared<streaming::spsc_queue::queue_type>();
        auto initiator_stream = std::make_shared<streaming::spsc_queue::stream>(send_queue, receive_queue, scheduler);
        auto responder_stream = std::make_shared<streaming::spsc_queue::stream>(receive_queue, send_queue, scheduler);

        rpc::stream_transport::stream_transport_options options{
            .call_timeout = service_level_route_call_timeout,
            .call_timeout_sweep = service_level_route_call_timeout_sweep,
        };
        auto initiator_transport = rpc::stream_transport::make_client(
            "service-level-route-initiator-transport", initiator_service, std::move(initiator_stream), options);
        auto responder_transport = rpc::stream_transport::make_client(
            "service-level-route-responder-transport", responder_service, std::move(responder_stream), options);
        initiator_transport->set_adjacent_zone_id(responder_zone);
        responder_transport->set_adjacent_zone_id(initiator_zone);
        initiator_service->add_transport(responder_zone, initiator_transport);
        responder_service->add_transport(initiator_zone, responder_transport);

        CORO_ASSERT_EQ(initiator_service->spawn(initiator_transport->pump_send_and_receive()), true);
        CORO_ASSERT_EQ(responder_service->spawn(responder_transport->pump_send_and_receive()), true);

        auto remote_object = responder_zone.with_object(rpc::dummy_object_id);
        CORO_ASSERT_EQ(remote_object.has_value(), true);

        rpc::add_ref_params params;
        params.protocol_version = rpc::get_version();
        params.remote_object_id = *remote_object;
        params.caller_zone_id = initiator_zone;
        params.requesting_zone_id = initiator_zone;
        params.build_out_param_channel = rpc::add_ref_options::normal;

        auto result = CO_AWAIT initiator_service->outbound_add_ref(std::move(params), initiator_transport);
        CORO_ASSERT_EQ(result.error_code, rpc::error::OK());

        auto initiator_state = initiator_service->get_attestation_route_state(responder_zone);
        auto responder_state = responder_service->get_attestation_route_state(initiator_zone);
        CORO_ASSERT_EQ(initiator_state.status, expected.initiator_route_status);
        CORO_ASSERT_EQ(responder_state.status, expected.responder_route_status);
        CORO_ASSERT_EQ(
            initiator_state.context && initiator_state.context->established, expected.initiator_context_established);
        CORO_ASSERT_EQ(
            responder_state.context && responder_state.context->established, expected.responder_context_established);

        if (initiator_state.context)
        {
            CORO_ASSERT_EQ(
                initiator_state.context->peer_identity.enclave_id, std::string{"service-level-responder-enclave"});
        }
        if (responder_state.context)
        {
            CORO_ASSERT_EQ(
                responder_state.context->peer_identity.enclave_id, std::string{"service-level-initiator-enclave"});
        }

        std::static_pointer_cast<rpc::transport>(initiator_transport)->set_status(rpc::transport_status::DISCONNECTED);
        std::static_pointer_cast<rpc::transport>(responder_transport)->set_status(rpc::transport_status::DISCONNECTED);
        for (size_t i = 0; i < service_level_route_cleanup_drain_iterations; ++i)
            CO_AWAIT initiator_service->schedule();

        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_generated_rpc_runs_through_enclave_service_protected_send_post(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto initiator_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto responder_zone = make_service_level_route_zone(service_level_route_responder_subnet);

        auto initiator_service = std::make_shared<rpc::enclave_service>(
            "protected-rpc-runtime-initiator", initiator_zone, responder_zone, scheduler);
        auto responder_service = std::make_shared<rpc::enclave_service>(
            "protected-rpc-runtime-responder", responder_zone, initiator_zone, scheduler);

        auto backend = std::make_shared<fake_backend>();
        auto initiator_identity = identity{"protected-runtime-initiator-enclave", "protected-runtime-initiator-zone"};
        auto responder_identity = identity{"protected-runtime-responder-enclave", "protected-runtime-responder-zone"};
        auto initiator_attestation_service = make_test_attestation_service(
            backend, initiator_identity.enclave_id, initiator_identity.zone_id, true, true);
        auto responder_attestation_service = make_test_attestation_service(
            backend, responder_identity.enclave_id, responder_identity.zone_id, true, true);

        auto initiator_context = make_attested_security_context(
            *initiator_attestation_service, responder_identity, protected_rpc_generated_runtime_transcript_id);
        auto responder_context = make_attested_security_context(
            *responder_attestation_service, initiator_identity, protected_rpc_generated_runtime_transcript_id);
        CORO_ASSERT_EQ(initiator_context.established, true);
        CORO_ASSERT_EQ(responder_context.established, true);
        CORO_ASSERT_EQ(initiator_context.session_id, responder_context.session_id);

        initiator_service->set_attestation_service(initiator_attestation_service);
        responder_service->set_attestation_service(responder_attestation_service);
        initiator_service->set_security_context(responder_zone, std::move(initiator_context));
        responder_service->set_security_context(initiator_zone, std::move(responder_context));
        initiator_service->set_protected_rpc_enabled(true);
        responder_service->set_protected_rpc_enabled(true);
        initiator_service->set_add_ref_attestation_required(true);
        responder_service->set_add_ref_attestation_required(true);

        auto send_queue = std::make_shared<streaming::spsc_queue::queue_type>();
        auto receive_queue = std::make_shared<streaming::spsc_queue::queue_type>();
        auto responder_stream = std::make_shared<streaming::spsc_queue::stream>(receive_queue, send_queue, scheduler);
        auto initiator_stream = std::make_shared<streaming::spsc_queue::stream>(send_queue, receive_queue, scheduler);

        rpc::stream_transport::stream_transport_options options{
            .call_timeout = service_level_route_call_timeout,
            .call_timeout_sweep = service_level_route_call_timeout_sweep,
        };

        auto factory = [](rpc::shared_ptr<yyy::i_host> host_ptr,
                           std::shared_ptr<rpc::service> svc) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
        {
            auto example = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(svc, host_ptr));
            CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(example)};
        };

        auto responder_transport = std::static_pointer_cast<rpc::stream_transport::transport>(
            CO_AWAIT responder_service->make_acceptor<yyy::i_host, yyy::i_example>(
                "protected-rpc-runtime-responder-transport",
                rpc::stream_transport::transport_factory(std::move(responder_stream), options),
                std::move(factory)));
        CO_AWAIT responder_transport->accept();

        auto initiator_transport = rpc::stream_transport::make_client(
            "protected-rpc-runtime-initiator-transport", initiator_service, std::move(initiator_stream), options);

        protected_rpc_runtime_observer observer;
        install_protected_rpc_runtime_observers(initiator_transport, responder_transport, observer);

        rpc::shared_ptr<yyy::i_host> host_ptr(new host());
        current_host_service = initiator_service;
        auto connect_result = CO_AWAIT initiator_service->connect_to_zone<yyy::i_host, yyy::i_example>(
            "protected-rpc-runtime-main-child", initiator_transport, host_ptr);
        CORO_ASSERT_EQ(connect_result.error_code, rpc::error::OK());
        auto example = std::move(connect_result.output_interface);
        CORO_ASSERT_NE(example, nullptr);

        int add_result = 0;
        CORO_ASSERT_EQ(
            CO_AWAIT example->add(arithmetic_test_left_value, arithmetic_test_right_value, add_result), rpc::error::OK());
        CORO_ASSERT_EQ(add_result, arithmetic_test_expected_result);

        rpc::shared_ptr<xxx::i_foo> foo;
        CORO_ASSERT_EQ(CO_AWAIT example->create_foo(foo), rpc::error::OK());
        CORO_ASSERT_NE(foo, nullptr);
        CORO_ASSERT_EQ(CO_AWAIT foo->clear_recorded_messages(), rpc::error::OK());

        for (size_t i = 0; i < protected_rpc_generated_runtime_post_count; ++i)
        {
            auto post_result = CO_AWAIT foo->record_message(static_cast<int>(i));
            CORO_ASSERT_EQ(post_result, rpc::error::OK());
        }

        for (size_t i = 0; i < protected_rpc_generated_runtime_post_count; ++i)
            CO_AWAIT initiator_service->schedule();

        std::vector<int> recorded_messages;
        CORO_ASSERT_EQ(CO_AWAIT foo->get_recorded_messages(recorded_messages), rpc::error::OK());
        CORO_ASSERT_EQ(recorded_messages.size(), protected_rpc_generated_runtime_post_count);
        for (size_t i = 0; i < recorded_messages.size(); ++i)
            CORO_ASSERT_EQ(recorded_messages[i], static_cast<int>(i));

        CORO_ASSERT_EQ(observer.plaintext_send_count, 0U);
        CORO_ASSERT_EQ(observer.plaintext_send_response_count, 0U);
        CORO_ASSERT_EQ(observer.plaintext_post_count, 0U);
        CORO_ASSERT_EQ(observer.plaintext_add_ref_count, 0U);
        CORO_ASSERT_EQ(observer.malformed_encrypted_payload, false);
        CORO_ASSERT_EQ(observer.protected_send_count >= protected_rpc_generated_runtime_min_send_count, true);
        CORO_ASSERT_EQ(observer.protected_send_count, observer.protected_send_response_count);
        CORO_ASSERT_EQ(observer.protected_post_count, protected_rpc_generated_runtime_post_count);
        CORO_ASSERT_EQ(observer.protected_add_ref_count > 0U, true);

        foo = nullptr;
        example = nullptr;
        host_ptr = nullptr;
        current_host_service.reset();

        for (size_t i = 0; i < protected_rpc_generated_runtime_disconnect_drain_iterations
                           && (initiator_transport->get_status() != rpc::transport_status::DISCONNECTED
                               || responder_transport->get_status() != rpc::transport_status::DISCONNECTED);
            ++i)
        {
            CO_AWAIT initiator_service->schedule();
        }

        if (initiator_transport->get_status() != rpc::transport_status::DISCONNECTED)
            std::static_pointer_cast<rpc::transport>(initiator_transport)->set_status(rpc::transport_status::DISCONNECTED);
        if (responder_transport->get_status() != rpc::transport_status::DISCONNECTED)
            std::static_pointer_cast<rpc::transport>(responder_transport)->set_status(rpc::transport_status::DISCONNECTED);

        for (size_t i = 0; i < service_level_route_cleanup_drain_iterations; ++i)
            CO_AWAIT initiator_service->schedule();

        CO_RETURN true;
    }

    void run_service_level_route_handshake_test(
        bool initiator_sends_evidence,
        bool initiator_requires_peer_evidence,
        bool responder_sends_evidence,
        bool responder_requires_peer_evidence,
        service_level_route_handshake_expectation expected)
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_add_ref_drives_service_level_route_handshake(
                scheduler,
                initiator_sends_evidence,
                initiator_requires_peer_evidence,
                responder_sends_evidence,
                responder_requires_peer_evidence,
                expected);
            done.store(true);
            CO_RETURN;
        };

        RPC_ASSERT(scheduler->spawn_detached(task()));
        const auto deadline = std::chrono::steady_clock::now() + service_level_route_test_timeout;
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        ASSERT_TRUE(done.load());
        EXPECT_TRUE(result);
        scheduler->shutdown();
    }

    void run_generated_rpc_protected_send_post_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_generated_rpc_runs_through_enclave_service_protected_send_post(scheduler);
            done.store(true);
            CO_RETURN;
        };

        RPC_ASSERT(scheduler->spawn_detached(task()));
        const auto deadline = std::chrono::steady_clock::now() + service_level_route_test_timeout;
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            scheduler->process_events(std::chrono::milliseconds{1});

        ASSERT_TRUE(done.load());
        EXPECT_TRUE(result);
        scheduler->shutdown();
    }
#  endif
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

#  ifdef CANOPY_BUILD_ENCLAVE
TEST(
    ServiceLevelRouteAttestation,
    AddRefDrivesMutualHandshakeOverStreamingTransport)
{
    run_service_level_route_handshake_test(
        true,
        true,
        true,
        true,
        service_level_route_handshake_expectation{.initiator_route_status = route_attestation_status::attested,
            .responder_route_status = route_attestation_status::attested,
            .initiator_context_established = true,
            .responder_context_established = true});
}

TEST(
    ServiceLevelRouteAttestation,
    AddRefAllowsExplicitUnattestedClientOverStreamingTransport)
{
    run_service_level_route_handshake_test(
        false,
        true,
        true,
        false,
        service_level_route_handshake_expectation{.initiator_route_status = route_attestation_status::attested,
            .responder_route_status = route_attestation_status::unattested_allowed,
            .initiator_context_established = true,
            .responder_context_established = false});
}

TEST(
    ProtectedRpcRuntime,
    GeneratedRpcSendAndPostUseEnclaveServiceProtection)
{
    run_generated_rpc_protected_send_post_test();
}
#  endif
#endif
