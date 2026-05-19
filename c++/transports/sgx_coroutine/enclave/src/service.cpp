/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/enclave/service.h>

#include <attestation/route_attestation_protocol.h>
#include <security/attestation/context_source.h>
#include <security/attestation/protected_rpc.h>
#include <streaming/stream.h>
#include <transports/sgx_coroutine/enclave/local_route_transport.h>

#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace rpc
{
    namespace
    {
        // This file is the enclave-side security interposer for coroutine SGX
        // transports. The base child_service still performs ordinary RPC
        // routing, proxy/stub dispatch, and reference counting; this derived
        // service adds the attestation decisions before traffic reaches those
        // base paths.
        //
        // Security model:
        // - Adjacent transports are next-hop links only. They are not treated as
        //   proof that the referenced object or caller lives in that same zone.
        // - Route attestation is keyed by the route zone that owns or calls the
        //   referenced object. That distinction matters when an intermediate
        //   forwards an interface from a third zone.
        // - Protected RPC payloads hide application data and object/interface
        //   details from intermediates. Public backchannels remain mutable route
        //   context and are intentionally not sender-authenticated here.
        // - Positive application send error codes are hidden inside encrypted
        //   send responses. Public control-message results are sanitised before
        //   being returned across an untrusted route.
        constexpr uint64_t route_attestation_session_epoch = 1;
        constexpr uint8_t route_attestation_flag_false = 0;
        constexpr uint8_t route_attestation_flag_true = 1;
        constexpr size_t max_route_attestation_handshake_payload_size = 1024 * 1024;
        constexpr std::chrono::milliseconds route_attestation_wait_poll_interval{1};
        constexpr std::chrono::milliseconds route_attestation_wait_timeout{2000};

        // Returns the IDL fingerprint for the route-attestation request schema
        // used by this RPC protocol version.
        [[nodiscard]] auto route_attestation_request_type_id(uint64_t protocol_version) -> uint64_t
        {
            return rpc::id<rpc::route_attestation_handshake_request>::get(protocol_version);
        }

        // Returns the IDL fingerprint for the matching route-attestation
        // response schema.
        [[nodiscard]] auto route_attestation_response_type_id(uint64_t protocol_version) -> uint64_t
        {
            return rpc::id<rpc::route_attestation_handshake_response>::get(protocol_version);
        }

        // Converts a zone id to a stable fallback identity string for test/fake
        // backends that do not provide a richer enclave identity.
        [[nodiscard]] auto route_identity_string(rpc::zone zone_id) -> std::string
        {
            return std::to_string(zone_id);
        }

        // Copies the in-memory security-library identity into the generated IDL
        // carrier used on the RPC wire.
        [[nodiscard]] auto to_wire_identity(const canopy::security::attestation::identity& value) -> rpc::attestation_identity
        {
            // Keep the IDL boundary explicit. The security library deliberately
            // owns the in-memory attestation types; the RPC wire protocol owns
            // the generated IDL carrier types and fingerprints.
            rpc::attestation_identity out;
            out.enclave_id = value.enclave_id;
            out.zone_id = value.zone_id;
            return out;
        }

        // Copies a generated IDL identity back into the in-memory security
        // library representation.
        [[nodiscard]] auto from_wire_identity(const rpc::attestation_identity& value)
            -> canopy::security::attestation::identity
        {
            canopy::security::attestation::identity out;
            out.enclave_id = value.enclave_id;
            out.zone_id = value.zone_id;
            return out;
        }

        // Copies an attestation CMW blob into the generated RPC carrier without
        // interpreting backend-specific evidence bytes.
        [[nodiscard]] auto to_wire_cmw(const canopy::security::attestation::cmw& value) -> rpc::attestation_cmw
        {
            rpc::attestation_cmw out;
            out.media_type = value.media_type;
            out.content_format = value.content_format;
            out.payload = value.payload;
            return out;
        }

        // Copies a generated RPC CMW carrier back into the security library
        // representation, still leaving backend evidence opaque at this layer.
        [[nodiscard]] auto from_wire_cmw(const rpc::attestation_cmw& value) -> canopy::security::attestation::cmw
        {
            canopy::security::attestation::cmw out;
            out.media_type = value.media_type;
            out.content_format = value.content_format;
            out.payload = value.payload;
            return out;
        }

        // Stores security_level as a protocol integer in the generated IDL
        // schema.
        [[nodiscard]] auto wire_security_level(canopy::security::attestation::security_level level) -> uint64_t
        {
            return static_cast<uint64_t>(level);
        }

        // Parses a wire security_level defensively; unknown future values are
        // treated as no verified hardware/software assurance.
        [[nodiscard]] auto from_wire_security_level(uint64_t level) -> canopy::security::attestation::security_level
        {
            using canopy::security::attestation::security_level;
            if (level > static_cast<uint64_t>(security_level::hardware))
                return security_level::none;
            return static_cast<security_level>(level);
        }

        // Selects the local identity that should be bound into a route
        // transcript for this service zone.
        [[nodiscard]] auto local_identity_for_route(
            const std::shared_ptr<canopy::security::attestation::attestation_service>& service,
            rpc::zone zone_id) -> canopy::security::attestation::identity
        {
            // A real backend should provide a stable identity. Fake/null
            // development backends may not, so fall back to the route zone string
            // rather than allowing an empty identity into transcripts.
            canopy::security::attestation::identity identity;
            if (service)
                identity = service->local_identity();
            if (identity.zone_id.empty())
                identity.zone_id = route_identity_string(zone_id);
            return identity;
        }

        // Serialises a typed route-handshake IDL payload in the caller-selected
        // encoding and reports serializer failures as invalid protocol data.
        template<class Payload>
        [[nodiscard]] auto serialise_route_attestation_payload(
            const Payload& payload,
            rpc::encoding payload_encoding) -> std::optional<std::vector<char>>
        {
            try
            {
                return rpc::serialise<std::vector<char>>(payload, payload_encoding);
            }
            catch (const std::exception& ex)
            {
                RPC_WARNING("route attestation payload serialisation failed: {}", ex.what());
            }
            catch (...)
            {
                RPC_WARNING("route attestation payload serialisation failed");
            }
            return std::nullopt;
        }

        // Parses a typed route-handshake payload after applying a fixed size
        // bound to unauthenticated input.
        template<class Payload>
        [[nodiscard]] auto parse_route_attestation_payload(
            const std::vector<char>& payload,
            rpc::encoding payload_encoding,
            Payload& out) -> bool
        {
            // Route handshakes are unauthenticated until the evidence is verified,
            // so bound the decoded payload before handing it to a serializer.
            if (payload.size() > max_route_attestation_handshake_payload_size)
                return false;
            return rpc::deserialise(payload_encoding, rpc::byte_span(payload), out).empty();
        }

        // Checks that a peer-supplied nonce has the exact width required by the
        // attestation backend binding code.
        [[nodiscard]] auto nonce_is_valid(const std::vector<uint8_t>& nonce) -> bool
        {
            return nonce.size() == canopy::security::attestation::attestation_nonce_size;
        }

        // Route-handshake booleans are encoded as explicit protocol bytes so
        // malformed values can be rejected before policy is evaluated.
        [[nodiscard]] auto flag_is_valid(uint8_t value) -> bool
        {
            return value == route_attestation_flag_false || value == route_attestation_flag_true;
        }

        // Builds a typed negative response for malformed or policy-rejected
        // handshakes while keeping the response schema stable.
        [[nodiscard]] auto make_failed_handshake_response(
            uint64_t protocol_version,
            uint64_t transcript_id,
            std::string reason,
            const canopy::security::attestation::identity& responder) -> rpc::route_attestation_handshake_response
        {
            rpc::route_attestation_handshake_response response;
            response.transcript_id = transcript_id;
            response.accepted = route_attestation_flag_false;
            response.reason = std::move(reason);
            response.responder = to_wire_identity(responder);
            return response;
        }

        // Serialises a handshake response into the generic marshaller
        // handshake_result wrapper.
        [[nodiscard]] auto make_route_attestation_handshake_result(
            uint64_t protocol_version,
            rpc::encoding payload_encoding,
            rpc::route_attestation_handshake_response response) -> rpc::handshake_result
        {
            auto payload = serialise_route_attestation_payload(response, payload_encoding);
            if (!payload.has_value())
                return rpc::handshake_result{rpc::error::INVALID_DATA(), 0, {}, {}};
            return rpc::handshake_result{
                rpc::error::OK(), route_attestation_response_type_id(protocol_version), std::move(payload.value()), {}};
        }

        // Removes application-private success/failure codes from public control
        // responses that may be visible to route intermediates.
        [[nodiscard]] auto sanitise_public_control_result(
            rpc::standard_result result,
            const char* operation) -> rpc::standard_result
        {
            // Control-message results may cross intermediates. Only the standard
            // negative RPC transport/status codes are safe to expose there.
            // Application-level positive codes belong inside protected send
            // responses and must not leak through add_ref/release/try_cast paths.
            const auto error_code = rpc::error::sanitise_public_control_status(result.error_code, operation);
            if (error_code == result.error_code)
                return result;

            result.error_code = error_code;
            result.out_back_channel.clear();
            return result;
        }

        // Chooses the route whose reference-control policy applies to an
        // outbound add_ref/release for a given transport.
        [[nodiscard]] auto outbound_reference_route_zone(
            const rpc::remote_object& remote_object_id,
            const std::shared_ptr<rpc::transport>& transport) -> std::optional<rpc::destination_zone>
        {
            if (!transport)
                return std::nullopt;

            // Enclave-local transports are only next-hop links between zones in
            // the same enclave. Reference-control security is still about the
            // referenced owner route, not the adjacent local peer.
            if (rpc::sgx::coro::enclave::is_local_route_transport(transport))
                return remote_object_id.as_zone();

            return transport->get_adjacent_zone_id();
        }

        // A route-handshake claim owns exactly one in-flight transcript. The
        // state publishes next_transcript_id after reservation, so transcript N
        // is current only while the route is still handshaking and the next id
        // is N + 1. This lets delayed coroutine completions detect that another
        // path has already replaced the route state.
        [[nodiscard]] auto route_attestation_claim_is_current(
            const canopy::security::attestation::route_attestation_state& state,
            uint64_t transcript_id) -> bool
        {
            return transcript_id != 0 && transcript_id != std::numeric_limits<uint64_t>::max()
                   && state.status == canopy::security::attestation::route_attestation_status::handshaking
                   && state.next_transcript_id == transcript_id + 1;
        }

        [[nodiscard]] auto route_attestation_state_is_admitted(
            const canopy::security::attestation::route_attestation_state& state) -> bool
        {
            using canopy::security::attestation::route_attestation_status;
            return (state.status == route_attestation_status::attested && state.context && state.context->established)
                   || state.status == route_attestation_status::unattested_allowed;
        }
    }

    // Stops enclave-owned coroutine infrastructure before service state is
    // destroyed.
    enclave_service::~enclave_service()
    {
        // The enclave service owns the coroutine-side io_uring controller. Ask it
        // to stop before the service object disappears so pending transport work
        // cannot keep using service state during teardown.
        if (auto controller = get_io_uring_controller())
            controller->request_shutdown();
    }

    // Stores an established protected-RPC security context for a route.
    void enclave_service::set_security_context(
        rpc::destination_zone attested_zone_id,
        canopy::security::attestation::security_context context)
    {
        // This is the only successful transition into the "attested" route
        // cache. Callers must already have verified evidence or received an
        // established context from an attested stream before storing it here.
        RPC_ASSERT(attested_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        canopy::security::attestation::route_attestation_state state;
        if (auto item = attestation_route_states_.find(attested_zone_id); item != attestation_route_states_.end())
            state.next_transcript_id = item->second.next_transcript_id;
        state.status = canopy::security::attestation::route_attestation_status::attested;
        state.context = std::move(context);
        attestation_route_states_[attested_zone_id] = std::move(state);
    }

    // Imports a security context that was already negotiated by an attested
    // stream before normal RPC traffic starts.
    bool enclave_service::publish_security_context_from_stream(
        rpc::destination_zone attested_zone_id,
        const std::shared_ptr<streaming::stream>& stream)
    {
        // Direct peer-to-peer streams can complete attestation before the RPC
        // layer is fully attached. If the stream implements security_context_source,
        // publish that already-established session for subsequent protected RPC.
        if (!attested_zone_id.is_set() || !stream)
            return false;

        auto source = std::dynamic_pointer_cast<canopy::security::attestation::security_context_source>(stream);
        if (!source)
            return false;

        auto context = source->security_context();
        if (!context.established)
            return false;

        set_security_context(attested_zone_id, std::move(context));
        return true;
    }

    // Removes all route-attestation state for a route when the context is no
    // longer valid.
    void enclave_service::remove_security_context(rpc::destination_zone attested_zone_id)
    {
        RPC_ASSERT(attested_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        attestation_route_states_.erase(attested_zone_id);
    }

    // Returns only established security contexts; policy-only and failed routes
    // are intentionally invisible to protected-RPC encryption.
    auto enclave_service::get_security_context(rpc::destination_zone attested_zone_id) const
        -> std::optional<canopy::security::attestation::security_context>
    {
        // Only established contexts are returned. Unknown, failed, handshaking,
        // and explicitly-unattested routes are not valid encryption contexts.
        RPC_ASSERT(attested_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(attested_zone_id);
        if (item == attestation_route_states_.end())
            return std::nullopt;
        if (item->second.status != canopy::security::attestation::route_attestation_status::attested)
            return std::nullopt;
        if (!item->second.context || !item->second.context->established)
            return std::nullopt;
        return item->second.context;
    }

    // Replaces a route's attestation state while preventing non-attested states
    // from retaining key material.
    void enclave_service::set_attestation_route_state(
        rpc::destination_zone attested_zone_id,
        canopy::security::attestation::route_attestation_state state)
    {
        // Keep the state machine internally consistent: only an established
        // security_context may be stored under an attested status. All other
        // statuses must not retain keys accidentally.
        RPC_ASSERT(attested_zone_id.get_subnet());
        std::lock_guard<std::mutex> lock(security_context_mutex_);
        if (state.status != canopy::security::attestation::route_attestation_status::attested)
        {
            state.context.reset();
        }
        else if (!state.context || !state.context->established)
        {
            state.status = canopy::security::attestation::route_attestation_status::unknown;
            state.context.reset();
        }
        attestation_route_states_[attested_zone_id] = std::move(state);
    }

    // Returns a copy of the current route state; callers must re-store updates
    // through set_attestation_route_state or another locked helper.
    auto enclave_service::get_attestation_route_state(rpc::destination_zone attested_zone_id) const
        -> canopy::security::attestation::route_attestation_state
    {
        if (!attested_zone_id.is_set())
            return {};

        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(attested_zone_id);
        if (item == attestation_route_states_.end())
            return {};
        return item->second;
    }

    // Attaches the enclave-wide attestation backend and snapshots its default
    // peer policy into this service's zone policy.
    void enclave_service::set_attestation_service(std::shared_ptr<canopy::security::attestation::attestation_service> service)
    {
        std::optional<canopy::security::attestation::attestation_policy> peer_policy;
        if (service)
            peer_policy = service->policy();

        {
            std::lock_guard<std::mutex> lock(attestation_service_mutex_);
            attestation_service_ = std::move(service);
        }

        if (peer_policy.has_value())
        {
            // Compatibility bridge for the current configuration model: the
            // attestation_service still carries backend appraisal defaults, and
            // the per-enclave-service route policy receives a snapshot of the
            // no-Evidence route-admission part. Future service flavours can set
            // their own zone_security_policy after attaching the shared
            // attestation_service.
            get_zone_security_policy()->set_peer_attestation_policy(std::move(peer_policy.value()));
        }
    }

    // Returns the shared attestation backend used for Evidence verification and
    // protected-RPC key derivation.
    auto enclave_service::get_attestation_service() const
        -> std::shared_ptr<canopy::security::attestation::attestation_service>
    {
        std::lock_guard<std::mutex> lock(attestation_service_mutex_);
        return attestation_service_;
    }

    // Installs this zone's route-admission policy; a null input restores the
    // default fail-closed policy object.
    void enclave_service::set_zone_security_policy(
        std::shared_ptr<canopy::security::attestation::zone_security_policy> policy)
    {
        if (!policy)
            policy = std::make_shared<canopy::security::attestation::zone_security_policy>();

        std::lock_guard<std::mutex> lock(zone_security_policy_mutex_);
        zone_security_policy_ = std::move(policy);
    }

    // Returns the policy object used to decide whether a route must attest,
    // may remain explicitly unattested, or must be rejected.
    auto enclave_service::get_zone_security_policy() const
        -> std::shared_ptr<canopy::security::attestation::zone_security_policy>
    {
        std::lock_guard<std::mutex> lock(zone_security_policy_mutex_);
        return zone_security_policy_;
    }

    // Enables or disables encrypted protected-RPC wrapping for application and
    // reference-control messages.
    void enclave_service::set_protected_rpc_enabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(attestation_service_mutex_);
        protected_rpc_enabled_ = enabled;
    }

    // Reports whether protected-RPC wrapping is currently required by this
    // enclave service.
    bool enclave_service::protected_rpc_enabled() const
    {
        std::lock_guard<std::mutex> lock(attestation_service_mutex_);
        return protected_rpc_enabled_;
    }

    // Compatibility setter for older call sites; the setting now lives on the
    // zone security policy.
    void enclave_service::set_add_ref_attestation_required(bool required)
    {
        get_zone_security_policy()->set_reference_routes_require_attestation(required);
    }

    // Compatibility getter for older call sites; reads the zone security policy.
    bool enclave_service::add_ref_attestation_required() const
    {
        return get_zone_security_policy()->reference_routes_require_attestation();
    }

    // Marks or unmarks a route as explicitly allowed without attestation.
    void enclave_service::set_route_unattested_allowed(
        rpc::destination_zone route_zone_id,
        bool allowed)
    {
        // This is an explicit policy override for routes such as JavaScript or
        // diagnostic clients that cannot currently produce enclave evidence.
        // It is route-scoped; it does not create a security_context or keys.
        if (!route_zone_id.is_set())
            return;

        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(route_zone_id);
        if (allowed)
        {
            canopy::security::attestation::route_attestation_state state;
            if (item != attestation_route_states_.end())
                state.next_transcript_id = item->second.next_transcript_id;
            state.status = canopy::security::attestation::route_attestation_status::unattested_allowed;
            attestation_route_states_[route_zone_id] = std::move(state);
            return;
        }

        if (item != attestation_route_states_.end()
            && item->second.status == canopy::security::attestation::route_attestation_status::unattested_allowed)
        {
            attestation_route_states_.erase(item);
        }
    }

    // Atomically decides whether add_ref may start a route handshake and, if so,
    // reserves the next transcript id before any coroutine suspension point.
    auto enclave_service::claim_add_ref_route_attestation(
        rpc::destination_zone route_zone_id,
        bool route_is_local,
        bool attestation_required) -> route_attestation_claim
    {
        route_attestation_claim claim;
        if (!route_zone_id.is_set())
        {
            claim.decision.action = canopy::security::attestation::route_attestation_action::reject;
            claim.decision.reason = "route zone id is not set";
            claim.error_code = rpc::error::INVALID_DATA();
            return claim;
        }

        std::lock_guard<std::mutex> lock(security_context_mutex_);
        if (auto item = attestation_route_states_.find(route_zone_id); item != attestation_route_states_.end())
            claim.state = item->second;

        canopy::security::attestation::reference_route_policy_input policy_input;
        policy_input.attestation_required = attestation_required;
        policy_input.route_is_local = route_is_local;
        policy_input.may_start_handshake = true;
        policy_input.state = claim.state;
        claim.decision = canopy::security::attestation::evaluate_reference_route_policy(policy_input);
        if (claim.decision.action != canopy::security::attestation::route_attestation_action::start_handshake)
            return claim;

        auto handshaking_state = claim.state;
        const auto transcript_id = handshaking_state.next_transcript_id;
        if (transcript_id == 0 || transcript_id == std::numeric_limits<uint64_t>::max())
        {
            // Record exhaustion while still under the route-state lock. Otherwise
            // another coroutine could observe "unknown" and try to start the same
            // impossible handshake before the failure state is visible.
            canopy::security::attestation::route_attestation_state failed_state;
            failed_state.status = canopy::security::attestation::route_attestation_status::failed;
            failed_state.failure_epoch = claim.state.failure_epoch + 1;
            failed_state.failure_reason = "route attestation transcript id exhausted";
            failed_state.next_transcript_id = claim.state.next_transcript_id;
            attestation_route_states_[route_zone_id] = std::move(failed_state);

            claim.decision.action = canopy::security::attestation::route_attestation_action::reject;
            claim.decision.reason = "route attestation transcript id exhausted";
            claim.error_code = rpc::error::RESOURCE_EXHAUSTED();
            return claim;
        }

        // This is the only transition from "may start" to "handshaking" for
        // add_ref. It reserves the transcript id and publishes handshaking while
        // still holding the route-state mutex; all expensive work happens after
        // returning to avoid holding a mutex across CO_AWAIT.
        handshaking_state.status = canopy::security::attestation::route_attestation_status::handshaking;
        handshaking_state.context.reset();
        handshaking_state.failure_reason.clear();
        ++handshaking_state.next_transcript_id;
        attestation_route_states_[route_zone_id] = handshaking_state;

        claim.state = std::move(handshaking_state);
        claim.transcript_id = transcript_id;
        return claim;
    }

    // Records failure for a route handshake only if this coroutine still owns
    // the active transcript claim.
    auto enclave_service::fail_claimed_attestation_route(
        rpc::destination_zone route_zone_id,
        uint64_t transcript_id,
        uint64_t previous_failure_epoch,
        std::string reason) -> bool
    {
        if (!route_zone_id.is_set())
            return false;

        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(route_zone_id);
        if (item == attestation_route_states_.end() || !route_attestation_claim_is_current(item->second, transcript_id))
        {
            return false;
        }

        canopy::security::attestation::route_attestation_state failed_state;
        failed_state.status = canopy::security::attestation::route_attestation_status::failed;
        failed_state.failure_epoch
            = (item->second.failure_epoch > previous_failure_epoch ? item->second.failure_epoch : previous_failure_epoch)
              + 1;
        failed_state.failure_reason = std::move(reason);
        failed_state.next_transcript_id = item->second.next_transcript_id;
        attestation_route_states_[route_zone_id] = std::move(failed_state);
        return true;
    }

    // Publishes an established protected-RPC context for the transcript that
    // this coroutine claimed. If another path already changed the route state,
    // the caller must not overwrite it with stale key material.
    auto enclave_service::complete_claimed_attestation_route(
        rpc::destination_zone route_zone_id,
        uint64_t transcript_id,
        canopy::security::attestation::security_context context) -> bool
    {
        if (!route_zone_id.is_set() || !context.established)
            return false;

        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(route_zone_id);
        if (item == attestation_route_states_.end() || !route_attestation_claim_is_current(item->second, transcript_id))
        {
            return false;
        }

        canopy::security::attestation::route_attestation_state completed_state;
        completed_state.status = canopy::security::attestation::route_attestation_status::attested;
        completed_state.context = std::move(context);
        completed_state.next_transcript_id = item->second.next_transcript_id;
        attestation_route_states_[route_zone_id] = std::move(completed_state);
        return true;
    }

    // Completes a no-Evidence route admission only for the transcript that was
    // actually claimed by this coroutine.
    auto enclave_service::complete_claimed_unattested_route(
        rpc::destination_zone route_zone_id,
        uint64_t transcript_id) -> bool
    {
        if (!route_zone_id.is_set())
            return false;

        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(route_zone_id);
        if (item == attestation_route_states_.end() || !route_attestation_claim_is_current(item->second, transcript_id))
        {
            return false;
        }

        canopy::security::attestation::route_attestation_state completed_state;
        completed_state.status = canopy::security::attestation::route_attestation_status::unattested_allowed;
        completed_state.next_transcript_id = item->second.next_transcript_id;
        attestation_route_states_[route_zone_id] = std::move(completed_state);
        return true;
    }

    // Records an inbound handshake failure only when doing so cannot destroy a
    // better route state. Inbound handshakes are unauthenticated until their
    // evidence verifies, so a stale or malicious failure must not downgrade an
    // already admitted route or an outbound transcript that is still running.
    auto enclave_service::record_inbound_attestation_failure(
        rpc::destination_zone route_zone_id,
        std::string reason) -> bool
    {
        if (!route_zone_id.is_set())
            return false;

        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(route_zone_id);
        if (item != attestation_route_states_.end())
        {
            const auto& current = item->second;
            if (route_attestation_state_is_admitted(current)
                || current.status == canopy::security::attestation::route_attestation_status::handshaking)
            {
                return false;
            }
        }

        canopy::security::attestation::route_attestation_state failed_state;
        if (item != attestation_route_states_.end())
        {
            failed_state.failure_epoch = item->second.failure_epoch + 1;
            failed_state.next_transcript_id = item->second.next_transcript_id;
        }
        else
        {
            failed_state.failure_epoch = 1;
        }
        failed_state.status = canopy::security::attestation::route_attestation_status::failed;
        failed_state.failure_reason = std::move(reason);
        attestation_route_states_[route_zone_id] = std::move(failed_state);
        return true;
    }

    // Stores an inbound attested context unless the route is already admitted or
    // has been explicitly failed. Existing admitted routes are preserved until a
    // future re-attestation protocol intentionally claims replacement.
    auto enclave_service::complete_inbound_attestation_route(
        rpc::destination_zone route_zone_id,
        canopy::security::attestation::security_context context) -> bool
    {
        if (!route_zone_id.is_set() || !context.established)
            return false;

        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(route_zone_id);
        if (item != attestation_route_states_.end())
        {
            if (route_attestation_state_is_admitted(item->second))
                return true;
            if (item->second.status == canopy::security::attestation::route_attestation_status::failed)
                return false;
        }

        canopy::security::attestation::route_attestation_state completed_state;
        if (item != attestation_route_states_.end())
            completed_state.next_transcript_id = item->second.next_transcript_id;
        completed_state.status = canopy::security::attestation::route_attestation_status::attested;
        completed_state.context = std::move(context);
        attestation_route_states_[route_zone_id] = std::move(completed_state);
        return true;
    }

    // Stores an explicit no-Evidence admission without downgrading an already
    // attested route. No-Evidence admission never creates key material.
    auto enclave_service::complete_inbound_unattested_route(rpc::destination_zone route_zone_id) -> bool
    {
        if (!route_zone_id.is_set())
            return false;

        std::lock_guard<std::mutex> lock(security_context_mutex_);
        auto item = attestation_route_states_.find(route_zone_id);
        if (item != attestation_route_states_.end())
        {
            if (route_attestation_state_is_admitted(item->second))
                return true;
            if (item->second.status == canopy::security::attestation::route_attestation_status::failed)
                return false;
        }

        canopy::security::attestation::route_attestation_state completed_state;
        if (item != attestation_route_states_.end())
            completed_state.next_transcript_id = item->second.next_transcript_id;
        completed_state.status = canopy::security::attestation::route_attestation_status::unattested_allowed;
        attestation_route_states_[route_zone_id] = std::move(completed_state);
        return true;
    }

    // Converts a superseded handshake completion into the result that add_ref
    // should observe. If the route is now admitted by another path, the original
    // add_ref may continue; otherwise return the original failure code without
    // mutating route state.
    auto enclave_service::result_for_superseded_add_ref_claim(
        rpc::destination_zone route_zone_id,
        const char* operation,
        int fallback_error_code) const -> rpc::standard_result
    {
        auto zone_policy = get_zone_security_policy();
        auto state = get_attestation_route_state(route_zone_id);
        canopy::security::attestation::reference_route_policy_input policy_input;
        policy_input.route_is_local = route_zone_id == get_zone_id();
        policy_input.may_start_handshake = false;
        policy_input.state = state;
        const auto decision = zone_policy->evaluate_reference_route(std::move(policy_input));
        if (decision.action == canopy::security::attestation::route_attestation_action::allow)
        {
            RPC_DEBUG(
                "route attestation claim for route {} during {} was superseded by an admitted route",
                std::to_string(route_zone_id),
                operation);
            return standard_result{rpc::error::OK(), {}};
        }

        RPC_WARNING(
            "route attestation claim for route {} during {} was superseded; current status={}, reason {}",
            std::to_string(route_zone_id),
            operation,
            static_cast<int>(state.status),
            decision.reason);
        return standard_result{fallback_error_code, {}};
    }

    // Ensures the route referenced by add_ref is already allowed or completes
    // the route-level attestation handshake needed to allow it.
    CORO_TASK(standard_result)
    enclave_service::ensure_add_ref_route_allowed(
        rpc::destination_zone route_zone_id,
        const char* operation)
    {
        // add_ref is the first point where a remote interface reference can
        // appear before application code sees it. When policy requires it, the
        // route owning that reference must be known, attested, or explicitly
        // allowed before reference state is created.
        if (!route_zone_id.is_set())
            CO_RETURN standard_result{rpc::error::INVALID_DATA(), {}};

        auto zone_policy = get_zone_security_policy();
        const auto route_is_local = route_zone_id == get_zone_id();
        const auto attestation_required = zone_policy->reference_routes_require_attestation();
        auto claim = claim_add_ref_route_attestation(route_zone_id, route_is_local, attestation_required);
        const auto& decision = claim.decision;
        if (decision.action == canopy::security::attestation::route_attestation_action::allow)
        {
            // "allow" covers already-attested routes and routes explicitly marked
            // unattested_allowed by local policy. It also covers local routes,
            // where remote attestation is not the isolation boundary.
            CO_RETURN standard_result{rpc::error::OK(), {}};
        }
        if (decision.action == canopy::security::attestation::route_attestation_action::reject)
        {
            RPC_WARNING(
                "add_ref attestation rejected for route {} during {}: previous failure {}",
                route_zone_id.get_subnet(),
                operation,
                decision.reason);
            CO_RETURN standard_result{
                claim.error_code != rpc::error::OK() ? claim.error_code : rpc::error::ZONE_NOT_SUPPORTED(), {}};
        }
        const auto wait_deadline = std::chrono::steady_clock::now() + route_attestation_wait_timeout;
        while (decision.action == canopy::security::attestation::route_attestation_action::wait_for_handshake)
        {
            // Another coroutine already owns the handshake. Wait outside
            // security_context_mutex_ so the owner can complete and publish the
            // route state. This preserves normal add_ref ordering without
            // allowing duplicate handshakes for the same route.
            if (std::chrono::steady_clock::now() >= wait_deadline)
            {
                RPC_WARNING("add_ref attestation timed out for route {} during {}", route_zone_id.get_subnet(), operation);
                CO_RETURN standard_result{rpc::error::CALL_TIMEOUT(), {}};
            }

            auto scheduler = get_scheduler();
            if (!scheduler)
                CO_RETURN standard_result{rpc::error::TRANSPORT_ERROR(), {}};

            CO_AWAIT scheduler->schedule_after(route_attestation_wait_poll_interval);
            claim = claim_add_ref_route_attestation(route_zone_id, route_is_local, attestation_required);
        }
        if (decision.action == canopy::security::attestation::route_attestation_action::allow)
            CO_RETURN standard_result{rpc::error::OK(), {}};
        if (decision.action == canopy::security::attestation::route_attestation_action::reject)
        {
            RPC_WARNING(
                "add_ref attestation rejected for route {} during {} after wait: previous failure {}",
                route_zone_id.get_subnet(),
                operation,
                decision.reason);
            CO_RETURN standard_result{
                claim.error_code != rpc::error::OK() ? claim.error_code : rpc::error::ZONE_NOT_SUPPORTED(), {}};
        }

        auto fail_claim = [&](std::string reason, int error_code) -> standard_result
        {
            if (!fail_claimed_attestation_route(
                    route_zone_id, claim.transcript_id, claim.state.failure_epoch, std::move(reason)))
                return result_for_superseded_add_ref_claim(route_zone_id, operation, error_code);
            return standard_result{error_code, {}};
        };

        auto complete_unattested_claim = [&]() -> standard_result
        {
            if (!complete_claimed_unattested_route(route_zone_id, claim.transcript_id))
            {
                return result_for_superseded_add_ref_claim(route_zone_id, operation, rpc::error::ZONE_NOT_SUPPORTED());
            }
            return standard_result{rpc::error::OK(), {}};
        };

        auto complete_attested_claim = [&](canopy::security::attestation::security_context context) -> standard_result
        {
            if (!complete_claimed_attestation_route(route_zone_id, claim.transcript_id, std::move(context)))
            {
                return result_for_superseded_add_ref_claim(route_zone_id, operation, rpc::error::ZONE_NOT_SUPPORTED());
            }
            return standard_result{rpc::error::OK(), {}};
        };

        auto service = get_attestation_service();

        rpc::route_attestation_handshake_request request;
        request.transcript_id = claim.transcript_id;
        request.claimant = to_wire_identity(local_identity_for_route(service, get_zone_id()));
        if (service)
        {
            // Evidence is optional because fake/null/demo modes and some clients
            // can be policy-allowed without enclave evidence. When configured to
            // send evidence, the nonce and transcript id bind the evidence to
            // this specific handshake and prevent replay across transcripts.
            request.backend_id = service->backend_id();
            if (service->should_send_local_evidence() || service->supports_verifier_challenge())
            {
                auto nonce = canopy::security::attestation::make_attestation_nonce();
                if (!nonce.has_value())
                {
                    CO_RETURN fail_claim("failed to generate route attestation nonce", rpc::error::SECURITY_ERROR());
                }
                request.nonce = std::move(nonce.value());
            }

            if (service->supports_verifier_challenge())
            {
                // SGX local attestation needs the verifier to send target_info
                // before the peer can create a report targeted at this enclave.
                // The challenge remains backend-neutral at this layer: a CMW
                // blob whose type is defined by the backend-specific IDL file.
                auto challenge = service->make_verifier_challenge(request.transcript_id, request.nonce);
                if (!challenge.accepted)
                {
                    CO_RETURN fail_claim(std::move(challenge.reason), rpc::error::SECURITY_ERROR());
                }
                request.verifier_challenge = to_wire_cmw(challenge.evidence);
            }

            if (service->should_send_local_evidence())
            {
                auto evidence = service->produce_evidence(request.transcript_id, request.nonce);
                if (!evidence.accepted)
                {
                    CO_RETURN fail_claim(std::move(evidence.reason), rpc::error::SECURITY_ERROR());
                }
                request.evidence = to_wire_cmw(evidence.evidence);
            }
        }

        // The route handshake is intentionally just another RPC marshaller
        // payload: type fingerprint + encoding + serialized IDL struct. The
        // transport does not need to understand the struct; it only routes the
        // handshake to route_zone_id.
        const auto payload_encoding = get_default_encoding();
        auto request_payload = serialise_route_attestation_payload(request, payload_encoding);
        if (!request_payload.has_value())
        {
            CO_RETURN fail_claim("failed to serialise route attestation request", rpc::error::INVALID_DATA());
        }

        handshake_params params;
        params.protocol_version = rpc::HIGHEST_SUPPORTED_VERSION;
        params.caller_zone_id = get_zone_id();
        params.destination_zone_id = route_zone_id;
        params.type_id = route_attestation_request_type_id(params.protocol_version);
        params.payload_encoding = payload_encoding;
        params.payload = std::move(request_payload.value());
        // This call bounces the first typed IDL blob to the destination zone. If
        // the route crosses intermediates, they can route the message but should
        // not need to parse the request payload.
        auto handshake = CO_AWAIT child_service::handshake(std::move(params));

        if (handshake.error_code != rpc::error::OK())
        {
            RPC_WARNING(
                "add_ref attestation failed for route {} during {}, handshake error {}",
                route_zone_id.get_subnet(),
                operation,
                handshake.error_code);
            CO_RETURN fail_claim("route attestation handshake transport failed", rpc::error::ZONE_NOT_SUPPORTED());
        }

        if (handshake.type_id != route_attestation_response_type_id(rpc::HIGHEST_SUPPORTED_VERSION))
        {
            CO_RETURN fail_claim(
                "route attestation handshake returned an unexpected payload type", rpc::error::INVALID_DATA());
        }

        // The response is the second typed IDL blob in this protocol file. The
        // type id already names the response schema, and transcript_id ties it
        // back to this concrete request.
        rpc::route_attestation_handshake_response response;
        if (!parse_route_attestation_payload(handshake.payload, payload_encoding, response)
            || response.transcript_id != request.transcript_id || !flag_is_valid(response.accepted))
        {
            CO_RETURN fail_claim("route attestation handshake returned malformed payload", rpc::error::INVALID_DATA());
        }

        if (!response.accepted)
        {
            CO_RETURN fail_claim(std::move(response.reason), rpc::error::ZONE_NOT_SUPPORTED());
        }

        if (!service)
        {
            // No local attestation service means no key material can be
            // established. The only possible successful state is an explicit
            // unattested route, which is still tracked separately from "attested".
            auto result = complete_unattested_claim();
            result.out_back_channel = std::move(handshake.out_back_channel);
            CO_RETURN result;
        }

        if (response.evidence.has_value())
        {
            // The peer supplied evidence in the response. Verify it before
            // accepting any identity, backend id, security level, or session keys
            // derived from the peer's claims.
            canopy::security::attestation::attestation_verdict verdict;
            if (request.verifier_challenge.has_value())
            {
                // Challenge-bound evidence is verified against the challenge
                // nonce and target_info we sent in the request. The response
                // does not need a second nonce for this evidence shape.
                canopy::security::attestation::evidence_binding expected_binding;
                expected_binding.subject = from_wire_identity(response.responder);
                expected_binding.transcript_id = response.transcript_id;
                expected_binding.nonce = request.nonce;
                verdict = service->verify_peer_evidence_for_challenge(
                    from_wire_cmw(response.evidence.value()),
                    from_wire_cmw(request.verifier_challenge.value()),
                    std::move(expected_binding));
            }
            else
            {
                if (!nonce_is_valid(response.nonce))
                {
                    CO_RETURN fail_claim("route attestation response nonce was invalid", rpc::error::INVALID_DATA());
                }

                canopy::security::attestation::evidence_binding expected_binding;
                expected_binding.subject = from_wire_identity(response.responder);
                expected_binding.transcript_id = response.transcript_id;
                expected_binding.nonce = response.nonce;
                verdict = service->verify_peer_evidence(
                    from_wire_cmw(response.evidence.value()), std::move(expected_binding));
            }

            if (!verdict.accepted)
            {
                CO_RETURN fail_claim(std::move(verdict.reason), rpc::error::ZONE_NOT_SUPPORTED());
            }
            if (response.backend_id != verdict.backend_id
                || from_wire_security_level(response.security_level) != verdict.level)
            {
                CO_RETURN fail_claim(
                    "route attestation response metadata did not match verified evidence",
                    rpc::error::ZONE_NOT_SUPPORTED());
            }

            canopy::security::attestation::establish_session_params session_params;
            session_params.peer_identity = verdict.peer_identity;
            session_params.transcript_id = response.transcript_id;
            session_params.local_evidence_sent = request.evidence.has_value();
            session_params.peer_attested = true;
            session_params.verified_backend_id = verdict.backend_id;
            session_params.verified_level = verdict.level;
            session_params.session_epoch = response.session_epoch;
            auto context = service->establish_session(session_params);
            if (!context.established)
            {
                CO_RETURN fail_claim("route attestation session establishment failed", rpc::error::SECURITY_ERROR());
            }

            auto result = complete_attested_claim(std::move(context));
            result.out_back_channel = std::move(handshake.out_back_channel);
            CO_RETURN result;
        }

        // If the peer did not provide evidence, fall back to the local policy.
        // This is not treated as attested and will not produce a security_context.
        const auto no_evidence_decision = get_zone_security_policy()->evaluate_missing_peer_evidence();
        if (!no_evidence_decision.accepted)
        {
            CO_RETURN fail_claim(std::move(no_evidence_decision.reason), rpc::error::ZONE_NOT_SUPPORTED());
        }

        auto result = complete_unattested_claim();
        result.out_back_channel = std::move(handshake.out_back_channel);
        CO_RETURN result;
    }

    // Checks normal-mode reference/control routes without starting or waiting
    // for attestation. These calls must follow an earlier admitted add_ref.
    CORO_TASK(standard_result)
    enclave_service::ensure_existing_reference_route_allowed(
        rpc::destination_zone route_zone_id,
        const char* operation) const
    {
        // add_ref is the boundary between the handshake/admission phase and
        // normal RPC mode. Non-add_ref operations that arrive while the route is
        // unknown or handshaking are suspicious: they must not start a handshake,
        // and they must not wait for one to complete as if they were valid early
        // traffic.
        if (!route_zone_id.is_set())
            CO_RETURN standard_result{rpc::error::INVALID_DATA(), {}};

        auto zone_policy = get_zone_security_policy();
        auto state = get_attestation_route_state(route_zone_id);
        canopy::security::attestation::reference_route_policy_input policy_input;
        policy_input.route_is_local = route_zone_id == get_zone_id();
        policy_input.may_start_handshake = false;
        policy_input.state = state;
        const auto decision = zone_policy->evaluate_reference_route(std::move(policy_input));
        if (decision.action == canopy::security::attestation::route_attestation_action::allow)
            CO_RETURN standard_result{rpc::error::OK(), {}};

        RPC_WARNING(
            "normal-mode RPC rejected before route admission for route {} during {}: status={}, reason {}",
            route_zone_id.get_subnet(),
            operation,
            static_cast<int>(state.status),
            decision.reason);
        CO_RETURN standard_result{rpc::error::FRAUDULANT_REQUEST(), {}};
    }

    // Finds the destination context for a protected outbound call that
    // originates in this service's own zone.
    auto enclave_service::find_security_context_for_protected_call(
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id) const -> std::optional<canopy::security::attestation::security_context>
    {
        // Outbound protected calls are only created for traffic that originates
        // in this service's own zone. If a forwarded message names a different
        // caller, this service must not encrypt it with its own keys.
        if (caller_zone_id != get_zone_id())
            return std::nullopt;
        if (!destination_zone_id.is_set())
            return std::nullopt;
        return get_security_context(destination_zone_id);
    }

    // Handles inbound add_ref, including optional protected payload unwrap and
    // first-use route admission.
    CORO_TASK(standard_result)
    enclave_service::add_ref(add_ref_params params)
    {
        // Inbound add_ref may arrive as either:
        // - a protected payload from an attested route, or
        // - a plaintext control message from an allowed/unattested route.
        //
        // If protected RPC is enabled, an unknown non-empty typed payload is not
        // forwarded. That avoids letting a malicious peer smuggle an extension
        // blob through reference-counting code as if it were ordinary plaintext.
        const bool protected_payload
            = canopy::security::attestation::is_protected_rpc_payload(params.payload, params.protocol_version);
        if (protected_payload)
        {
            auto service = get_attestation_service();
            if (!protected_rpc_enabled() || !service)
                CO_RETURN standard_result{rpc::error::ZONE_NOT_SUPPORTED(), {}};

            auto request = canopy::security::attestation::unprotect_add_ref_request(*service, params);
            if (!request.accepted)
            {
                RPC_ERROR(
                    "protected RPC unprotect add_ref failed: code={} ({}) reason={}",
                    request.error.error_code,
                    rpc::error::to_string(request.error.error_code),
                    request.error.reason);
                CO_RETURN standard_result{request.error.error_code, {}};
            }
            params = std::move(request.value.params);
        }
        else if (protected_rpc_enabled() && params.payload)
        {
            // A non-empty typed payload with an unknown fingerprint may simply
            // be a newer peer using a schema this build does not understand. It
            // is rejected, but not classified as fraud/blacklist material.
            CO_RETURN standard_result{rpc::error::INVALID_VERSION(), {}};
        }

        const auto route_zone_id = params.remote_object_id.as_zone();
        // The attestation route is the owner of the referenced remote_object,
        // not necessarily the adjacent transport peer that delivered this add_ref.
        auto route_result = CO_AWAIT ensure_add_ref_route_allowed(route_zone_id, "inbound add_ref remote object");
        if (route_result.error_code != rpc::error::OK())
            CO_RETURN route_result;

        if (!protected_payload && protected_rpc_enabled() && get_security_context(route_zone_id))
        {
            // Once a route has an established protected context, downgrade to a
            // plaintext add_ref is rejected. That prevents an intermediate from
            // stripping the protected wrapper while keeping routable fields.
            RPC_WARNING("plaintext add_ref rejected for protected route {}", rpc::to_string(route_zone_id));
            CO_RETURN standard_result{rpc::error::FRAUDULANT_REQUEST(), {}};
        }

        auto result = CO_AWAIT child_service::add_ref(std::move(params));
        CO_RETURN sanitise_public_control_result(std::move(result), "add_ref");
    }

    // Handles inbound release after verifying the caller route was previously
    // admitted by add_ref.
    CORO_TASK(standard_result)
    enclave_service::release(release_params params)
    {
        // release is paired with a prior add_ref. It should not create trust or
        // trigger a new handshake; it only checks that the caller route is
        // already in an allowed state before reference counts are changed.
        const bool protected_payload
            = canopy::security::attestation::is_protected_rpc_payload(params.payload, params.protocol_version);
        if (protected_payload)
        {
            auto service = get_attestation_service();
            if (!protected_rpc_enabled() || !service)
                CO_RETURN standard_result{rpc::error::ZONE_NOT_SUPPORTED(), {}};

            auto request = canopy::security::attestation::unprotect_release_request(*service, params);
            if (!request.accepted)
            {
                RPC_ERROR(
                    "protected RPC unprotect release failed: code={} ({}) reason={}",
                    request.error.error_code,
                    rpc::error::to_string(request.error.error_code),
                    request.error.reason);
                CO_RETURN standard_result{request.error.error_code, {}};
            }
            params = std::move(request.value.params);
        }
        else if (protected_rpc_enabled() && params.payload)
        {
            // Unknown typed payloads are version/compatibility failures rather
            // than evidence of malicious behaviour.
            CO_RETURN standard_result{rpc::error::INVALID_VERSION(), {}};
        }

        auto route_result
            = CO_AWAIT ensure_existing_reference_route_allowed(params.caller_zone_id, "inbound release caller");
        if (route_result.error_code != rpc::error::OK())
            CO_RETURN route_result;

        if (!protected_payload && protected_rpc_enabled() && get_security_context(params.caller_zone_id))
        {
            RPC_WARNING("plaintext release rejected for protected route {}", rpc::to_string(params.caller_zone_id));
            CO_RETURN standard_result{rpc::error::FRAUDULANT_REQUEST(), {}};
        }

        auto result = CO_AWAIT child_service::release(std::move(params));
        CO_RETURN sanitise_public_control_result(std::move(result), "release");
    }

    // Handles inbound object lifetime notifications from an owner route.
    CORO_TASK(void)
    enclave_service::object_released(object_released_params params)
    {
        // object_released is generated by an owner zone to tell peers that a
        // referenced object is gone. If the owner route was protected, require
        // the protected payload so an intermediate cannot forge object lifetime
        // updates for that route.
        const bool protected_payload
            = canopy::security::attestation::is_protected_rpc_payload(params.payload, params.protocol_version);
        if (protected_payload)
        {
            auto service = get_attestation_service();
            if (!protected_rpc_enabled() || !service)
                CO_RETURN;

            auto request = canopy::security::attestation::unprotect_object_released_request(*service, params);
            if (!request.accepted)
            {
                RPC_ERROR(
                    "protected RPC unprotect object_released failed: code={} ({}) reason={}",
                    request.error.error_code,
                    rpc::error::to_string(request.error.error_code),
                    request.error.reason);
                CO_RETURN;
            }
            params = std::move(request.value.params);
        }
        else if (protected_rpc_enabled() && params.payload)
        {
            CO_RETURN;
        }

        const auto route_zone_id = params.remote_object_id.as_zone();
        auto route_result
            = CO_AWAIT ensure_existing_reference_route_allowed(route_zone_id, "inbound object_released owner");
        if (route_result.error_code != rpc::error::OK())
            CO_RETURN;

        if (!protected_payload && protected_rpc_enabled() && get_security_context(route_zone_id))
        {
            RPC_WARNING("plaintext object_released rejected for protected route {}", rpc::to_string(route_zone_id));
            CO_RETURN;
        }

        CO_AWAIT child_service::object_released(std::move(params));
    }

    // Handles inbound transport-down notifications, accepting route-layer
    // plaintext liveness reports while still supporting protected endpoint
    // reports.
    CORO_TASK(void)
    enclave_service::transport_down(transport_down_params params)
    {
        // transport_down is special: an intermediate may legitimately report a
        // route-liveness failure even though it cannot attest on behalf of the
        // failed endpoint. Protected transport_down is accepted when present, but
        // empty plaintext transport_down remains part of the route layer.
        const bool protected_payload
            = canopy::security::attestation::is_protected_rpc_payload(params.payload, params.protocol_version);
        if (protected_payload)
        {
            auto service = get_attestation_service();
            if (!protected_rpc_enabled() || !service)
                CO_RETURN;

            auto request = canopy::security::attestation::unprotect_transport_down_request(*service, params);
            if (!request.accepted)
            {
                RPC_ERROR(
                    "protected RPC unprotect transport_down failed: code={} ({}) reason={}",
                    request.error.error_code,
                    rpc::error::to_string(request.error.error_code),
                    request.error.reason);
                CO_RETURN;
            }
            params = std::move(request.value.params);
        }
        else if (protected_rpc_enabled() && params.payload)
        {
            CO_RETURN;
        }

        auto route_result
            = CO_AWAIT ensure_existing_reference_route_allowed(params.caller_zone_id, "inbound transport_down caller");
        if (route_result.error_code != rpc::error::OK())
            CO_RETURN;

        // Intermediates may synthesize route-layer transport_down messages and
        // usually cannot attest on behalf of the failed endpoint. Empty
        // plaintext transport_down remains valid for that liveness path.
        CO_AWAIT child_service::transport_down(std::move(params));
    }

    // Handles the route-attestation handshake RPC used to admit new referenced
    // routes after the initial stream connection exists.
    CORO_TASK(handshake_result)
    enclave_service::handshake(handshake_params params)
    {
        // This is the inbound side of the same two-blob exchange started by
        // ensure_add_ref_route_allowed():
        //   1. receive a route_attestation_handshake_request IDL blob;
        //   2. verify the type fingerprint, transcript id, flags, and Evidence;
        //   3. optionally produce local Evidence;
        //   4. return a route_attestation_handshake_response IDL blob using the
        //      caller's payload encoding.
        if (params.destination_zone_id != get_zone_id())
        {
            // Not addressed to this zone. Keep passthrough semantics intact and
            // let the normal child_service route it onward.
            CO_RETURN CO_AWAIT child_service::handshake(std::move(params));
        }

        if (params.protocol_version < rpc::LOWEST_SUPPORTED_VERSION
            || params.protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            CO_RETURN handshake_result{rpc::error::INVALID_VERSION(), 0, {}, {}};
        }

        if (params.type_id != route_attestation_request_type_id(params.protocol_version))
        {
            CO_RETURN handshake_result{rpc::error::NOT_IMPLEMENTED(), 0, {}, {}};
        }

        if (params.payload_encoding == rpc::encoding::not_set)
            params.payload_encoding = get_default_encoding();

        auto service = get_attestation_service();
        auto responder_identity = local_identity_for_route(service, get_zone_id());
        rpc::route_attestation_handshake_request request;
        if (!parse_route_attestation_payload(params.payload, params.payload_encoding, request) || request.transcript_id == 0)
        {
            // Always return a typed failure response for a malformed request when
            // possible. The peer receives only a concise reason; local logging can
            // carry more detail if needed.
            auto response = make_failed_handshake_response(
                params.protocol_version, 0, "malformed route attestation request", responder_identity);
            CO_RETURN make_route_attestation_handshake_result(
                params.protocol_version, params.payload_encoding, std::move(response));
        }

        auto fail = [&](std::string reason) -> handshake_result
        {
            // A failed inbound handshake can only mark an unknown/failed route.
            // It must not poison a route that was already admitted by a stream
            // context, a previous route handshake, or an outbound handshake that
            // is still in progress.
            if (!record_inbound_attestation_failure(params.caller_zone_id, reason))
            {
                RPC_DEBUG(
                    "ignored inbound route attestation failure for already-active route {}",
                    std::to_string(params.caller_zone_id));
            }
            auto response = make_failed_handshake_response(
                params.protocol_version, request.transcript_id, std::move(reason), responder_identity);
            return make_route_attestation_handshake_result(
                params.protocol_version, params.payload_encoding, std::move(response));
        };

        if (!service)
        {
            CO_RETURN fail("no local attestation service configured");
        }

        rpc::route_attestation_handshake_response response;
        response.transcript_id = request.transcript_id;
        response.responder = to_wire_identity(responder_identity);
        response.backend_id = service->backend_id();
        response.security_level = wire_security_level(service->backend_level());
        response.session_epoch = route_attestation_session_epoch;

        bool peer_attested = false;
        canopy::security::attestation::attestation_verdict verdict;
        if (request.evidence.has_value())
        {
            // Verify the claimant's evidence against the claimant identity,
            // nonce, and transcript id carried by this request. A successful
            // verdict is the only source of trusted peer identity here.
            if (!nonce_is_valid(request.nonce))
            {
                CO_RETURN fail("route attestation request nonce was invalid");
            }

            canopy::security::attestation::evidence_binding expected_binding;
            expected_binding.subject = from_wire_identity(request.claimant);
            expected_binding.transcript_id = request.transcript_id;
            expected_binding.nonce = request.nonce;
            verdict = service->verify_peer_evidence(from_wire_cmw(request.evidence.value()), std::move(expected_binding));
            if (!verdict.accepted)
            {
                CO_RETURN fail(verdict.reason);
            }
            peer_attested = true;
        }
        else
        {
            // A no-Evidence handshake is an explicit policy decision. It is
            // useful for gateways and development clients, but it never creates
            // a protected security_context and must not be confused with
            // attested identity.
            const auto no_evidence_decision = get_zone_security_policy()->evaluate_missing_peer_evidence();
            if (!no_evidence_decision.accepted)
            {
                CO_RETURN fail(no_evidence_decision.reason);
            }
        }

        if (service->should_send_local_evidence())
        {
            if (request.verifier_challenge.has_value())
            {
                // Prefer verifier-challenge evidence when the requester supplied
                // one. For SGX local attestation this means the response carries
                // a report targeted at the requester enclave instead of another
                // self-targeted report.
                auto evidence = service->produce_evidence_for_challenge(
                    from_wire_cmw(request.verifier_challenge.value()), response.transcript_id, request.nonce);
                if (!evidence.accepted)
                {
                    CO_RETURN fail(evidence.reason);
                }

                response.evidence = to_wire_cmw(evidence.evidence);
            }
            else
            {
                // Backends that do not use verifier challenges continue to use
                // the original two-blob evidence exchange with a fresh response
                // nonce chosen by this responder.
                auto nonce = canopy::security::attestation::make_attestation_nonce();
                if (!nonce.has_value())
                {
                    CO_RETURN fail("failed to generate route attestation response nonce");
                }
                response.nonce = std::move(nonce.value());

                auto evidence = service->produce_evidence(response.transcript_id, response.nonce);
                if (!evidence.accepted)
                {
                    CO_RETURN fail(evidence.reason);
                }

                response.evidence = to_wire_cmw(evidence.evidence);
            }
        }

        if (peer_attested)
        {
            // Only attested peers get a stored security_context. That context is
            // later used by outbound/inbound protected RPC wrappers to derive
            // AEAD keys and monotonic counters for this route.
            canopy::security::attestation::establish_session_params session_params;
            session_params.peer_identity = verdict.peer_identity;
            session_params.transcript_id = request.transcript_id;
            session_params.local_evidence_sent = response.evidence.has_value();
            session_params.peer_attested = true;
            session_params.verified_backend_id = verdict.backend_id;
            session_params.verified_level = verdict.level;
            session_params.session_epoch = response.session_epoch;
            auto context = service->establish_session(session_params);
            if (!context.established)
            {
                CO_RETURN fail("route attestation session establishment failed");
            }
            if (!complete_inbound_attestation_route(params.caller_zone_id, std::move(context)))
                CO_RETURN fail("route attestation route state rejected inbound context");
        }
        else
        {
            // Policy accepted this caller without evidence. Track that explicit
            // decision, but do not manufacture protected-RPC key material.
            if (!complete_inbound_unattested_route(params.caller_zone_id))
                CO_RETURN fail("route attestation route state rejected inbound no-evidence admission");
        }

        response.accepted = route_attestation_flag_true;
        response.reason = peer_attested ? "route attestation accepted" : "unattested route accepted by policy";
        CO_RETURN make_route_attestation_handshake_result(
            params.protocol_version, params.payload_encoding, std::move(response));
    }

    // Handles inbound send by decrypting protected requests and encrypting the
    // send response so application error codes and payloads stay private.
    CORO_TASK(send_result)
    enclave_service::send(send_params params)
    {
        // Protected send uses a public carrier interface/method id whose in_data
        // is an encrypted_payload. Plain application sends still flow through the
        // base child_service path.
        if (!canopy::security::attestation::is_protected_rpc_envelope(
                params.interface_id, params.method_id, params.protocol_version))
        {
            CO_RETURN CO_AWAIT child_service::send(std::move(params));
        }

        auto service = get_attestation_service();
        if (!protected_rpc_enabled() || !service)
        {
            CO_RETURN send_result{rpc::error::ZONE_NOT_SUPPORTED(), {}, {}};
        }

        const auto outer = params;
        // Preserve the public carrier exactly for response authentication. The
        // response AAD binds to this carrier and the accepted request counter.
        auto request = canopy::security::attestation::unprotect_send_request(*service, outer);
        if (!request.accepted)
        {
            RPC_ERROR(
                "protected RPC unprotect send request failed: code={} ({}) reason={}",
                request.error.error_code,
                rpc::error::to_string(request.error.error_code),
                request.error.reason);
            CO_RETURN send_result{request.error.error_code, {}, {}};
        }

        auto response = CO_AWAIT child_service::send(std::move(request.value.params));
        // Application error_code and out_buf are encrypted in the protected
        // response. The public carrier should contain only safe RPC status.
        auto protected_response = canopy::security::attestation::protect_send_response(
            *service, request.value.context, outer, request.value.request_counter, std::move(response));
        if (!protected_response.accepted)
        {
            RPC_ERROR(
                "protected RPC protect send response failed: code={} ({}) reason={}",
                protected_response.error.error_code,
                rpc::error::to_string(protected_response.error.error_code),
                protected_response.error.reason);
            CO_RETURN send_result{protected_response.error.error_code, {}, {}};
        }

        CO_RETURN std::move(protected_response.value);
    }

    // Handles inbound one-way post by decrypting protected requests before
    // dispatch and dropping unauthenticated protected traffic.
    CORO_TASK(void)
    enclave_service::post(post_params params)
    {
        // post is one-way, so there is no protected response path. Authentication
        // failure simply drops the message rather than returning application
        // detail to an untrusted caller.
        if (!canopy::security::attestation::is_protected_rpc_envelope(
                params.interface_id, params.method_id, params.protocol_version))
        {
            CO_AWAIT child_service::post(std::move(params));
            CO_RETURN;
        }

        auto service = get_attestation_service();
        if (!protected_rpc_enabled() || !service)
            CO_RETURN;

        auto request = canopy::security::attestation::unprotect_post_request(*service, params);
        if (!request.accepted)
        {
            RPC_ERROR(
                "protected RPC unprotect post request failed: code={} ({}) reason={}",
                request.error.error_code,
                rpc::error::to_string(request.error.error_code),
                request.error.reason);
            CO_RETURN;
        }

        CO_AWAIT child_service::post(std::move(request.value.params));
        CO_RETURN;
    }

    // Handles inbound try_cast metadata requests with the same protected
    // payload and route-admission checks as other control messages.
    CORO_TASK(standard_result)
    enclave_service::try_cast(try_cast_params params)
    {
        // try_cast exposes runtime interface metadata. Treat it like other
        // control messages: unwrap protected payloads when present, reject unknown
        // typed payloads, and sanitise the public result before returning.
        const bool protected_payload
            = canopy::security::attestation::is_protected_rpc_payload(params.payload, params.protocol_version);
        if (protected_payload)
        {
            auto service = get_attestation_service();
            if (!protected_rpc_enabled() || !service)
                CO_RETURN standard_result{rpc::error::ZONE_NOT_SUPPORTED(), {}};

            auto request = canopy::security::attestation::unprotect_try_cast_request(*service, params);
            if (!request.accepted)
            {
                RPC_ERROR(
                    "protected RPC unprotect try_cast failed: code={} ({}) reason={}",
                    request.error.error_code,
                    rpc::error::to_string(request.error.error_code),
                    request.error.reason);
                CO_RETURN standard_result{request.error.error_code, {}};
            }
            params = std::move(request.value.params);
        }
        else if (protected_rpc_enabled() && params.payload)
        {
            // try_cast can legitimately expose newer interface/schema metadata.
            // Do not turn an unknown fingerprint into a blacklist signal.
            CO_RETURN standard_result{rpc::error::INVALID_VERSION(), {}};
        }

        auto route_result
            = CO_AWAIT ensure_existing_reference_route_allowed(params.caller_zone_id, "inbound try_cast caller");
        if (route_result.error_code != rpc::error::OK())
            CO_RETURN route_result;

        if (!protected_payload && protected_rpc_enabled() && get_security_context(params.caller_zone_id))
        {
            RPC_WARNING("plaintext try_cast rejected for protected route {}", rpc::to_string(params.caller_zone_id));
            CO_RETURN standard_result{rpc::error::FRAUDULANT_REQUEST(), {}};
        }

        auto result = CO_AWAIT child_service::try_cast(std::move(params));
        CO_RETURN sanitise_public_control_result(std::move(result), "try_cast");
    }

    // Protects outbound send traffic at the service/transport boundary and
    // unwraps the protected send response on return.
    CORO_TASK(send_result)
    enclave_service::outbound_send(
        send_params params,
        std::shared_ptr<transport> transport)
    {
        // Outbound send is the last point before a request leaves this service
        // through a transport. If protected RPC is enabled, encrypt here so
        // intermediate zones only see routeable carrier fields.
        auto service = get_attestation_service();
        if (!protected_rpc_enabled() || !service)
        {
            CO_RETURN CO_AWAIT child_service::outbound_send(std::move(params), std::move(transport));
        }
        if (!transport)
        {
            CO_RETURN send_result{rpc::error::TRANSPORT_ERROR(), {}, {}};
        }

        auto context = find_security_context_for_protected_call(params.caller_zone_id, params.remote_object_id.as_zone());
        if (!context)
        {
            // Do not silently downgrade a protected route to plaintext. If the
            // destination has not completed attestation, fail the call.
            CO_RETURN send_result{rpc::error::ZONE_NOT_SUPPORTED(), {}, {}};
        }

        auto request = canopy::security::attestation::protect_send_request(*service, *context, std::move(params));
        if (!request.accepted)
        {
            RPC_ERROR(
                "protected RPC protect send request failed: code={} ({}) reason={}",
                request.error.error_code,
                rpc::error::to_string(request.error.error_code),
                request.error.reason);
            CO_RETURN send_result{request.error.error_code, {}, {}};
        }

        auto outer_request = request.value.params;
        auto outer_response = CO_AWAIT child_service::outbound_send(std::move(request.value.params), std::move(transport));
        // The response must authenticate against the exact public request carrier
        // and request counter created above. This catches swapped or replayed
        // protected responses.
        auto response = canopy::security::attestation::unprotect_send_response(
            *service, request.value.context, outer_request, request.value.request_counter, std::move(outer_response));
        if (!response.accepted)
        {
            RPC_ERROR(
                "protected RPC unprotect send response failed: code={} ({}) reason={}",
                response.error.error_code,
                rpc::error::to_string(response.error.error_code),
                response.error.reason);
            CO_RETURN send_result{response.error.error_code, {}, {}};
        }

        CO_RETURN std::move(response.value);
    }

    // Protects outbound one-way post traffic before it reaches the next
    // transport hop.
    CORO_TASK(void)
    enclave_service::outbound_post(
        post_params params,
        std::shared_ptr<transport> transport)
    {
        // Outbound post is protected like send, but with no response to unwrap.
        // Missing context means drop/fail closed rather than leaking plaintext.
        auto service = get_attestation_service();
        if (!protected_rpc_enabled() || !service)
        {
            CO_AWAIT child_service::outbound_post(std::move(params), std::move(transport));
            CO_RETURN;
        }
        if (!transport)
            CO_RETURN;

        auto context = find_security_context_for_protected_call(params.caller_zone_id, params.remote_object_id.as_zone());
        if (!context)
            CO_RETURN;

        auto request = canopy::security::attestation::protect_post_request(*service, *context, std::move(params));
        if (!request.accepted)
        {
            RPC_ERROR(
                "protected RPC protect post request failed: code={} ({}) reason={}",
                request.error.error_code,
                rpc::error::to_string(request.error.error_code),
                request.error.reason);
            CO_RETURN;
        }

        CO_AWAIT child_service::outbound_post(std::move(request.value.params), std::move(transport));
        CO_RETURN;
    }

    // Protects outbound try_cast metadata when an attested context exists for
    // the destination route.
    CORO_TASK(standard_result)
    enclave_service::outbound_try_cast(
        try_cast_params params,
        std::shared_ptr<transport> transport)
    {
        // Interface discovery is metadata, but it still identifies application
        // interface types. Protect it when a route security_context exists.
        auto service = get_attestation_service();
        if (!protected_rpc_enabled() || !service)
        {
            CO_RETURN CO_AWAIT child_service::outbound_try_cast(std::move(params), std::move(transport));
        }
        if (!transport)
        {
            CO_RETURN standard_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        auto context = find_security_context_for_protected_call(params.caller_zone_id, params.remote_object_id.as_zone());
        if (!context)
        {
            CO_RETURN standard_result{rpc::error::ZONE_NOT_SUPPORTED(), {}};
        }

        auto request = canopy::security::attestation::protect_try_cast_request(
            *service, *context, std::move(params), get_default_encoding());
        if (!request.accepted)
        {
            RPC_ERROR(
                "protected RPC protect try_cast failed: code={} ({}) reason={}",
                request.error.error_code,
                rpc::error::to_string(request.error.error_code),
                request.error.reason);
            CO_RETURN standard_result{request.error.error_code, {}};
        }

        auto result = CO_AWAIT child_service::outbound_try_cast(std::move(request.value.params), std::move(transport));
        CO_RETURN sanitise_public_control_result(std::move(result), "outbound_try_cast");
    }

    // Admits and optionally protects outbound add_ref before a remote interface
    // reference is made visible outside this service.
    CORO_TASK(standard_result)
    enclave_service::outbound_add_ref(
        add_ref_params params,
        std::shared_ptr<transport> transport)
    {
        // Outbound add_ref is not the trust synchronization point. The receiver
        // decides whether the referenced route is admitted before it exposes the
        // reference locally. This side only protects the control payload if keys
        // already exist, then sends it onward.
        auto route_zone_id = outbound_reference_route_zone(params.remote_object_id, transport);

        auto service = get_attestation_service();
        if (protected_rpc_enabled() && service && route_zone_id)
        {
            auto context = get_security_context(*route_zone_id);
            if (context)
            {
                // Encrypt the full add_ref control payload, including the private
                // object id. Public AAD keeps only the destination zone route.
                auto request = canopy::security::attestation::protect_add_ref_request(
                    *service, *context, std::move(params), get_default_encoding());
                if (!request.accepted)
                {
                    RPC_ERROR(
                        "protected RPC protect add_ref failed: code={} ({}) reason={}",
                        request.error.error_code,
                        rpc::error::to_string(request.error.error_code),
                        request.error.reason);
                    CO_RETURN standard_result{request.error.error_code, {}};
                }
                params = std::move(request.value.params);
            }
        }

        auto result = CO_AWAIT child_service::outbound_add_ref(std::move(params), std::move(transport));
        CO_RETURN sanitise_public_control_result(std::move(result), "outbound_add_ref");
    }

    // Protects outbound release for an already-admitted reference route.
    CORO_TASK(standard_result)
    enclave_service::outbound_release(
        release_params params,
        std::shared_ptr<transport> transport)
    {
        // Release is sent to the peer that owns the corresponding reference
        // state. The receiver validates/adopts route state before mutating its
        // local counts; this side only protects the payload when keys already
        // exist.
        auto route_zone_id = outbound_reference_route_zone(params.remote_object_id, transport);

        auto service = get_attestation_service();
        if (protected_rpc_enabled() && service && route_zone_id)
        {
            auto context = get_security_context(*route_zone_id);
            if (context)
            {
                // Protect release for routes with established keys so reference
                // counts cannot be manipulated by an intermediate.
                auto request = canopy::security::attestation::protect_release_request(
                    *service, *context, std::move(params), get_default_encoding());
                if (!request.accepted)
                {
                    RPC_ERROR(
                        "protected RPC protect release failed: code={} ({}) reason={}",
                        request.error.error_code,
                        rpc::error::to_string(request.error.error_code),
                        request.error.reason);
                    CO_RETURN standard_result{request.error.error_code, {}};
                }
                params = std::move(request.value.params);
            }
        }

        auto result = CO_AWAIT child_service::outbound_release(std::move(params), std::move(transport));
        CO_RETURN sanitise_public_control_result(std::move(result), "outbound_release");
    }

    // Protects outbound object_released notifications for the caller route that
    // owns the corresponding remote reference.
    CORO_TASK(void)
    enclave_service::outbound_object_released(
        object_released_params params,
        std::shared_ptr<transport> transport)
    {
        // object_released travels back toward the caller route. Validate that
        // route first, then protect the lifetime notification if keys exist.
        std::optional<rpc::destination_zone> caller_zone_id;
        if (transport)
        {
            caller_zone_id = params.caller_zone_id;
        }

        auto service = get_attestation_service();
        if (protected_rpc_enabled() && service && caller_zone_id)
        {
            auto context = get_security_context(*caller_zone_id);
            if (context)
            {
                auto request = canopy::security::attestation::protect_object_released_request(
                    *service, *context, std::move(params), get_default_encoding());
                if (!request.accepted)
                {
                    RPC_ERROR(
                        "protected RPC protect object_released failed: code={} ({}) reason={}",
                        request.error.error_code,
                        rpc::error::to_string(request.error.error_code),
                        request.error.reason);
                    CO_RETURN;
                }
                params = std::move(request.value.params);
            }
        }

        CO_AWAIT child_service::outbound_object_released(std::move(params), std::move(transport));
    }
}
