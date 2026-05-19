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
#  include <security/attestation/fake_backend.h>
#  include <security/attestation/protected_rpc.h>
#  include <streaming/attestation/stream.h>
#  include <streaming/spsc_queue/stream.h>
#  include <transports/local/transport.h>
#  include <transport/tests/streaming_setup_base.h>
#  include <optional>
#  include <unordered_map>
#  ifdef CANOPY_BUILD_ENCLAVE
#    include <transports/sgx_coroutine/enclave/local_transport.h>
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
    constexpr uint64_t enclave_local_subject_zone_subnet = 8192;
    constexpr uint64_t enclave_local_adjacent_zone_subnet = 8193;
    constexpr uint64_t enclave_local_referenced_zone_subnet = 8194;
    constexpr uint64_t concurrent_route_first_object_id = 201;
    constexpr uint64_t concurrent_route_second_object_id = 202;
    constexpr int enclave_local_post_message_value = 77;

    auto make_test_attestation_service(
        const std::shared_ptr<fake_backend>& backend,
        std::string enclave_id,
        std::string zone_id,
        bool send_evidence,
        bool require_peer_evidence,
        bool allow_unattested_peer = false) -> std::shared_ptr<attestation_service>
    {
        attestation_service_options options;
        options.local_identity = identity{std::move(enclave_id), std::move(zone_id)};
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
            std::string enclave_id,
            std::string zone_id,
            bool send_evidence,
            bool require_peer_evidence,
            bool allow_unattested_peer) -> std::shared_ptr<attestation_service>
        {
            return make_test_attestation_service(
                backend, std::move(enclave_id), std::move(zone_id), send_evidence, require_peer_evidence, allow_unattested_peer);
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
                "initiator-enclave",
                "initiator-zone",
                initiator_sends_evidence_,
                initiator_requires_peer_evidence_,
                initiator_allows_unattested_peer_);
            responder_attestation_service_ = make_attestation_service(
                backend,
                "responder-enclave",
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

    auto is_valid_encrypted_payload(
        const std::vector<char>& payload,
        rpc::encoding encoding = rpc::encoding::yas_binary) -> bool
    {
        rpc::encrypted_payload encrypted_payload;
        auto err = rpc::deserialise(encoding, rpc::byte_span(payload), encrypted_payload);
        return err.empty() && !encrypted_payload.session_id.empty() && encrypted_payload.session_epoch != 0
               && encrypted_payload.e2e_counter != 0 && !encrypted_payload.payload.empty()
               && !encrypted_payload.authentication_tag.empty();
    }

    auto is_valid_encrypted_payload(const std::optional<rpc::typed_payload>& payload) -> bool
    {
        return payload.has_value() && is_valid_encrypted_payload(payload->get_payload(), payload->get_encoding());
    }

    auto make_route_handshake_params(
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        const rpc::route_attestation_handshake_request& request,
        rpc::encoding payload_encoding = rpc::encoding::yas_binary) -> rpc::handshake_params
    {
        rpc::handshake_params params;
        params.protocol_version = rpc::get_version();
        params.caller_zone_id = caller_zone_id;
        params.destination_zone_id = destination_zone_id;
        params.type_id = rpc::id<rpc::route_attestation_handshake_request>::get(params.protocol_version);
        params.payload_encoding = payload_encoding;
        params.payload = rpc::serialise<std::vector<char>>(request, payload_encoding);
        return params;
    }

    auto is_valid_public_control_status(int error_code) -> bool
    {
        return error_code == rpc::error::OK() || rpc::error::is_error(error_code);
    }

    auto is_route_attestation_handshake_request_type(
        uint64_t type_id,
        uint64_t protocol_version) -> bool
    {
        return type_id == rpc::id<rpc::route_attestation_handshake_request>::get(protocol_version);
    }

    auto is_route_attestation_handshake_response_type(
        uint64_t type_id,
        uint64_t protocol_version) -> bool
    {
        return type_id == rpc::id<rpc::route_attestation_handshake_response>::get(protocol_version);
    }

    struct protected_rpc_runtime_observer
    {
        size_t stream_init_send_count{0};
        size_t stream_init_initial_response_count{0};
        size_t stream_init_response_count{0};
        size_t route_handshake_send_count{0};
        size_t route_handshake_response_count{0};
        size_t get_new_zone_id_send_count{0};
        size_t get_new_zone_id_response_count{0};
        size_t close_connection_send_count{0};
        size_t close_connection_ack_count{0};
        size_t protected_send_count{0};
        size_t plaintext_send_count{0};
        size_t protected_send_response_count{0};
        size_t plaintext_send_response_count{0};
        size_t protected_post_count{0};
        size_t plaintext_post_count{0};
        size_t protected_try_cast_count{0};
        size_t plaintext_try_cast_count{0};
        size_t protected_add_ref_count{0};
        size_t plaintext_add_ref_count{0};
        size_t protected_release_count{0};
        size_t plaintext_release_count{0};
        size_t protected_object_released_count{0};
        size_t plaintext_object_released_count{0};
        size_t protected_transport_down_count{0};
        size_t plaintext_transport_down_count{0};
        std::unordered_map<uint64_t, rpc::encoding> protected_send_response_encoding_by_sequence;
        bool malformed_encrypted_payload{false};
        bool protected_object_id_visible{false};
        bool non_rpc_public_control_status_visible{false};
        bool unexpected_route_handshake_type_visible{false};
    };

#    ifdef CANOPY_BUILD_ENCLAVE
    class positive_control_status_transport final : public rpc::transport
    {
    public:
        static constexpr int positive_control_status = 42;
        static constexpr uint64_t positive_handshake_type_id = 1234;
        static constexpr char positive_handshake_payload_byte = 'x';

        positive_control_status_transport(
            rpc::zone zone_id,
            rpc::zone adjacent_zone_id)
            : rpc::transport(
                  "positive-control-status-transport",
                  zone_id)
        {
            set_adjacent_zone_id(adjacent_zone_id);
            set_status(rpc::transport_status::CONNECTED);
        }

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub>,
            rpc::connection_settings) override
        {
            CO_RETURN rpc::connect_result{rpc::error::NOT_IMPLEMENTED(), {}};
        }

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        CORO_TASK(rpc::send_result) outbound_send(rpc::send_params) override
        {
            CO_RETURN rpc::send_result{rpc::error::NOT_IMPLEMENTED(), {}, {}};
        }

        CORO_TASK(void) outbound_post(rpc::post_params) override { CO_RETURN; }

        CORO_TASK(rpc::standard_result) outbound_try_cast(rpc::try_cast_params params) override
        {
            ++try_cast_count;
            try_cast_was_protected
                = canopy::security::attestation::is_protected_rpc_payload(params.payload, params.protocol_version);
            CO_RETURN rpc::standard_result{positive_control_status, {}};
        }

        CORO_TASK(rpc::standard_result) outbound_add_ref(rpc::add_ref_params params) override
        {
            ++add_ref_count;
            add_ref_was_protected
                = canopy::security::attestation::is_protected_rpc_payload(params.payload, params.protocol_version);
            CO_RETURN rpc::standard_result{positive_control_status, {}};
        }

        CORO_TASK(rpc::standard_result) outbound_release(rpc::release_params params) override
        {
            ++release_count;
            release_was_protected
                = canopy::security::attestation::is_protected_rpc_payload(params.payload, params.protocol_version);
            CO_RETURN rpc::standard_result{positive_control_status, {}};
        }

        CORO_TASK(rpc::handshake_result) outbound_handshake(rpc::handshake_params) override
        {
            CO_RETURN rpc::handshake_result{
                positive_control_status, positive_handshake_type_id, {positive_handshake_payload_byte}, {}};
        }

        CORO_TASK(rpc::new_zone_id_result) outbound_get_new_zone_id(rpc::get_new_zone_id_params) override
        {
            CO_RETURN rpc::new_zone_id_result{positive_control_status, get_adjacent_zone_id(), {}};
        }

        CORO_TASK(void) outbound_object_released(rpc::object_released_params) override { CO_RETURN; }
        CORO_TASK(void) outbound_transport_down(rpc::transport_down_params) override { CO_RETURN; }

        size_t try_cast_count{0};
        size_t add_ref_count{0};
        size_t release_count{0};
        bool try_cast_was_protected{false};
        bool add_ref_was_protected{false};
        bool release_was_protected{false};
    };

    class recording_reference_control_transport : public rpc::transport
    {
    public:
        recording_reference_control_transport(
            rpc::zone zone_id,
            rpc::zone adjacent_zone_id)
            : rpc::transport(
                  "recording-reference-control-transport",
                  zone_id)
        {
            set_adjacent_zone_id(adjacent_zone_id);
            set_status(rpc::transport_status::CONNECTED);
        }

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub>,
            rpc::connection_settings) override
        {
            CO_RETURN rpc::connect_result{rpc::error::NOT_IMPLEMENTED(), {}};
        }

        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }
        CORO_TASK(rpc::send_result) outbound_send(rpc::send_params params) override
        {
            ++send_count;
            send_was_protected = canopy::security::attestation::is_protected_rpc_envelope(
                params.interface_id, params.method_id, params.protocol_version);
            last_send_remote_object = params.remote_object_id;
            CO_RETURN rpc::send_result{rpc::error::NOT_IMPLEMENTED(), {}, {}};
        }
        CORO_TASK(void) outbound_post(rpc::post_params params) override
        {
            ++post_count;
            post_was_protected = canopy::security::attestation::is_protected_rpc_envelope(
                params.interface_id, params.method_id, params.protocol_version);
            last_post_remote_object = params.remote_object_id;
            CO_RETURN;
        }
        CORO_TASK(rpc::standard_result) outbound_try_cast(rpc::try_cast_params params) override
        {
            ++try_cast_count;
            try_cast_was_protected
                = canopy::security::attestation::is_protected_rpc_payload(params.payload, params.protocol_version);
            last_try_cast_remote_object = params.remote_object_id;
            CO_RETURN rpc::standard_result{rpc::error::OK(), {}};
        }
        CORO_TASK(rpc::standard_result) outbound_add_ref(rpc::add_ref_params params) override
        {
            ++add_ref_count;
            last_add_ref_remote_object = params.remote_object_id;
            CO_RETURN rpc::standard_result{rpc::error::OK(), {}};
        }
        CORO_TASK(rpc::standard_result) outbound_release(rpc::release_params params) override
        {
            ++release_count;
            last_release_remote_object = params.remote_object_id;
            CO_RETURN rpc::standard_result{rpc::error::OK(), {}};
        }
        CORO_TASK(rpc::handshake_result) outbound_handshake(rpc::handshake_params) override
        {
            CO_RETURN rpc::handshake_result{rpc::error::NOT_IMPLEMENTED(), 0, {}, {}};
        }
        CORO_TASK(rpc::new_zone_id_result) outbound_get_new_zone_id(rpc::get_new_zone_id_params) override
        {
            CO_RETURN rpc::new_zone_id_result{rpc::error::NOT_IMPLEMENTED(), {}, {}};
        }
        CORO_TASK(void) outbound_object_released(rpc::object_released_params params) override
        {
            ++object_released_count;
            object_released_was_protected
                = canopy::security::attestation::is_protected_rpc_payload(params.payload, params.protocol_version);
            last_object_released_remote_object = params.remote_object_id;
            last_object_released_caller_zone = params.caller_zone_id;
            CO_RETURN;
        }
        CORO_TASK(void) outbound_transport_down(rpc::transport_down_params params) override
        {
            ++transport_down_count;
            transport_down_was_protected
                = canopy::security::attestation::is_protected_rpc_payload(params.payload, params.protocol_version);
            last_transport_down_destination_zone = params.destination_zone_id;
            last_transport_down_caller_zone = params.caller_zone_id;
            CO_RETURN;
        }

        size_t send_count{0};
        size_t post_count{0};
        size_t try_cast_count{0};
        size_t add_ref_count{0};
        size_t release_count{0};
        size_t object_released_count{0};
        size_t transport_down_count{0};
        bool send_was_protected{false};
        bool post_was_protected{false};
        bool try_cast_was_protected{false};
        bool object_released_was_protected{false};
        bool transport_down_was_protected{false};
        rpc::remote_object last_send_remote_object;
        rpc::remote_object last_post_remote_object;
        rpc::remote_object last_try_cast_remote_object;
        rpc::remote_object last_add_ref_remote_object;
        rpc::remote_object last_release_remote_object;
        rpc::remote_object last_object_released_remote_object;
        rpc::caller_zone last_object_released_caller_zone;
        rpc::destination_zone last_transport_down_destination_zone;
        rpc::caller_zone last_transport_down_caller_zone;
    };

    class recording_enclave_local_reference_control_transport final : public recording_reference_control_transport,
                                                                      public rpc::sgx::coro::enclave::local_route_transport
    {
    public:
        recording_enclave_local_reference_control_transport(
            rpc::zone zone_id,
            rpc::zone adjacent_zone_id)
            : recording_reference_control_transport(
                  zone_id,
                  adjacent_zone_id)
        {
        }
    };

    class gated_handshake_transport final : public recording_reference_control_transport
    {
    public:
        gated_handshake_transport(
            rpc::zone zone_id,
            rpc::zone adjacent_zone_id,
            std::shared_ptr<coro::scheduler> scheduler)
            : recording_reference_control_transport(
                  zone_id,
                  adjacent_zone_id)
            , scheduler_(std::move(scheduler))
        {
        }

        CORO_TASK(rpc::handshake_result) outbound_handshake(rpc::handshake_params) override
        {
            ++handshake_count;
            handshake_started.store(true);
            while (!release_handshake.load())
                CO_AWAIT scheduler_->schedule_after(std::chrono::milliseconds{1});

            CO_RETURN rpc::handshake_result{handshake_error_code, 0, {}, {}};
        }

        std::shared_ptr<coro::scheduler> scheduler_;
        std::atomic_bool handshake_started{false};
        std::atomic_bool release_handshake{false};
        size_t handshake_count{0};
        int handshake_error_code{rpc::error::TRANSPORT_ERROR()};
    };
#    endif

    class positive_zone_allocator_service final : public rpc::root_service
    {
    public:
        static constexpr int positive_allocator_status = 41;

        positive_zone_allocator_service(
            const char* name,
            rpc::zone zone_id,
            const std::shared_ptr<coro::scheduler>& scheduler)
            : rpc::root_service(
                  name,
                  zone_id,
                  scheduler)
        {
        }

        CORO_TASK(rpc::new_zone_id_result) get_new_zone_id(rpc::get_new_zone_id_params) override
        {
            CO_RETURN rpc::new_zone_id_result{positive_allocator_status, get_zone_id(), {}};
        }
    };

    void install_protected_rpc_runtime_observers(
        const std::shared_ptr<rpc::stream_transport::transport>& initiator_transport,
        const std::shared_ptr<rpc::stream_transport::transport>& responder_transport,
        protected_rpc_runtime_observer& observer)
    {
        auto init_send_handler
            = [&observer](auto, const auto&, const auto&, const rpc::stream_transport::init_client_channel_send&)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            ++observer.stream_init_send_count;
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::init_client_channel_send>(init_send_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::init_client_channel_send>(
            std::move(init_send_handler));

        auto init_initial_response_handler
            = [&observer](
                  auto, const auto&, const auto&, const rpc::stream_transport::init_client_initial_channel_response&)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            ++observer.stream_init_initial_response_count;
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::init_client_initial_channel_response>(
            init_initial_response_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::init_client_initial_channel_response>(
            std::move(init_initial_response_handler));

        auto init_response_handler
            = [&observer](auto, const auto&, const auto&, const rpc::stream_transport::init_client_channel_response& response)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            ++observer.stream_init_response_count;
            observer.non_rpc_public_control_status_visible
                = observer.non_rpc_public_control_status_visible || !is_valid_public_control_status(response.err_code);
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::init_client_channel_response>(
            init_response_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::init_client_channel_response>(
            std::move(init_response_handler));

        auto route_handshake_send_handler
            = [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::handshake_send& request)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            ++observer.route_handshake_send_count;
            observer.unexpected_route_handshake_type_visible
                = observer.unexpected_route_handshake_type_visible
                  || !is_route_attestation_handshake_request_type(request.type_id, prefix.version);
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::handshake_send>(route_handshake_send_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::handshake_send>(
            std::move(route_handshake_send_handler));

        auto route_handshake_response_handler
            = [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::handshake_receive& response)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            ++observer.route_handshake_response_count;
            observer.non_rpc_public_control_status_visible
                = observer.non_rpc_public_control_status_visible || !is_valid_public_control_status(response.err_code);
            if (response.err_code == rpc::error::OK())
            {
                observer.unexpected_route_handshake_type_visible
                    = observer.unexpected_route_handshake_type_visible
                      || !is_route_attestation_handshake_response_type(response.type_id, prefix.version);
            }
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::handshake_receive>(
            route_handshake_response_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::handshake_receive>(
            std::move(route_handshake_response_handler));

        auto get_new_zone_id_send_handler
            = [&observer](auto, const auto&, const auto&, const rpc::stream_transport::get_new_zone_id_send&)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            ++observer.get_new_zone_id_send_count;
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::get_new_zone_id_send>(
            get_new_zone_id_send_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::get_new_zone_id_send>(
            std::move(get_new_zone_id_send_handler));

        auto get_new_zone_id_response_handler
            = [&observer](auto, const auto&, const auto&, const rpc::stream_transport::get_new_zone_id_receive& response)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            ++observer.get_new_zone_id_response_count;
            observer.non_rpc_public_control_status_visible
                = observer.non_rpc_public_control_status_visible || !is_valid_public_control_status(response.err_code);
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::get_new_zone_id_receive>(
            get_new_zone_id_response_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::get_new_zone_id_receive>(
            std::move(get_new_zone_id_response_handler));

        auto close_send_handler
            = [&observer](auto, const auto&, const auto&, const rpc::stream_transport::close_connection_send&)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            ++observer.close_connection_send_count;
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::close_connection_send>(close_send_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::close_connection_send>(
            std::move(close_send_handler));

        auto close_ack_handler
            = [&observer](auto, const auto&, const auto&, const rpc::stream_transport::close_connection_ack&)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            ++observer.close_connection_ack_count;
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::close_connection_ack>(close_ack_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::close_connection_ack>(
            std::move(close_ack_handler));

        responder_transport->add_typed_message_handler<rpc::stream_transport::call_send>(
            [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::call_send& request)
                -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
            {
                if (canopy::security::attestation::is_protected_rpc_envelope(
                        request.interface_id, request.method_id, prefix.version))
                {
                    ++observer.protected_send_count;
                    observer.malformed_encrypted_payload
                        = observer.malformed_encrypted_payload
                          || !is_valid_encrypted_payload(request.payload, request.encoding);
                    observer.protected_object_id_visible
                        = observer.protected_object_id_visible || request.destination_zone_id.get_object_id().is_set();
                    observer.protected_send_response_encoding_by_sequence[prefix.sequence_number] = request.encoding;
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
                        = observer.malformed_encrypted_payload
                          || !is_valid_encrypted_payload(request.payload, request.encoding);
                    observer.protected_object_id_visible
                        = observer.protected_object_id_visible || request.destination_zone_id.get_object_id().is_set();
                }
                else
                {
                    ++observer.plaintext_post_count;
                }
                CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
            });

        initiator_transport->add_typed_message_handler<rpc::stream_transport::call_receive>(
            [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::call_receive& response)
                -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
            {
                auto encoding = observer.protected_send_response_encoding_by_sequence.find(prefix.sequence_number);
                if (encoding != observer.protected_send_response_encoding_by_sequence.end()
                    && is_valid_encrypted_payload(response.payload, encoding->second))
                {
                    ++observer.protected_send_response_count;
                    observer.protected_send_response_encoding_by_sequence.erase(encoding);
                    observer.non_rpc_public_control_status_visible = observer.non_rpc_public_control_status_visible
                                                                     || !is_valid_public_control_status(response.err_code);
                }
                else
                {
                    ++observer.plaintext_send_response_count;
                }
                CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
            });

        auto try_cast_receive_handler
            = [&observer](auto, const auto&, const auto&, const rpc::stream_transport::try_cast_receive& response)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            observer.non_rpc_public_control_status_visible
                = observer.non_rpc_public_control_status_visible || !is_valid_public_control_status(response.err_code);
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::try_cast_receive>(try_cast_receive_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::try_cast_receive>(
            std::move(try_cast_receive_handler));

        auto add_ref_receive_handler
            = [&observer](auto, const auto&, const auto&, const rpc::stream_transport::addref_receive& response)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            observer.non_rpc_public_control_status_visible
                = observer.non_rpc_public_control_status_visible || !is_valid_public_control_status(response.err_code);
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::addref_receive>(add_ref_receive_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::addref_receive>(
            std::move(add_ref_receive_handler));

        auto add_ref_handler
            = [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::addref_send& request)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            if (canopy::security::attestation::is_protected_rpc_payload(request.payload, prefix.version))
            {
                ++observer.protected_add_ref_count;
                observer.malformed_encrypted_payload
                    = observer.malformed_encrypted_payload || !is_valid_encrypted_payload(request.payload);
                observer.protected_object_id_visible
                    = observer.protected_object_id_visible || request.destination_zone_id.get_object_id().is_set();
            }
            else
            {
                ++observer.plaintext_add_ref_count;
            }
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::addref_send>(add_ref_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::addref_send>(std::move(add_ref_handler));

        auto try_cast_handler
            = [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::try_cast_send& request)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            if (canopy::security::attestation::is_protected_rpc_payload(request.payload, prefix.version))
            {
                ++observer.protected_try_cast_count;
                observer.malformed_encrypted_payload
                    = observer.malformed_encrypted_payload || !is_valid_encrypted_payload(request.payload);
                observer.protected_object_id_visible
                    = observer.protected_object_id_visible || request.destination_zone_id.get_object_id().is_set();
            }
            else
            {
                ++observer.plaintext_try_cast_count;
            }
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::try_cast_send>(try_cast_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::try_cast_send>(std::move(try_cast_handler));

        auto release_handler
            = [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::release_send& request)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            if (canopy::security::attestation::is_protected_rpc_payload(request.payload, prefix.version))
            {
                ++observer.protected_release_count;
                observer.malformed_encrypted_payload
                    = observer.malformed_encrypted_payload || !is_valid_encrypted_payload(request.payload);
                observer.protected_object_id_visible
                    = observer.protected_object_id_visible || request.destination_zone_id.get_object_id().is_set();
            }
            else
            {
                ++observer.plaintext_release_count;
            }
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::release_send>(release_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::release_send>(std::move(release_handler));

        auto object_released_handler
            = [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::object_released_send& request)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            if (canopy::security::attestation::is_protected_rpc_payload(request.payload, prefix.version))
            {
                ++observer.protected_object_released_count;
                observer.malformed_encrypted_payload
                    = observer.malformed_encrypted_payload || !is_valid_encrypted_payload(request.payload);
                observer.protected_object_id_visible
                    = observer.protected_object_id_visible || request.destination_zone_id.get_object_id().is_set();
            }
            else
            {
                ++observer.plaintext_object_released_count;
            }
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::object_released_send>(
            object_released_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::object_released_send>(
            std::move(object_released_handler));

        auto transport_down_handler
            = [&observer](auto, const auto& prefix, const auto&, const rpc::stream_transport::transport_down_send& request)
            -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
        {
            if (canopy::security::attestation::is_protected_rpc_payload(request.payload, prefix.version))
            {
                ++observer.protected_transport_down_count;
                observer.malformed_encrypted_payload
                    = observer.malformed_encrypted_payload || !is_valid_encrypted_payload(request.payload);
            }
            else
            {
                ++observer.plaintext_transport_down_count;
            }
            CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
        };

        responder_transport->add_typed_message_handler<rpc::stream_transport::transport_down_send>(transport_down_handler);
        initiator_transport->add_typed_message_handler<rpc::stream_transport::transport_down_send>(
            std::move(transport_down_handler));
    }

    struct service_level_route_handshake_expectation
    {
        int add_ref_error_code{rpc::error::OK()};
        route_attestation_status initiator_route_status{route_attestation_status::attested};
        route_attestation_status responder_route_status{route_attestation_status::attested};
        bool initiator_context_established{true};
        bool responder_context_established{true};
        uint64_t initiator_next_transcript_id{2};
        uint64_t responder_next_transcript_id{1};
    };

    enum class claimed_route_control_operation
    {
        add_ref,
        release,
        try_cast,
        object_released,
        transport_down
    };

    CORO_TASK(bool)
    coro_add_ref_drives_service_level_route_handshake(
        const std::shared_ptr<coro::scheduler>& scheduler,
        bool initiator_sends_evidence,
        bool initiator_requires_peer_evidence,
        bool initiator_allows_unattested_peer,
        bool responder_sends_evidence,
        bool responder_requires_peer_evidence,
        bool responder_allows_unattested_peer,
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
            initiator_requires_peer_evidence,
            initiator_allows_unattested_peer));
        responder_service->set_attestation_service(make_test_attestation_service(
            backend,
            "service-level-responder-enclave",
            "service-level-responder-zone",
            responder_sends_evidence,
            responder_requires_peer_evidence,
            responder_allows_unattested_peer));
        initiator_service->set_add_ref_attestation_required(true);
        responder_service->set_add_ref_attestation_required(true);

        CORO_ASSERT_EQ(
            initiator_service->get_attestation_route_state(responder_zone).status, route_attestation_status::unknown);
        CORO_ASSERT_EQ(
            responder_service->get_attestation_route_state(initiator_zone).status, route_attestation_status::unknown);
        CORO_ASSERT_EQ(initiator_service->get_attestation_route_state(responder_zone).next_transcript_id, 1U);
        CORO_ASSERT_EQ(responder_service->get_attestation_route_state(initiator_zone).next_transcript_id, 1U);

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

        protected_rpc_runtime_observer observer;
        install_protected_rpc_runtime_observers(initiator_transport, responder_transport, observer);

        CORO_ASSERT_EQ(initiator_service->spawn(initiator_transport->pump_send_and_receive()), true);
        CORO_ASSERT_EQ(responder_service->spawn(responder_transport->pump_send_and_receive()), true);

        auto remote_object = responder_zone.with_object(rpc::dummy_object_id);
        CORO_ASSERT_EQ(remote_object.has_value(), true);

        rpc::add_ref_params params;
        params.protocol_version = rpc::get_version();
        params.remote_object_id = *remote_object;
        params.caller_zone_id = initiator_zone;
        params.requesting_zone_id = initiator_zone;
        params.build_out_param_channel = rpc::add_ref_options::build_caller_route;

        auto result = CO_AWAIT initiator_service->add_ref(std::move(params));
        CORO_ASSERT_EQ(result.error_code, expected.add_ref_error_code);

        auto initiator_state = initiator_service->get_attestation_route_state(responder_zone);
        auto responder_state = responder_service->get_attestation_route_state(initiator_zone);
        CORO_ASSERT_EQ(initiator_state.status, expected.initiator_route_status);
        CORO_ASSERT_EQ(responder_state.status, expected.responder_route_status);
        CORO_ASSERT_EQ(initiator_state.next_transcript_id, expected.initiator_next_transcript_id);
        CORO_ASSERT_EQ(responder_state.next_transcript_id, expected.responder_next_transcript_id);
        CORO_ASSERT_EQ(
            initiator_state.context && initiator_state.context->established, expected.initiator_context_established);
        CORO_ASSERT_EQ(
            responder_state.context && responder_state.context->established, expected.responder_context_established);
        CORO_ASSERT_EQ(observer.route_handshake_send_count > 0U, true);
        CORO_ASSERT_EQ(observer.route_handshake_response_count > 0U, true);
        CORO_ASSERT_EQ(observer.unexpected_route_handshake_type_visible, false);
        CORO_ASSERT_EQ(observer.non_rpc_public_control_status_visible, false);

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
    coro_reference_control_handles_handshaking_route(
        const std::shared_ptr<coro::scheduler>& scheduler,
        claimed_route_control_operation operation)
    {
        auto local_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto peer_zone = make_service_level_route_zone(service_level_route_responder_subnet);

        auto service = std::make_shared<rpc::enclave_service>("claimed-route-add-ref", local_zone, peer_zone, scheduler);
        service->set_add_ref_attestation_required(true);

        canopy::security::attestation::route_attestation_state handshaking_state;
        handshaking_state.status = route_attestation_status::handshaking;
        handshaking_state.next_transcript_id = 7;
        service->set_attestation_route_state(peer_zone, handshaking_state);

        auto remote_object = peer_zone.with_object(rpc::dummy_object_id);
        CORO_ASSERT_EQ(remote_object.has_value(), true);
        auto local_object = local_zone.with_object(rpc::dummy_object_id);
        CORO_ASSERT_EQ(local_object.has_value(), true);

        auto transport = std::make_shared<recording_reference_control_transport>(local_zone, peer_zone);
        service->add_transport(peer_zone, transport);

        int result_code = rpc::error::OK();
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            switch (operation)
            {
            case claimed_route_control_operation::add_ref:
            {
                rpc::add_ref_params params;
                params.protocol_version = rpc::get_version();
                params.remote_object_id = *remote_object;
                params.caller_zone_id = local_zone;
                params.requesting_zone_id = local_zone;
                params.build_out_param_channel = rpc::add_ref_options::build_caller_route;
                auto result = CO_AWAIT service->add_ref(std::move(params));
                result_code = result.error_code;
                break;
            }
            case claimed_route_control_operation::release:
            {
                rpc::release_params params;
                params.protocol_version = rpc::get_version();
                params.remote_object_id = *local_object;
                params.caller_zone_id = peer_zone;
                params.options = rpc::release_options::normal;
                auto result = CO_AWAIT service->release(std::move(params));
                result_code = result.error_code;
                break;
            }
            case claimed_route_control_operation::try_cast:
            {
                rpc::try_cast_params params;
                params.protocol_version = rpc::get_version();
                params.remote_object_id = *local_object;
                params.caller_zone_id = peer_zone;
                params.interface_id = rpc::interface_ordinal(0x1234);
                auto result = CO_AWAIT service->try_cast(std::move(params));
                result_code = result.error_code;
                break;
            }
            case claimed_route_control_operation::object_released:
            {
                rpc::object_released_params params;
                params.protocol_version = rpc::get_version();
                params.remote_object_id = *remote_object;
                params.caller_zone_id = local_zone;
                CO_AWAIT service->object_released(std::move(params));
                result_code = rpc::error::OK();
                break;
            }
            case claimed_route_control_operation::transport_down:
            {
                rpc::transport_down_params params;
                params.protocol_version = rpc::get_version();
                params.destination_zone_id = local_zone;
                params.caller_zone_id = peer_zone;
                CO_AWAIT service->transport_down(std::move(params));
                result_code = rpc::error::OK();
                break;
            }
            }
            done.store(true);
            CO_RETURN;
        };

        CORO_ASSERT_EQ(service->spawn(task()), true);
        for (size_t i = 0; i < 4; ++i)
            CO_AWAIT scheduler->schedule_after(std::chrono::milliseconds{1});

        if (operation != claimed_route_control_operation::add_ref)
        {
            CORO_ASSERT_EQ(done.load(), true);

            auto route_state = service->get_attestation_route_state(peer_zone);
            CORO_ASSERT_EQ(route_state.status, route_attestation_status::handshaking);
            CORO_ASSERT_EQ(route_state.next_transcript_id, 7U);
            CORO_ASSERT_EQ(route_state.context.has_value(), false);

            switch (operation)
            {
            case claimed_route_control_operation::add_ref:
                break;
            case claimed_route_control_operation::release:
                CORO_ASSERT_EQ(result_code, rpc::error::FRAUDULANT_REQUEST());
                break;
            case claimed_route_control_operation::try_cast:
                CORO_ASSERT_EQ(result_code, rpc::error::FRAUDULANT_REQUEST());
                break;
            case claimed_route_control_operation::object_released:
            case claimed_route_control_operation::transport_down:
                CORO_ASSERT_EQ(result_code, rpc::error::OK());
                break;
            }

            service->remove_transport(peer_zone);
            std::static_pointer_cast<rpc::transport>(transport)->set_status(rpc::transport_status::DISCONNECTED);
            CO_RETURN true;
        }

        CORO_ASSERT_EQ(done.load(), false);
        service->set_route_unattested_allowed(peer_zone, true);

        const auto deadline = std::chrono::steady_clock::now() + service_level_route_test_timeout;
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            CO_AWAIT scheduler->schedule_after(std::chrono::milliseconds{1});

        CORO_ASSERT_EQ(done.load(), true);

        auto route_state = service->get_attestation_route_state(peer_zone);
        CORO_ASSERT_EQ(route_state.status, route_attestation_status::unattested_allowed);
        CORO_ASSERT_EQ(route_state.next_transcript_id, 7U);

        switch (operation)
        {
        case claimed_route_control_operation::add_ref:
            CORO_ASSERT_EQ(route_state.context.has_value(), false);
            CORO_ASSERT_EQ(result_code, rpc::error::OK());
            break;
        case claimed_route_control_operation::release:
        case claimed_route_control_operation::try_cast:
        case claimed_route_control_operation::object_released:
        case claimed_route_control_operation::transport_down:
            break;
        }

        service->remove_transport(peer_zone);
        std::static_pointer_cast<rpc::transport>(transport)->set_status(rpc::transport_status::DISCONNECTED);

        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_concurrent_add_refs_share_service_level_route_handshake(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto initiator_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto responder_zone = make_service_level_route_zone(service_level_route_responder_subnet);

        auto initiator_service = std::make_shared<rpc::enclave_service>(
            "concurrent-route-initiator", initiator_zone, responder_zone, scheduler);
        auto responder_service = std::make_shared<rpc::enclave_service>(
            "concurrent-route-responder", responder_zone, initiator_zone, scheduler);

        auto backend = std::make_shared<fake_backend>();
        initiator_service->set_attestation_service(make_test_attestation_service(
            backend, "concurrent-initiator-enclave", "concurrent-initiator-zone", true, true));
        responder_service->set_attestation_service(make_test_attestation_service(
            backend, "concurrent-responder-enclave", "concurrent-responder-zone", true, true));
        initiator_service->set_add_ref_attestation_required(true);
        responder_service->set_add_ref_attestation_required(true);

        auto send_queue = std::make_shared<streaming::spsc_queue::queue_type>();
        auto receive_queue = std::make_shared<streaming::spsc_queue::queue_type>();
        auto initiator_stream = std::make_shared<streaming::spsc_queue::stream>(send_queue, receive_queue, scheduler);
        auto responder_stream = std::make_shared<streaming::spsc_queue::stream>(receive_queue, send_queue, scheduler);

        rpc::stream_transport::stream_transport_options options{
            .call_timeout = service_level_route_call_timeout,
            .call_timeout_sweep = service_level_route_call_timeout_sweep,
        };
        auto initiator_transport = rpc::stream_transport::make_client(
            "concurrent-route-initiator-transport", initiator_service, std::move(initiator_stream), options);
        auto responder_transport = rpc::stream_transport::make_client(
            "concurrent-route-responder-transport", responder_service, std::move(responder_stream), options);
        initiator_transport->set_adjacent_zone_id(responder_zone);
        responder_transport->set_adjacent_zone_id(initiator_zone);
        initiator_service->add_transport(responder_zone, initiator_transport);
        responder_service->add_transport(initiator_zone, responder_transport);

        // Delay the first route-handshake request before builtin dispatch. This
        // gives the test a deterministic window where the first add_ref owns the
        // handshake and the second add_ref must wait on the same route state
        // instead of starting a duplicate transcript.
        std::atomic_bool delayed_first_handshake{false};
        responder_transport->add_typed_message_handler<rpc::stream_transport::handshake_send>(
            [&scheduler, &delayed_first_handshake](
                auto, const auto&, const auto&, const rpc::stream_transport::handshake_send&)
                -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
            {
                if (!delayed_first_handshake.exchange(true))
                    CO_AWAIT scheduler->schedule_after(std::chrono::milliseconds{5});
                CO_RETURN rpc::stream_transport::transport::message_hook_result::unhandled;
            });

        protected_rpc_runtime_observer observer;
        install_protected_rpc_runtime_observers(initiator_transport, responder_transport, observer);

        CORO_ASSERT_EQ(initiator_service->spawn(initiator_transport->pump_send_and_receive()), true);
        CORO_ASSERT_EQ(responder_service->spawn(responder_transport->pump_send_and_receive()), true);

        auto first_remote_object = responder_zone.with_object(concurrent_route_first_object_id);
        auto second_remote_object = responder_zone.with_object(concurrent_route_second_object_id);
        CORO_ASSERT_EQ(first_remote_object.has_value(), true);
        CORO_ASSERT_EQ(second_remote_object.has_value(), true);

        auto make_add_ref_params = [&](rpc::remote_object remote_object_id) -> rpc::add_ref_params
        {
            rpc::add_ref_params params;
            params.protocol_version = rpc::get_version();
            params.remote_object_id = remote_object_id;
            params.caller_zone_id = initiator_zone;
            params.requesting_zone_id = initiator_zone;
            params.build_out_param_channel = rpc::add_ref_options::build_caller_route;
            return params;
        };

        int first_error_code = rpc::error::TRANSPORT_ERROR();
        std::atomic_bool first_done{false};
        auto first_add_ref_task = [&]() -> coro::task<void>
        {
            auto result = CO_AWAIT initiator_service->add_ref(make_add_ref_params(*first_remote_object));
            first_error_code = result.error_code;
            first_done.store(true);
            CO_RETURN;
        };

        CORO_ASSERT_EQ(initiator_service->spawn(first_add_ref_task()), true);

        const auto handshaking_deadline = std::chrono::steady_clock::now() + service_level_route_test_timeout;
        auto route_state = initiator_service->get_attestation_route_state(responder_zone);
        while (route_state.status != route_attestation_status::handshaking
               && std::chrono::steady_clock::now() < handshaking_deadline)
        {
            CO_AWAIT scheduler->schedule_after(std::chrono::milliseconds{1});
            route_state = initiator_service->get_attestation_route_state(responder_zone);
        }

        CORO_ASSERT_EQ(route_state.status, route_attestation_status::handshaking);
        CORO_ASSERT_EQ(route_state.next_transcript_id, 2U);
        CORO_ASSERT_EQ(route_state.context.has_value(), false);
        CORO_ASSERT_EQ(first_done.load(), false);

        auto second_result = CO_AWAIT initiator_service->add_ref(make_add_ref_params(*second_remote_object));
        CORO_ASSERT_EQ(second_result.error_code, rpc::error::OK());

        const auto first_done_deadline = std::chrono::steady_clock::now() + service_level_route_test_timeout;
        while (!first_done.load() && std::chrono::steady_clock::now() < first_done_deadline)
            CO_AWAIT scheduler->schedule_after(std::chrono::milliseconds{1});

        CORO_ASSERT_EQ(first_done.load(), true);
        CORO_ASSERT_EQ(first_error_code, rpc::error::OK());

        auto initiator_state = initiator_service->get_attestation_route_state(responder_zone);
        auto responder_state = responder_service->get_attestation_route_state(initiator_zone);
        CORO_ASSERT_EQ(initiator_state.status, route_attestation_status::attested);
        CORO_ASSERT_EQ(responder_state.status, route_attestation_status::attested);
        CORO_ASSERT_EQ(initiator_state.next_transcript_id, 2U);
        CORO_ASSERT_EQ(responder_state.next_transcript_id, 1U);
        CORO_ASSERT_EQ(initiator_state.context && initiator_state.context->established, true);
        CORO_ASSERT_EQ(responder_state.context && responder_state.context->established, true);
        CORO_ASSERT_EQ(initiator_state.context->transcript_id, 1U);
        CORO_ASSERT_EQ(responder_state.context->transcript_id, 1U);
        CORO_ASSERT_EQ(observer.route_handshake_send_count, 1U);
        CORO_ASSERT_EQ(observer.route_handshake_response_count, 1U);
        CORO_ASSERT_EQ(observer.unexpected_route_handshake_type_visible, false);
        CORO_ASSERT_EQ(observer.non_rpc_public_control_status_visible, false);
        // The security property under test is that both add_refs completed
        // through one route handshake and one transcript reservation.

        std::static_pointer_cast<rpc::transport>(initiator_transport)->set_status(rpc::transport_status::DISCONNECTED);
        std::static_pointer_cast<rpc::transport>(responder_transport)->set_status(rpc::transport_status::DISCONNECTED);
        for (size_t i = 0; i < service_level_route_cleanup_drain_iterations; ++i)
            CO_AWAIT initiator_service->schedule();

        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_stale_route_handshake_failure_does_not_overwrite_admitted_route(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto local_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto peer_zone = make_service_level_route_zone(service_level_route_responder_subnet);

        auto service
            = std::make_shared<rpc::enclave_service>("stale-route-handshake-initiator", local_zone, peer_zone, scheduler);
        auto backend = std::make_shared<fake_backend>();
        auto attestation_service
            = make_test_attestation_service(backend, "stale-route-local-enclave", "stale-route-local-zone", true, true);
        service->set_attestation_service(attestation_service);
        service->set_add_ref_attestation_required(true);

        auto transport = std::make_shared<gated_handshake_transport>(local_zone, peer_zone, scheduler);
        service->add_transport(peer_zone, transport);

        auto remote_object = peer_zone.with_object(rpc::dummy_object_id);
        CORO_ASSERT_EQ(remote_object.has_value(), true);

        int add_ref_error_code = rpc::error::TRANSPORT_ERROR();
        std::atomic_bool add_ref_done{false};
        auto add_ref_task = [&]() -> coro::task<void>
        {
            rpc::add_ref_params params;
            params.protocol_version = rpc::get_version();
            params.remote_object_id = *remote_object;
            params.caller_zone_id = local_zone;
            params.requesting_zone_id = local_zone;
            params.build_out_param_channel = rpc::add_ref_options::build_caller_route;

            auto result = CO_AWAIT service->add_ref(std::move(params));
            add_ref_error_code = result.error_code;
            add_ref_done.store(true);
            CO_RETURN;
        };

        CORO_ASSERT_EQ(service->spawn(add_ref_task()), true);

        const auto handshaking_deadline = std::chrono::steady_clock::now() + service_level_route_test_timeout;
        auto route_state = service->get_attestation_route_state(peer_zone);
        while ((!transport->handshake_started.load() || route_state.status != route_attestation_status::handshaking)
               && std::chrono::steady_clock::now() < handshaking_deadline)
        {
            CO_AWAIT scheduler->schedule_after(std::chrono::milliseconds{1});
            route_state = service->get_attestation_route_state(peer_zone);
        }

        CORO_ASSERT_EQ(transport->handshake_started.load(), true);
        CORO_ASSERT_EQ(route_state.status, route_attestation_status::handshaking);
        CORO_ASSERT_EQ(route_state.next_transcript_id, 2U);

        auto superseding_context = make_attested_security_context(
            *attestation_service, identity{"superseding-peer-enclave", "superseding-peer-zone"}, 77);
        CORO_ASSERT_EQ(superseding_context.established, true);
        service->set_security_context(peer_zone, superseding_context);

        transport->release_handshake.store(true);
        const auto completion_deadline = std::chrono::steady_clock::now() + service_level_route_test_timeout;
        while (!add_ref_done.load() && std::chrono::steady_clock::now() < completion_deadline)
            CO_AWAIT scheduler->schedule_after(std::chrono::milliseconds{1});

        CORO_ASSERT_EQ(add_ref_done.load(), true);
        CORO_ASSERT_EQ(add_ref_error_code, rpc::error::OK());
        CORO_ASSERT_EQ(transport->handshake_count, 1U);

        route_state = service->get_attestation_route_state(peer_zone);
        CORO_ASSERT_EQ(route_state.status, route_attestation_status::attested);
        CORO_ASSERT_EQ(route_state.next_transcript_id, 2U);
        CORO_ASSERT_EQ(route_state.context.has_value(), true);
        CORO_ASSERT_EQ(route_state.context->established, true);
        CORO_ASSERT_EQ(route_state.context->peer_identity.enclave_id, std::string{"superseding-peer-enclave"});
        CORO_ASSERT_EQ(route_state.context->transcript_id, 77U);

        service->remove_transport(peer_zone);
        std::static_pointer_cast<rpc::transport>(transport)->set_status(rpc::transport_status::DISCONNECTED);
        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_inbound_failed_handshake_does_not_overwrite_admitted_route(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto local_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto peer_zone = make_service_level_route_zone(service_level_route_responder_subnet);

        auto service = std::make_shared<rpc::enclave_service>(
            "inbound-failed-handshake-preserves-route", local_zone, peer_zone, scheduler);
        auto backend = std::make_shared<fake_backend>();
        auto attestation_service = make_test_attestation_service(
            backend, "inbound-failed-local-enclave", "inbound-failed-local-zone", true, true);
        auto established_context = make_attested_security_context(
            *attestation_service, identity{"existing-peer-enclave", "existing-peer-zone"}, 91);
        CORO_ASSERT_EQ(established_context.established, true);
        service->set_security_context(peer_zone, established_context);

        rpc::route_attestation_handshake_request request;
        request.transcript_id = 1;
        request.claimant = rpc::attestation_identity{"rejected-peer-enclave", "rejected-peer-zone"};

        auto response = CO_AWAIT service->handshake(make_route_handshake_params(peer_zone, local_zone, request));
        CORO_ASSERT_EQ(response.error_code, rpc::error::OK());
        CORO_ASSERT_EQ(response.type_id, rpc::id<rpc::route_attestation_handshake_response>::get(rpc::get_version()));

        rpc::route_attestation_handshake_response decoded_response;
        CORO_ASSERT_EQ(
            rpc::deserialise(rpc::encoding::yas_binary, rpc::byte_span(response.payload), decoded_response).empty(), true);
        CORO_ASSERT_EQ(decoded_response.accepted, 0U);

        auto route_state = service->get_attestation_route_state(peer_zone);
        CORO_ASSERT_EQ(route_state.status, route_attestation_status::attested);
        CORO_ASSERT_EQ(route_state.context.has_value(), true);
        CORO_ASSERT_EQ(route_state.context->established, true);
        CORO_ASSERT_EQ(route_state.context->peer_identity.enclave_id, std::string{"existing-peer-enclave"});
        CORO_ASSERT_EQ(route_state.context->transcript_id, 91U);
        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_inbound_no_evidence_handshake_does_not_downgrade_attested_route(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto local_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto peer_zone = make_service_level_route_zone(service_level_route_responder_subnet);

        auto service = std::make_shared<rpc::enclave_service>(
            "inbound-no-evidence-handshake-preserves-route", local_zone, peer_zone, scheduler);
        auto backend = std::make_shared<fake_backend>();
        auto attestation_service = make_test_attestation_service(
            backend, "inbound-no-evidence-local-enclave", "inbound-no-evidence-local-zone", false, false, true);
        service->set_attestation_service(attestation_service);

        auto established_context = make_attested_security_context(
            *attestation_service, identity{"existing-attested-peer-enclave", "existing-attested-peer-zone"}, 92);
        CORO_ASSERT_EQ(established_context.established, true);
        service->set_security_context(peer_zone, established_context);

        rpc::route_attestation_handshake_request request;
        request.transcript_id = 2;
        request.claimant = rpc::attestation_identity{"unattested-peer-enclave", "unattested-peer-zone"};

        auto response = CO_AWAIT service->handshake(make_route_handshake_params(peer_zone, local_zone, request));
        CORO_ASSERT_EQ(response.error_code, rpc::error::OK());

        rpc::route_attestation_handshake_response decoded_response;
        CORO_ASSERT_EQ(
            rpc::deserialise(rpc::encoding::yas_binary, rpc::byte_span(response.payload), decoded_response).empty(), true);
        CORO_ASSERT_EQ(decoded_response.accepted, 1U);

        auto route_state = service->get_attestation_route_state(peer_zone);
        CORO_ASSERT_EQ(route_state.status, route_attestation_status::attested);
        CORO_ASSERT_EQ(route_state.context.has_value(), true);
        CORO_ASSERT_EQ(route_state.context->established, true);
        CORO_ASSERT_EQ(route_state.context->peer_identity.enclave_id, std::string{"existing-attested-peer-enclave"});
        CORO_ASSERT_EQ(route_state.context->transcript_id, 92U);
        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_stream_route_control_messages_remain_public_control(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto initiator_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto responder_zone = make_service_level_route_zone(service_level_route_responder_subnet);

        auto initiator_service = rpc::root_service::create("route-control-initiator", initiator_zone, scheduler);
        auto responder_service = rpc::root_service::create("route-control-responder", responder_zone, scheduler);

        auto send_queue = std::make_shared<streaming::spsc_queue::queue_type>();
        auto receive_queue = std::make_shared<streaming::spsc_queue::queue_type>();
        auto initiator_stream = std::make_shared<streaming::spsc_queue::stream>(send_queue, receive_queue, scheduler);
        auto responder_stream = std::make_shared<streaming::spsc_queue::stream>(receive_queue, send_queue, scheduler);

        rpc::stream_transport::stream_transport_options options{
            .call_timeout = service_level_route_call_timeout,
            .call_timeout_sweep = service_level_route_call_timeout_sweep,
        };
        auto initiator_transport = rpc::stream_transport::make_client(
            "route-control-initiator-transport", initiator_service, std::move(initiator_stream), options);
        auto responder_transport = rpc::stream_transport::make_client(
            "route-control-responder-transport", responder_service, std::move(responder_stream), options);
        initiator_transport->set_adjacent_zone_id(responder_zone);
        responder_transport->set_adjacent_zone_id(initiator_zone);
        initiator_service->add_transport(responder_zone, initiator_transport);
        responder_service->add_transport(initiator_zone, responder_transport);

        protected_rpc_runtime_observer observer;
        install_protected_rpc_runtime_observers(initiator_transport, responder_transport, observer);

        CORO_ASSERT_EQ(initiator_service->spawn(initiator_transport->pump_send_and_receive()), true);
        CORO_ASSERT_EQ(responder_service->spawn(responder_transport->pump_send_and_receive()), true);

        rpc::get_new_zone_id_params get_new_zone_id_params;
        get_new_zone_id_params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT initiator_transport->get_new_zone_id(std::move(get_new_zone_id_params));
        CORO_ASSERT_EQ(zone_result.error_code, rpc::error::OK());
        CORO_ASSERT_EQ(zone_result.zone_id.is_set(), true);
        CORO_ASSERT_EQ(observer.get_new_zone_id_send_count > 0U, true);
        CORO_ASSERT_EQ(observer.get_new_zone_id_response_count > 0U, true);
        CORO_ASSERT_EQ(observer.non_rpc_public_control_status_visible, false);

        rpc::transport_down_params transport_down_params;
        transport_down_params.protocol_version = rpc::get_version();
        transport_down_params.destination_zone_id = responder_zone;
        transport_down_params.caller_zone_id = initiator_zone;
        CO_AWAIT initiator_transport->outbound_transport_down(std::move(transport_down_params));

        for (size_t i = 0; i < service_level_route_cleanup_drain_iterations; ++i)
            CO_AWAIT initiator_service->schedule();

        CORO_ASSERT_EQ(observer.plaintext_transport_down_count > 0U, true);
        CORO_ASSERT_EQ(observer.protected_transport_down_count, 0U);
        CORO_ASSERT_EQ(observer.malformed_encrypted_payload, false);
        CORO_ASSERT_EQ(observer.non_rpc_public_control_status_visible, false);

        std::static_pointer_cast<rpc::transport>(initiator_transport)->set_status(rpc::transport_status::DISCONNECTED);
        std::static_pointer_cast<rpc::transport>(responder_transport)->set_status(rpc::transport_status::DISCONNECTED);
        for (size_t i = 0; i < service_level_route_cleanup_drain_iterations; ++i)
            CO_AWAIT initiator_service->schedule();

        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_get_new_zone_id_sanitises_positive_allocator_status(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto initiator_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto responder_zone = make_service_level_route_zone(service_level_route_responder_subnet);

        auto initiator_service = rpc::root_service::create("route-control-status-initiator", initiator_zone, scheduler);
        auto responder_service = std::make_shared<positive_zone_allocator_service>(
            "route-control-status-responder", responder_zone, scheduler);

        auto send_queue = std::make_shared<streaming::spsc_queue::queue_type>();
        auto receive_queue = std::make_shared<streaming::spsc_queue::queue_type>();
        auto initiator_stream = std::make_shared<streaming::spsc_queue::stream>(send_queue, receive_queue, scheduler);
        auto responder_stream = std::make_shared<streaming::spsc_queue::stream>(receive_queue, send_queue, scheduler);

        rpc::stream_transport::stream_transport_options options{
            .call_timeout = service_level_route_call_timeout,
            .call_timeout_sweep = service_level_route_call_timeout_sweep,
        };
        auto initiator_transport = rpc::stream_transport::make_client(
            "route-control-status-initiator-transport", initiator_service, std::move(initiator_stream), options);
        auto responder_transport = rpc::stream_transport::make_client(
            "route-control-status-responder-transport", responder_service, std::move(responder_stream), options);
        initiator_transport->set_adjacent_zone_id(responder_zone);
        responder_transport->set_adjacent_zone_id(initiator_zone);
        initiator_service->add_transport(responder_zone, initiator_transport);
        responder_service->add_transport(initiator_zone, responder_transport);

        protected_rpc_runtime_observer observer;
        install_protected_rpc_runtime_observers(initiator_transport, responder_transport, observer);

        CORO_ASSERT_EQ(initiator_service->spawn(initiator_transport->pump_send_and_receive()), true);
        CORO_ASSERT_EQ(responder_service->spawn(responder_transport->pump_send_and_receive()), true);

        rpc::get_new_zone_id_params params;
        params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT initiator_transport->get_new_zone_id(std::move(params));
        CORO_ASSERT_EQ(zone_result.error_code, rpc::error::PROTOCOL_ERROR());
        CORO_ASSERT_EQ(zone_result.zone_id.is_set(), false);
        CORO_ASSERT_EQ(observer.get_new_zone_id_send_count > 0U, true);
        CORO_ASSERT_EQ(observer.get_new_zone_id_response_count > 0U, true);
        CORO_ASSERT_EQ(observer.non_rpc_public_control_status_visible, false);

        std::static_pointer_cast<rpc::transport>(initiator_transport)->set_status(rpc::transport_status::DISCONNECTED);
        std::static_pointer_cast<rpc::transport>(responder_transport)->set_status(rpc::transport_status::DISCONNECTED);
        for (size_t i = 0; i < service_level_route_cleanup_drain_iterations; ++i)
            CO_AWAIT initiator_service->schedule();

        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_transport_final_methods_sanitise_positive_control_statuses(const std::shared_ptr<coro::scheduler>&)
    {
        auto initiator_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto responder_zone = make_service_level_route_zone(service_level_route_responder_subnet);
        auto transport = std::make_shared<positive_control_status_transport>(initiator_zone, responder_zone);

        rpc::handshake_params handshake_params;
        handshake_params.protocol_version = rpc::get_version();
        handshake_params.caller_zone_id = initiator_zone;
        handshake_params.destination_zone_id = responder_zone;
        auto handshake_result = CO_AWAIT transport->handshake(std::move(handshake_params));
        CORO_ASSERT_EQ(handshake_result.error_code, rpc::error::PROTOCOL_ERROR());
        CORO_ASSERT_EQ(handshake_result.type_id, 0U);
        CORO_ASSERT_EQ(handshake_result.payload.empty(), true);
        CORO_ASSERT_EQ(handshake_result.out_back_channel.empty(), true);

        rpc::get_new_zone_id_params get_new_zone_id_params;
        get_new_zone_id_params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT transport->get_new_zone_id(std::move(get_new_zone_id_params));
        CORO_ASSERT_EQ(zone_result.error_code, rpc::error::PROTOCOL_ERROR());
        CORO_ASSERT_EQ(zone_result.zone_id.is_set(), false);
        CORO_ASSERT_EQ(zone_result.out_back_channel.empty(), true);

        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_local_parent_transport_sanitises_positive_allocator_status(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto parent_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto child_zone = make_service_level_route_zone(service_level_route_responder_subnet);
        auto parent_service
            = std::make_shared<positive_zone_allocator_service>("local-positive-zone-allocator", parent_zone, scheduler);
        auto parent_side_transport = std::make_shared<rpc::local::child_transport>("local-parent-side", parent_service);
        parent_side_transport->set_adjacent_zone_id(child_zone);
        auto child_side_transport
            = std::make_shared<rpc::local::parent_transport>("local-child-side", parent_side_transport);

        rpc::get_new_zone_id_params params;
        params.protocol_version = rpc::get_version();
        auto zone_result = CO_AWAIT child_side_transport->get_new_zone_id(std::move(params));
        CORO_ASSERT_EQ(zone_result.error_code, rpc::error::PROTOCOL_ERROR());
        CORO_ASSERT_EQ(zone_result.zone_id.is_set(), false);
        CORO_ASSERT_EQ(zone_result.out_back_channel.empty(), true);

        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_enclave_local_reference_controls_use_referenced_route(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto local_zone = make_service_level_route_zone(enclave_local_subject_zone_subnet);
        auto adjacent_zone = make_service_level_route_zone(enclave_local_adjacent_zone_subnet);
        auto referenced_zone = make_service_level_route_zone(enclave_local_referenced_zone_subnet);

        auto service = std::make_shared<rpc::enclave_service>(
            "enclave-local-reference-control", local_zone, adjacent_zone, scheduler);
        service->set_add_ref_attestation_required(true);
        service->set_route_unattested_allowed(referenced_zone, true);

        canopy::security::attestation::route_attestation_state failed_adjacent_state;
        failed_adjacent_state.status = route_attestation_status::failed;
        failed_adjacent_state.failure_reason = "adjacent local peer is only a next hop";
        service->set_attestation_route_state(adjacent_zone, std::move(failed_adjacent_state));

        auto referenced_object = referenced_zone.with_object(rpc::dummy_object_id);
        CORO_ASSERT_EQ(referenced_object.has_value(), true);

        rpc::add_ref_params add_ref_params;
        add_ref_params.protocol_version = rpc::get_version();
        add_ref_params.remote_object_id = *referenced_object;
        add_ref_params.caller_zone_id = local_zone;
        add_ref_params.requesting_zone_id = adjacent_zone;
        add_ref_params.build_out_param_channel = rpc::add_ref_options::normal;

        auto generic_transport = std::make_shared<recording_reference_control_transport>(local_zone, adjacent_zone);
        auto generic_add_ref_result = CO_AWAIT service->outbound_add_ref(add_ref_params, generic_transport);
        CORO_ASSERT_EQ(generic_add_ref_result.error_code, rpc::error::OK());
        CORO_ASSERT_EQ(generic_transport->add_ref_count, 1U);

        auto enclave_local_transport
            = std::make_shared<recording_enclave_local_reference_control_transport>(local_zone, adjacent_zone);
        auto local_add_ref_result = CO_AWAIT service->outbound_add_ref(add_ref_params, enclave_local_transport);
        CORO_ASSERT_EQ(local_add_ref_result.error_code, rpc::error::OK());
        CORO_ASSERT_EQ(enclave_local_transport->add_ref_count, 1U);
        CORO_ASSERT_EQ(enclave_local_transport->last_add_ref_remote_object, *referenced_object);

        rpc::release_params release_params;
        release_params.protocol_version = rpc::get_version();
        release_params.remote_object_id = *referenced_object;
        release_params.caller_zone_id = local_zone;
        release_params.options = rpc::release_options::normal;

        auto generic_release_result = CO_AWAIT service->outbound_release(release_params, generic_transport);
        CORO_ASSERT_EQ(generic_release_result.error_code, rpc::error::OK());
        CORO_ASSERT_EQ(generic_transport->release_count, 1U);

        auto local_release_result = CO_AWAIT service->outbound_release(release_params, enclave_local_transport);
        CORO_ASSERT_EQ(local_release_result.error_code, rpc::error::OK());
        CORO_ASSERT_EQ(enclave_local_transport->release_count, 1U);
        CORO_ASSERT_EQ(enclave_local_transport->last_release_remote_object, *referenced_object);

        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_enclave_local_metadata_controls_use_endpoint_routes(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto local_zone = make_service_level_route_zone(enclave_local_subject_zone_subnet);
        auto adjacent_zone = make_service_level_route_zone(enclave_local_adjacent_zone_subnet);
        auto referenced_zone = make_service_level_route_zone(enclave_local_referenced_zone_subnet);

        auto service = std::make_shared<rpc::enclave_service>(
            "enclave-local-metadata-control", local_zone, adjacent_zone, scheduler);

        auto backend = std::make_shared<fake_backend>();
        auto local_identity = identity{"enclave-local-metadata-local", "enclave-local-metadata-local-zone"};
        auto referenced_identity = identity{"enclave-local-metadata-referenced", "enclave-local-metadata-referenced-zone"};
        auto attestation_service
            = make_test_attestation_service(backend, local_identity.enclave_id, local_identity.zone_id, true, true);
        auto context = make_attested_security_context(
            *attestation_service, referenced_identity, protected_rpc_generated_runtime_transcript_id + 2);
        CORO_ASSERT_EQ(context.established, true);

        service->set_attestation_service(attestation_service);
        service->set_security_context(referenced_zone, std::move(context));
        service->set_protected_rpc_enabled(true);
        service->set_add_ref_attestation_required(true);

        canopy::security::attestation::route_attestation_state failed_adjacent_state;
        failed_adjacent_state.status = route_attestation_status::failed;
        failed_adjacent_state.failure_reason = "adjacent local peer is only a next hop";
        service->set_attestation_route_state(adjacent_zone, std::move(failed_adjacent_state));

        auto referenced_object = referenced_zone.with_object(rpc::dummy_object_id);
        CORO_ASSERT_EQ(referenced_object.has_value(), true);

        auto enclave_local_transport
            = std::make_shared<recording_enclave_local_reference_control_transport>(local_zone, adjacent_zone);

        rpc::try_cast_params try_cast_params;
        try_cast_params.protocol_version = rpc::get_version();
        try_cast_params.remote_object_id = *referenced_object;
        try_cast_params.caller_zone_id = local_zone;
        try_cast_params.interface_id = rpc::interface_ordinal(0x1234);

        auto try_cast_result = CO_AWAIT service->outbound_try_cast(try_cast_params, enclave_local_transport);
        CORO_ASSERT_EQ(try_cast_result.error_code, rpc::error::OK());
        CORO_ASSERT_EQ(enclave_local_transport->try_cast_count, 1U);
        CORO_ASSERT_EQ(enclave_local_transport->try_cast_was_protected, true);
        CORO_ASSERT_EQ(enclave_local_transport->last_try_cast_remote_object.as_zone(), referenced_zone);
        CORO_ASSERT_EQ(enclave_local_transport->last_try_cast_remote_object.get_object_id().get_val(), 0U);

        auto local_object = local_zone.with_object(rpc::dummy_object_id);
        CORO_ASSERT_EQ(local_object.has_value(), true);

        rpc::object_released_params object_released_params;
        object_released_params.protocol_version = rpc::get_version();
        object_released_params.remote_object_id = *local_object;
        object_released_params.caller_zone_id = referenced_zone;

        enclave_local_transport->increment_inbound_stub_count(referenced_zone);
        CO_AWAIT service->outbound_object_released(object_released_params, enclave_local_transport);
        CORO_ASSERT_EQ(enclave_local_transport->object_released_count, 1U);
        CORO_ASSERT_EQ(enclave_local_transport->object_released_was_protected, true);
        CORO_ASSERT_EQ(enclave_local_transport->last_object_released_remote_object.as_zone(), local_zone);
        CORO_ASSERT_EQ(enclave_local_transport->last_object_released_remote_object.get_object_id().get_val(), 0U);
        CORO_ASSERT_EQ(enclave_local_transport->last_object_released_caller_zone, referenced_zone);

        rpc::transport_down_params transport_down_params;
        transport_down_params.protocol_version = rpc::get_version();
        transport_down_params.destination_zone_id = referenced_zone;
        transport_down_params.caller_zone_id = local_zone;

        CO_AWAIT enclave_local_transport->transport_down(std::move(transport_down_params));
        CORO_ASSERT_EQ(enclave_local_transport->transport_down_count, 1U);
        CORO_ASSERT_EQ(enclave_local_transport->transport_down_was_protected, false);
        CORO_ASSERT_EQ(enclave_local_transport->last_transport_down_destination_zone, referenced_zone);
        CORO_ASSERT_EQ(enclave_local_transport->last_transport_down_caller_zone, local_zone);

        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_enclave_local_transport_wrappers_are_marked(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto parent_zone = make_service_level_route_zone(enclave_local_subject_zone_subnet);
        auto child_zone = make_service_level_route_zone(enclave_local_adjacent_zone_subnet);
        auto parent_service = rpc::root_service::create("enclave-local-wrapper-parent", parent_zone, scheduler);
        bool child_parent_transport_marked = false;
        bool child_service_is_enclave_service = false;
        auto child_transport = std::make_shared<rpc::sgx::coro::enclave::local_child_transport>(
            "enclave-local-child-transport", parent_service);
        child_transport->set_adjacent_zone_id(child_zone);
        child_transport->template set_child_entry_point<yyy::i_host, yyy::i_example>(
            [&child_parent_transport_marked, &child_service_is_enclave_service](
                const rpc::shared_ptr<yyy::i_host>& host, const std::shared_ptr<rpc::child_service>& child_service_ptr)
                -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
            {
                child_parent_transport_marked
                    = rpc::sgx::coro::enclave::is_local_route_transport(child_service_ptr->get_parent_transport());
                child_service_is_enclave_service
                    = static_cast<bool>(std::dynamic_pointer_cast<rpc::enclave_service>(child_service_ptr));
                CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(),
                    rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(child_service_ptr, host))};
            });
        auto parent_transport = std::make_shared<rpc::sgx::coro::enclave::local_parent_transport>(
            "enclave-local-parent-transport", child_transport);

        CORO_ASSERT_EQ(rpc::sgx::coro::enclave::is_local_route_transport(child_transport), true);
        CORO_ASSERT_EQ(rpc::sgx::coro::enclave::is_local_route_transport(parent_transport), true);
        CORO_ASSERT_EQ(child_transport->get_adjacent_zone_id(), child_zone);
        CORO_ASSERT_EQ(parent_transport->get_adjacent_zone_id(), parent_zone);

        rpc::shared_ptr<yyy::i_host> host_ptr(new host());
        auto connect_result = CO_AWAIT parent_service->connect_to_zone<yyy::i_host, yyy::i_example>(
            "enclave-local-wrapper-child", child_transport, host_ptr);
        CORO_ASSERT_EQ(connect_result.error_code, rpc::error::OK());
        CORO_ASSERT_NE(connect_result.output_interface, nullptr);
        CORO_ASSERT_EQ(child_parent_transport_marked, true);
        CORO_ASSERT_EQ(child_service_is_enclave_service, true);

        int add_result = 0;
        CORO_ASSERT_EQ(
            CO_AWAIT connect_result.output_interface->add(
                arithmetic_test_left_value, arithmetic_test_right_value, add_result),
            rpc::error::OK());
        CORO_ASSERT_EQ(add_result, arithmetic_test_expected_result);

        rpc::shared_ptr<xxx::i_foo> foo;
        CORO_ASSERT_EQ(CO_AWAIT connect_result.output_interface->create_foo(foo), rpc::error::OK());
        CORO_ASSERT_NE(foo, nullptr);
        CORO_ASSERT_EQ(CO_AWAIT foo->clear_recorded_messages(), rpc::error::OK());
        CORO_ASSERT_EQ(CO_AWAIT foo->record_message(enclave_local_post_message_value), rpc::error::OK());
        CO_AWAIT parent_service->schedule();
        std::vector<int> recorded_messages;
        CORO_ASSERT_EQ(CO_AWAIT foo->get_recorded_messages(recorded_messages), rpc::error::OK());
        CORO_ASSERT_EQ(recorded_messages.size(), 1U);
        CORO_ASSERT_EQ(recorded_messages[0], enclave_local_post_message_value);

        foo = nullptr;
        connect_result.output_interface = nullptr;
        host_ptr = nullptr;
        for (size_t i = 0; i < service_level_route_cleanup_drain_iterations; ++i)
            CO_AWAIT parent_service->schedule();

        std::static_pointer_cast<rpc::transport>(parent_transport)->set_status(rpc::transport_status::DISCONNECTED);
        std::static_pointer_cast<rpc::transport>(child_transport)->set_status(rpc::transport_status::DISCONNECTED);
        for (size_t i = 0; i < service_level_route_cleanup_drain_iterations; ++i)
            CO_AWAIT parent_service->schedule();

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

        auto bar = CO_AWAIT rpc::dynamic_pointer_cast<xxx::i_bar>(foo);
        CORO_ASSERT_EQ(bar, nullptr);

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
        CORO_ASSERT_EQ(observer.plaintext_try_cast_count, 0U);
        CORO_ASSERT_EQ(observer.plaintext_add_ref_count, 0U);
        CORO_ASSERT_EQ(observer.plaintext_release_count, 0U);
        CORO_ASSERT_EQ(observer.malformed_encrypted_payload, false);
        CORO_ASSERT_EQ(observer.protected_object_id_visible, false);
        CORO_ASSERT_EQ(observer.non_rpc_public_control_status_visible, false);
        CORO_ASSERT_EQ(observer.unexpected_route_handshake_type_visible, false);
        CORO_ASSERT_EQ(observer.stream_init_send_count > 0U, true);
        CORO_ASSERT_EQ(observer.stream_init_initial_response_count > 0U, true);
        CORO_ASSERT_EQ(observer.stream_init_response_count > 0U, true);
        CORO_ASSERT_EQ(observer.protected_send_count >= protected_rpc_generated_runtime_min_send_count, true);
        CORO_ASSERT_EQ(observer.protected_send_count, observer.protected_send_response_count);
        CORO_ASSERT_EQ(observer.protected_post_count, protected_rpc_generated_runtime_post_count);
        CORO_ASSERT_EQ(observer.protected_try_cast_count > 0U, true);
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

        CORO_ASSERT_EQ(observer.plaintext_release_count, 0U);
        CORO_ASSERT_EQ(observer.protected_release_count > 0U, true);
        CORO_ASSERT_EQ(observer.malformed_encrypted_payload, false);
        CORO_ASSERT_EQ(observer.protected_object_id_visible, false);
        CORO_ASSERT_EQ(observer.non_rpc_public_control_status_visible, false);
        CORO_ASSERT_EQ(observer.unexpected_route_handshake_type_visible, false);
        CORO_ASSERT_EQ(observer.close_connection_send_count > 0U, true);

        CO_RETURN true;
    }

    CORO_TASK(bool)
    coro_enclave_service_sanitises_positive_control_statuses(const std::shared_ptr<coro::scheduler>& scheduler)
    {
        auto initiator_zone = make_service_level_route_zone(service_level_route_initiator_subnet);
        auto responder_zone = make_service_level_route_zone(service_level_route_responder_subnet);

        auto initiator_service = std::make_shared<rpc::enclave_service>(
            "protected-rpc-control-status-initiator", initiator_zone, responder_zone, scheduler);

        auto backend = std::make_shared<fake_backend>();
        auto initiator_identity = identity{"protected-control-initiator-enclave", "protected-control-initiator-zone"};
        auto responder_identity = identity{"protected-control-responder-enclave", "protected-control-responder-zone"};
        auto initiator_attestation_service = make_test_attestation_service(
            backend, initiator_identity.enclave_id, initiator_identity.zone_id, true, true);
        auto initiator_context = make_attested_security_context(
            *initiator_attestation_service, responder_identity, protected_rpc_generated_runtime_transcript_id + 1);
        CORO_ASSERT_EQ(initiator_context.established, true);

        initiator_service->set_attestation_service(initiator_attestation_service);
        initiator_service->set_security_context(responder_zone, std::move(initiator_context));
        initiator_service->set_protected_rpc_enabled(true);

        auto transport = std::make_shared<positive_control_status_transport>(initiator_zone, responder_zone);
        auto remote_object = responder_zone.with_object(rpc::dummy_object_id);
        CORO_ASSERT_EQ(remote_object.has_value(), true);

        rpc::try_cast_params try_cast_params;
        try_cast_params.protocol_version = rpc::get_version();
        try_cast_params.caller_zone_id = initiator_zone;
        try_cast_params.remote_object_id = *remote_object;
        try_cast_params.interface_id = rpc::interface_ordinal(0x1234);

        auto try_cast_result = CO_AWAIT initiator_service->outbound_try_cast(try_cast_params, transport);
        CORO_ASSERT_EQ(try_cast_result.error_code, rpc::error::PROTOCOL_ERROR());
        CORO_ASSERT_EQ(transport->try_cast_count, 1U);
        CORO_ASSERT_EQ(transport->try_cast_was_protected, true);

        rpc::add_ref_params add_ref_params;
        add_ref_params.protocol_version = rpc::get_version();
        add_ref_params.remote_object_id = *remote_object;
        add_ref_params.caller_zone_id = initiator_zone;
        add_ref_params.requesting_zone_id = initiator_zone;
        add_ref_params.build_out_param_channel = rpc::add_ref_options::normal;

        auto add_ref_result = CO_AWAIT initiator_service->outbound_add_ref(add_ref_params, transport);
        CORO_ASSERT_EQ(add_ref_result.error_code, rpc::error::PROTOCOL_ERROR());
        CORO_ASSERT_EQ(transport->add_ref_count, 1U);
        CORO_ASSERT_EQ(transport->add_ref_was_protected, true);

        rpc::release_params release_params;
        release_params.protocol_version = rpc::get_version();
        release_params.remote_object_id = *remote_object;
        release_params.caller_zone_id = initiator_zone;
        release_params.options = rpc::release_options::normal;

        auto release_result = CO_AWAIT initiator_service->outbound_release(release_params, transport);
        CORO_ASSERT_EQ(release_result.error_code, rpc::error::PROTOCOL_ERROR());
        CORO_ASSERT_EQ(transport->release_count, 1U);
        CORO_ASSERT_EQ(transport->release_was_protected, true);

        CO_RETURN true;
    }

    void run_service_level_route_handshake_test(
        bool initiator_sends_evidence,
        bool initiator_requires_peer_evidence,
        bool initiator_allows_unattested_peer,
        bool responder_sends_evidence,
        bool responder_requires_peer_evidence,
        bool responder_allows_unattested_peer,
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
                initiator_allows_unattested_peer,
                responder_sends_evidence,
                responder_requires_peer_evidence,
                responder_allows_unattested_peer,
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

    void run_stream_route_control_message_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_stream_route_control_messages_remain_public_control(scheduler);
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

    void run_claimed_route_control_admission_test(claimed_route_control_operation operation)
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_reference_control_handles_handshaking_route(scheduler, operation);
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

    void run_concurrent_add_ref_route_handshake_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_concurrent_add_refs_share_service_level_route_handshake(scheduler);
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

    void run_stale_route_handshake_completion_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_stale_route_handshake_failure_does_not_overwrite_admitted_route(scheduler);
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

    void run_inbound_failed_handshake_preserves_admitted_route_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_inbound_failed_handshake_does_not_overwrite_admitted_route(scheduler);
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

    void run_inbound_no_evidence_handshake_preserves_attested_route_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_inbound_no_evidence_handshake_does_not_downgrade_attested_route(scheduler);
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

    void run_transport_final_control_status_sanitiser_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_transport_final_methods_sanitise_positive_control_statuses(scheduler);
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

    void run_local_transport_control_status_sanitiser_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_local_parent_transport_sanitises_positive_allocator_status(scheduler);
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

    void run_enclave_local_reference_route_subject_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_enclave_local_reference_controls_use_referenced_route(scheduler);
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

    void run_enclave_local_metadata_route_subject_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_enclave_local_metadata_controls_use_endpoint_routes(scheduler);
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

    void run_enclave_local_transport_wrapper_marker_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_enclave_local_transport_wrappers_are_marked(scheduler);
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

    void run_get_new_zone_id_status_sanitiser_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_get_new_zone_id_sanitises_positive_allocator_status(scheduler);
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

    void run_enclave_service_control_status_sanitiser_test()
    {
        auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));

        bool result = false;
        std::atomic_bool done{false};
        auto task = [&]() -> coro::task<void>
        {
            result = CO_AWAIT coro_enclave_service_sanitises_positive_control_statuses(scheduler);
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
    CORO_ASSERT_EQ(initiator_context.peer_attested, lib.responder_sends_evidence());
    CORO_ASSERT_EQ(responder_context.peer_attested, lib.initiator_sends_evidence());
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
        false,
        true,
        true,
        false,
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
        false,
        true,
        false,
        true,
        service_level_route_handshake_expectation{.initiator_route_status = route_attestation_status::attested,
            .responder_route_status = route_attestation_status::unattested_allowed,
            .initiator_context_established = true,
            .responder_context_established = false});
}

TEST(
    ServiceLevelRouteAttestation,
    AddRefRejectsUnattestedClientWithoutExplicitPolicy)
{
    run_service_level_route_handshake_test(
        false,
        true,
        false,
        true,
        false,
        false,
        service_level_route_handshake_expectation{.add_ref_error_code = rpc::error::ZONE_NOT_SUPPORTED(),
            .initiator_route_status = route_attestation_status::failed,
            .responder_route_status = route_attestation_status::failed,
            .initiator_context_established = false,
            .responder_context_established = false});
}

TEST(
    ServiceLevelRouteAttestation,
    AddRefWaitsWhenRouteHandshakeIsAlreadyClaimed)
{
    run_claimed_route_control_admission_test(claimed_route_control_operation::add_ref);
}

TEST(
    ServiceLevelRouteAttestation,
    ConcurrentAddRefsShareOneRouteHandshake)
{
    run_concurrent_add_ref_route_handshake_test();
}

TEST(
    ServiceLevelRouteAttestation,
    StaleRouteHandshakeFailureDoesNotOverwriteAdmittedRoute)
{
    run_stale_route_handshake_completion_test();
}

TEST(
    ServiceLevelRouteAttestation,
    InboundFailedHandshakeDoesNotOverwriteAdmittedRoute)
{
    run_inbound_failed_handshake_preserves_admitted_route_test();
}

TEST(
    ServiceLevelRouteAttestation,
    InboundNoEvidenceHandshakeDoesNotDowngradeAttestedRoute)
{
    run_inbound_no_evidence_handshake_preserves_attested_route_test();
}

TEST(
    ServiceLevelRouteAttestation,
    ReleaseRejectsBeforeRouteAdmission)
{
    run_claimed_route_control_admission_test(claimed_route_control_operation::release);
}

TEST(
    ServiceLevelRouteAttestation,
    TryCastRejectsBeforeRouteAdmission)
{
    run_claimed_route_control_admission_test(claimed_route_control_operation::try_cast);
}

TEST(
    ServiceLevelRouteAttestation,
    ObjectReleasedReturnsBeforeRouteAdmission)
{
    run_claimed_route_control_admission_test(claimed_route_control_operation::object_released);
}

TEST(
    ServiceLevelRouteAttestation,
    TransportDownReturnsBeforeRouteAdmission)
{
    run_claimed_route_control_admission_test(claimed_route_control_operation::transport_down);
}

TEST(
    StreamRouteControl,
    GetNewZoneIdAndRouteTransportDownUsePublicControlOnly)
{
    run_stream_route_control_message_test();
}

TEST(
    StreamRouteControl,
    GetNewZoneIdSanitisesPositiveAllocatorStatus)
{
    run_get_new_zone_id_status_sanitiser_test();
}

TEST(
    TransportRouteControl,
    FinalMethodsSanitisePositiveControlStatuses)
{
    run_transport_final_control_status_sanitiser_test();
}

TEST(
    LocalRouteControl,
    ParentTransportSanitisesPositiveAllocatorStatus)
{
    run_local_transport_control_status_sanitiser_test();
}

TEST(
    EnclaveLocalRouteControl,
    TransportWrappersAreMarkedAsLocalRoutes)
{
    run_enclave_local_transport_wrapper_marker_test();
}

TEST(
    EnclaveLocalRouteControl,
    ReferenceControlUsesReferencedRouteNotAdjacentLocalPeer)
{
    run_enclave_local_reference_route_subject_test();
}

TEST(
    EnclaveLocalRouteControl,
    MetadataControlUsesEndpointRoutesNotAdjacentLocalPeer)
{
    run_enclave_local_metadata_route_subject_test();
}

TEST(
    ProtectedRpcRuntime,
    GeneratedRpcSendAndPostUseEnclaveServiceProtection)
{
    run_generated_rpc_protected_send_post_test();
}

TEST(
    ProtectedRpcRuntime,
    EnclaveServiceSanitisesPositiveControlStatuses)
{
    run_enclave_service_control_status_sanitiser_test();
}
#  endif
#endif
