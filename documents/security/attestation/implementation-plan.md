<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Attestation Implementation Plan

## Purpose

A staged plan that turns the attestation design documents in this
directory into working code. Each phase is mergeable: at every phase
boundary the tree builds, existing tests pass, and the new functionality
is exercised by tests that can run on the available hardware.

The first production-shaped target is SGX/DCAP. The plan deliberately defers
anything that requires the IETF TLS attestation draft to stabilise. Phase 1
uses the fake backend with no SGX dependency; Phase 3 introduces Intel's
`sgx_ttls` helpers as the first concrete TLS-carried Evidence envelope. The
CMW-based in-process Evidence interface is in place from the start so the wire
format can be replaced later without re-shaping the rest of the code.

The SGX-first implementation must still keep the backend boundary portable.
Intel TDX, AMD SEV-SNP, and Arm TrustZone/PSA should be later L3 backend
additions with backend-specific policy, not changes to L4-L8.

## Design Inputs

This plan implements:

- [Overview](overview.md)
- [Attestation Backends](attestation-backends.md)
- [Wire Format](wire-format.md)
- [Protected RPC Envelope](protected-rpc-envelope.md)
- [Zone Address Validation](zone-address-validation.md)
- [Back-Channel Context](back-channel-context.md)
- [Failure Policy](failure-policy.md)
- [DCAP Operations](dcap-operations.md)

If a phase below conflicts with one of those documents, the documents are
authoritative and the plan must be amended.

## Implementation Checkpoint - 2026-05-15

The current tree contains a no-SGX proof of concept plus the first protected
RPC envelope implementation. It is a vertical development slice across the
fake backend, a first L4 `attestation_service`, an attestation stream
decorator, normal RPC traffic over that decorator, AES-GCM envelope helpers,
`rpc::enclave_service` send/post hooks, encrypted `try_cast`, `add_ref`,
`release`, `object_released`, and endpoint-originated `transport_down` payload
wrapping, and the first route-state gate for reference-control messages. It
now also has the first generated-IDL
service-level route
attestation handshake payload carried by `i_marshaller::handshake()` and
SGX-sim host integration coverage that drives that handshake through a real
transport from an unknown-route `add_ref`. It is **not** completion of Phase 1
or Phase 2 as written below, because the protected envelope still needs
real SGX/DCAP backend wiring, the later carrier work, and the
policy-hardening work described in the remaining phases.

### Implemented

- `c++/security/attestation/` now builds a `security_attestation` target with
  backend-neutral passive types, a development fake backend, a null backend,
  an initial fail-closed SGX EPID backend, and a small configured-backend
  factory:
  - `include/security/attestation/types.h`
  - `include/security/attestation/backend_factory.h`
  - `include/security/attestation/fake_backend.h`
  - `include/security/attestation/null_backend.h`
  - `include/security/attestation/sgx_epid_backend.h`
  - `include/security/attestation/service.h`
  - `src/backend_factory.cpp`
  - `src/fake_backend.cpp`
  - `src/null_backend.cpp`
  - `src/sgx_epid_backend.cpp`
  - `src/service.cpp`
- The fake backend produces CMW-like fake Evidence using
  `media_type == "application/canopy-fake-evidence"` and
  `content_format == "canopy.fake.v1"`.
- The null backend is an explicit no-Evidence backend for demos and browser or
  host clients. It reports `backend_id == "null"`,
  `security_level == none`, does not produce accepted local Evidence, and
  rejects Evidence verification. A peer without Evidence is accepted only when
  policy sets both `require_peer_evidence == false` and
  `allow_unattested_peer == true`.
- `CANOPY_ATTESTATION_BACKEND` currently accepts `FAKE`, `SGX_SIM`,
  `SGX_EPID`, and `NULL` and defines the backend selected by
  `make_configured_attestation_service_options(...)`.
- `interfaces/attestation/sgx_epid_protocol.idl` defines the first legacy SGX
  EPID CMW payload:
  - `sgx_epid_report_binding`;
  - `sgx_epid_ias_report`;
  - `sgx_epid_quote_evidence`.
  The EPID backend hashes the canonical `sgx_epid_report_binding` to produce
  the report_data value that a real quote provider must embed in the SGX quote.
  The default backend has no provider or verifier and fails closed with
  `content_format == "canopy.sgx-epid-unavailable.v1"`. Tests exercise the
  provider/verifier seams with injected fakes; no test claims production EPID
  trust on non-SGX hardware.
- `SGX_SIM` is a distinct SGX-simulation policy/profile. Host-only builds still
  use a signed development fallback. Intel SGX simulation enclave builds now
  have two IDL-defined SGX-report paths:
  - `sgx_sim_report_evidence` is the standalone/self-test payload with
    transcript-bound `sgx_report_data_t`, raw `sgx_target_info_t`, raw
    `sgx_report_t`, and a development HMAC over the payload. The producer must
    verify this self-targeted report with `sgx_verify_report` before sending it.
  - `sgx_sim_local_attestation_challenge` plus
    `sgx_sim_local_attestation_report` is the route-handshake payload pair. The
    verifier sends its `sgx_target_info_t` as a backend-neutral CMW
    `verifier_challenge`, and the peer returns a report targeted to that
    verifier.
  Both paths remain non-production `security_level::simulation` evidence.
- Test-only SGX SIM runtime coverage now lives in dedicated attestation test
  infrastructure rather than in the io_uring test interface:
  - `c++/tests/idls/attestation_test/attestation_test.idl`
  - `c++/tests/attestation_test_enclave/`
  - `c++/tests/attestation_test_host/`
  The test enclave calls the SGX-sim backend inside the enclave, observes
  `canopy.sgx-sim-report.v1` evidence, checks that report and target-info bytes
  are present, verifies the evidence before returning success to the host
  gtest, and also exercises peer-targeted local-report challenge/response
  between two test enclaves.
- Fake Evidence verification checks:
  - policy permission for development Evidence;
  - required backend id;
  - minimum security level;
  - backend id;
  - attested enclave id and zone id;
  - transcript id;
  - nonce;
  - fake development signature.
- `c++/streaming/attestation/` now builds a `streaming_attestation` target:
  - `include/streaming/attestation/stream.h`
  - `src/stream.cpp`
- `streaming::attestation::stream` wraps a generic
  `std::shared_ptr<streaming::stream>`. In production-shaped direct paths this
  can wrap a secure stream; the current tests wrap SPSC streams so the POC can
  run without TLS, SGX, or DCAP.
- `streaming::attestation::stream` now calls `attestation_service` for local
  Evidence production, peer Evidence verification, policy checks, and session
  creation. It no longer talks directly to an attestation backend.
- The first `attestation_service` owns:
  - local enclave and zone identity;
  - local attestation policy;
  - selected backend;
  - established `security_context` records keyed by enclave-pair session id;
  - OpenSSL HKDF-SHA256 root-secret extraction for each established session;
  - AEAD key derivation scoped by session id, caller zone, destination zone,
    and direction;
  - per-derived-key monotonic send and receive counters.
- Key-derivation inputs now use a named Canopy Attestation v1 canonical KDF
  encoding instead of anonymous serializer bytes. This encoding uses
  length-prefixed fields, big-endian integers, explicit labels, and the fixed
  canonical encoding domain `Canopy-Attestation-v1`.
- The current fake and SGX-simulation paths may use a deterministic
  development shared secret when a real key-exchange secret is not supplied.
  This is only for no-SGX developer tests and does not provide production
  confidentiality against an on-path attacker.
- Hardware-grade verdicts are not allowed to fall back to that development
  secret. `attestation_service::establish_session` requires explicit shared
  secret bytes for `hardware_legacy` and `hardware` sessions so SGX EPID/DCAP
  cannot accidentally derive AEAD keys only from public transcript data.
- The current development handshake uses four in-tunnel frame kinds:
  - `client_hello_attest`
  - `server_hello_attest`
  - `evidence`
  - `evidence_verdict`
- The handshake supports both:
  - mutual fake attestation;
  - an unattested client verifying an attested server.
- `streaming::attestation::stream` exposes a `security_context()` after the
  handshake and delegates normal `send` / `receive` to the wrapped stream. The
  `security_context` is now created and cached by `attestation_service`.
- `streaming::attestation::stream` implements
  `canopy::security::attestation::security_context_source`, a small
  header-only interface that lets enclave-owned code discover attested streams
  without depending on the concrete decorator type.
- `rpc::i_marshaller` now has a route-addressed `handshake()` operation for
  service-level attestation and key exchange. Base service and transport
  implementations route it generically.
- `interfaces/rpc/rpc_types.idl` remains limited to generic RPC carriers such
  as `rpc::encrypted_payload`.
- `interfaces/attestation/route_attestation_protocol.idl` defines the
  first service-level route attestation payloads:
  - `attestation_identity`;
  - `attestation_cmw`;
  - `route_attestation_handshake_request`;
  - `route_attestation_handshake_response`.
  These are generated RPC/YAS types and their generated fingerprints are used
  as the `handshake_params::type_id` and `handshake_result::type_id` values.
  This is the general handshake versioning rule:
  every handshake blob is identified by an IDL fingerprint and the selected
  payload encoding. Protocol-specific handshakes should live in their own IDL
  files with an `[inline] namespace vN`, so SGX, TDX, SEV-SNP,
  TrustZone/PSA, certificate-authenticated routes, and future route-security
  variants can evolve without growing `rpc_types.idl`. Human-readable names
  remain diagnostic labels only.
  MCP and A2A are consumers of generated IDL descriptions, fingerprints, and
  schema metadata; they are not attestation protocol families.
- `route_attestation_handshake_request` can now carry an optional
  backend-neutral `verifier_challenge` CMW blob. SGX SIM local attestation uses
  this to send `sgx_target_info_t` from the verifier to the peer, while keeping
  the SGX-specific payload schema in `interfaces/attestation/sgx_sim_protocol.idl`.
  The state transition and sequence diagrams for these blobs are in
  [overview.md](overview.md#route-sign-on-state).
- `attestation_service` now exposes OpenSSL/SGXSSL-backed nonce generation for
  route-attestation Evidence bindings. The POC uses 32-byte nonces and rejects
  malformed service-level request/response nonces before Evidence
  verification.
- `security_context`, fake Evidence, session ids, and KDF input contexts bind
  the attested identity pair, transcript id, nonce material, backend result,
  session epoch, and protected-RPC direction. Future security-critical
  transcript hashes should bind the exact transmitted typed blobs:
  `(type_id, payload_encoding, payload bytes)` in protocol order.
- `rpc::enclave_service::handshake()` now handles local route-attestation
  requests. The first implementation:
  - deserializes bounded generated-YAS payloads;
  - verifies peer fake Evidence against claimed identity, transcript id, and
    nonce;
  - produces local Evidence for the response when policy requires it;
  - establishes and stores an attested `security_context` when peer Evidence is
    accepted;
  - marks routes `unattested_allowed` only when peer Evidence is absent, the
    local policy does not require it, and the local policy explicitly allows
    an unattested peer;
  - returns a structured rejection payload for malformed or policy-rejected
    handshakes.
- `rpc::enclave_service` stores attestation `security_context` records keyed by
  the attested peer route zone through a single `route_attestation_state` map.
  The `security_context` is optional inside the route state and is only
  authoritative when the route state is `attested`. For a direct stream that
  peer is the adjacent zone; for routed end-to-end protection it must be the
  final destination zone. The base `rpc::service` and generic
  `rpc::stream_transport::transport` remain attestation-neutral.
- Route-attestation transcript ids are allocated from the destination route's
  `route_attestation_state`, not from a service-global counter. This keeps
  transcript uniqueness, replay reasoning, and audit state scoped to the
  route/session domain that owns the handshake.
- Outbound and inbound `add_ref` route admission now claims the route
  handshake atomically under the route-state mutex: it evaluates the current
  state, reserves the next transcript id, and publishes `handshaking` before
  any Evidence generation, serialization, or transport `handshake()` coroutine
  work begins. No route-state mutex is held across `CO_AWAIT`.
- `route_attestation_state` now has a pure
  `evaluate_route_attestation_state(...)` decision helper. It maps route state
  to `allow`, `start_handshake`, `wait_for_handshake`, or `reject`; host tests
  cover the state matrix.
- `try_cast_params`, `add_ref_params`, `release_params`,
  `object_released_params`, `transport_down_params`, and the streaming
  transport wire structs now carry `payload_type_id` plus `payload` fields so
  encrypted reference/control payloads can reuse the existing polymorphic
  marshaller shape. `try_cast`, `add_ref`, `release`, `object_released`, and
  endpoint-originated `transport_down` now use those fields for protected
  payloads.
- `rpc::enclave_service` has opt-in reference route enforcement:
  - `set_add_ref_attestation_required`;
  - `set_route_unattested_allowed`;
  - inbound `add_ref` checks the route in `remote_object_id.as_zone()`;
  - outbound `add_ref` checks the adjacent transport zone;
  - inbound `try_cast` checks the caller route without starting a new
    handshake;
  - outbound `try_cast` uses the protected destination route context;
  - inbound `release` checks the caller route without starting a new
    handshake;
  - outbound `release` checks the adjacent transport zone;
  - inbound `object_released` checks the released-object owner route without
    starting a new handshake;
  - outbound `object_released` checks the recipient caller route;
  - inbound `transport_down` unwraps protected payloads when present but still
    accepts empty plaintext route-layer notifications from intermediates;
  - unknown routes start the route-addressed service-level `handshake()` path;
  - successful Evidence verification marks the route `attested`;
  - accepted no-Evidence policy marks the route `unattested_allowed`;
  - failed or malformed handshakes mark the route `failed` and the call fails
    closed.
- `try_cast`, `add_ref`, `release`, `object_released`, and `transport_down`
  protected payloads are implemented in the L7 envelope helpers:
  - outbound `rpc::enclave_service::outbound_try_cast` wraps the requested
    interface id and object id when the destination route has an attested
    `security_context`;
  - inbound `rpc::enclave_service::try_cast` unwraps protected payloads before
    checking the existing caller route state;
  - outbound `rpc::enclave_service::outbound_add_ref` wraps when the adjacent
    route has an attested `security_context`;
  - inbound `rpc::enclave_service::add_ref` unwraps protected payloads before
    route validation;
  - outbound `rpc::enclave_service::outbound_release` wraps when the adjacent
    route has an attested `security_context`;
  - inbound `rpc::enclave_service::release` unwraps protected payloads before
    checking the existing caller route state;
  - outbound `rpc::enclave_service::outbound_object_released` wraps optimistic
    reference notifications when the recipient route has an attested
    `security_context`;
  - inbound `rpc::enclave_service::object_released` unwraps protected payloads
    before checking the owner route state;
  - inbound `rpc::enclave_service::transport_down` unwraps protected endpoint
    notifications when present and leaves the empty route-layer form available
    for intermediates;
  - `object_stub::add_ref` now calls the service `outbound_add_ref` virtual for
    outcall add-ref messages, so initial connection add-refs pass through the
    enclave policy hook;
  - `service::release_local_stub` now calls the service
    `outbound_object_released` virtual, so optimistic reference notifications
    generated inside an enclave pass through the enclave policy hook;
  - the current transport still needs visible `build_out_param_channel` for
    route construction, so that field is public but AEAD-bound and repeated in
    encrypted plaintext until the route-control refactor happens. The
    requesting zone and route-direction bits are likewise live routing inputs;
  - the current transport/passthrough release path still exposes
    `release_options` for lifetime accounting, so that field is public but
    AEAD-bound and repeated in encrypted plaintext. The `add_ref` optimistic
    bit, `release_options`, and `object_released` passthrough accounting must
    be refactored as one route token/state-table change before
    shared-vs-optimistic intent can be hidden.
- Protected envelopes now clear the public object id in outer
  `remote_object_id` values. Intermediates receive the destination zone needed
  for routing; endpoints recover the full remote object from the decrypted
  plaintext.
- Protected RPC treats back-channel vectors as mutable public metadata:
  intermediates may append entries, the vectors are not included in AEAD
  associated data, and inbound protected requests/responses pass the received
  outer vectors onward.
- Protected `send` keeps positive application-domain result codes inside the
  encrypted response. The public carrier status is now limited to `OK()` or
  built-in `rpc::error::*` control values; a positive public carrier status is
  rejected as a protocol error.
- `rpc::enclave_service` now sanitises `try_cast`, `add_ref`, and `release`
  control results so non-RPC positive values are not returned on those public
  control paths.
- Protected RPC runtime tests now observe public response statuses for
  protected `send`, `try_cast`, and `add_ref`, and fail if a non-RPC positive
  status becomes visible. A direct enclave-service regression test also forces
  positive statuses through protected `try_cast`, `add_ref`, and the one-way
  `release` outbound hook and verifies they are converted to
  `PROTOCOL_ERROR()`.
- Stream transport now sanitises routed `handshake` response statuses so
  positive application-domain values cannot be emitted as public handshake
  results. Runtime tests observe stream sign-on, generated route-attestation
  `handshake` request/response messages, and stream close messages; they
  assert that setup/handshake statuses remain RPC control statuses and that
  routed handshakes use the expected generated route-attestation type ids.
- Stream transport now sanitises `get_new_zone_id` carrier and response
  statuses so positive application-domain values cannot be emitted as public
  allocator results. Runtime tests observe `get_new_zone_id` request/response
  messages and the intentionally plaintext route-layer `transport_down` form
  used for intermediate route-liveness notifications.
- Stream sign-on remains a narrow transport-adjacency step. The setup
  descriptors in `init_client_channel_send` are public control data until a
  dedicated setup envelope exists. A direct peer-to-peer attested stream may
  publish its already-established `security_context` to the service before RPC
  routing starts; otherwise an unattested sign-on must be explicitly allowed by
  policy, as with development JavaScript/demo clients. Routed references
  received during sign-on are still validated later by `add_ref` against the
  referenced zone, not trusted solely because the adjacent stream connected.
- Service-level route handshakes now complete against the transcript claim
  that reserved the route. A delayed handshake failure or success may only
  update the route state while the route is still handshaking for that
  transcript; if another path has already admitted the route, the stale
  completion leaves the newer state intact. Runtime coverage blocks an
  `add_ref` handshake, publishes a superseding attested context, then releases
  the stale handshake to fail and verifies the route remains admitted.
- Inbound route-handshake state updates are now fail-closed without being
  destructive to better state. A failed inbound handshake may mark an unknown
  route failed, but it cannot overwrite an already admitted route or an
  in-progress outbound transcript. A later no-Evidence inbound handshake also
  cannot downgrade an attested route to `unattested_allowed`; re-attestation or
  downgrade needs an explicit future protocol. Runtime coverage verifies both
  a failed inbound handshake and an allowed no-Evidence inbound handshake
  preserve the existing attested context.
- Connection setup now carries an explicit `connection_settings::encoding_type`
  from the caller's service default into `transport::connect()`,
  `inner_connect()`, stream `init_client_channel_send`, `attach_remote_zone()`,
  and `create_child_zone()`. `transport::connect()` rejects `not_set` before
  calling `inner_connect()`: a transport must not guess a serialization format.
  Child services copy this explicit connection encoding into their own default
  encoding so a parent can deliberately instantiate a child zone with the
  parent's chosen format.
- `rpc::error` now owns the public-control status predicate and sanitiser.
  `rpc::transport` final control methods use it for public `handshake()` and
  `get_new_zone_id()` results for all transport implementations. Stream
  transport, protected RPC response validation, and enclave-service control
  result handling share that same error-code helper instead of copying the
  rule locally. This gives local and non-stream coroutine transports the same
  "no positive public control status" guardrail as stream transports, even
  when they do not use the stream envelope.
- Current runtime coverage now includes focused non-stream control-status
  regressions, including the real local parent-side `get_new_zone_id` path.
  The marked enclave-local wrapper path exercises generated `send`, generated
  `post`, `try_cast`, `add_ref`, `release`, `object_released`, and plaintext
  route-layer `transport_down`. `rpc::local::parent_transport` and
  `rpc::local::child_transport` can be built for enclaves and can link zones
  within the same enclave. Their outbound methods call the peer transport's
  inbound marshaller handlers directly, so there is no stream envelope, stream
  sign-on, or stream close message on that path. The service-level
  `rpc::enclave_service` hooks remain the intended protection boundary.
- The SGX coroutine host transport uses a fixed YAS binary encoding only for
  the pre-service ECALL bootstrap `init_request`, because that blob is decoded
  before an enclave-side service exists and before a service default encoding
  can be consulted. Once the RPC transport link is live, route handshakes and
  protected payload carriers use the negotiated/request/service encoding paths
  described above.
- The non-coroutine SGX marshal-test DTOs and the current C ABI bridge now
  preserve the explicit connection encoding plus typed-control
  `payload_type_id`, `payload_encoding`, and payload bytes for `try_cast`,
  `add_ref`, `release`, `object_released`, and `transport_down`. This is a
  compatibility guard so older compiled paths do not silently discard
  protected control payloads; it is not a full C ABI security-audit pass.
- `documents/security/attestation/intermediate-visibility-audit.md` records
  the current field-by-field visibility decision. The audit confirms that
  intermediate transports and passthroughs need route zones, carrier metadata,
  and current route-control fields, including the `add_ref` optimistic flag
  and `release_options` used for passthrough lifetime accounting. They do not
  need application object ids, real application interface ids, real application
  method ids, or payload bytes.
- `rpc::encrypted_payload` is pinned in `interfaces/rpc/rpc_types.idl` with:
  - public `session_id`;
  - public `session_epoch`;
  - public `e2e_counter`;
  - encrypted `payload`;
  - AES-GCM `authentication_tag`.
- `c++/security/attestation/include/security/attestation/protected_rpc.h` and
  `c++/security/attestation/src/protected_rpc.cpp` implement the first L7
  protected-envelope helpers:
  - `protect_send_request` / `unprotect_send_request`;
  - `protect_send_response` / `unprotect_send_response`;
  - `protect_post_request` / `unprotect_post_request`;
  - outer `interface_id == rpc::id<rpc::encrypted_payload>::get(version)`;
  - outer `method_id == 0`;
  - serialization of the outer `encrypted_payload` carrier using the agreed
    public carrier encoding (`encoding_type` for `send`/`post`,
    `payload_encoding` beside `payload_type_id` for typed payload carriers);
  - typed-carrier `payload_encoding == rpc::encoding::not_set` values resolved
    from the live service default before outbound transport activity, not from
    a compile-time fallback;
  - handshake response payloads encoded with the request `payload_encoding`,
    with no independent reply encoding field;
  - bounded canonical plaintext and AAD encoding using big-endian integers and
    length-prefixed fields;
  - AES-256-GCM encryption through OpenSSL/SGXSSL-compatible EVP APIs;
  - replay checks that accept receive counters only after successful
    authentication and plaintext validation.
- `rpc::enclave_service` now owns the protected-RPC integration point:
  - `set_attestation_service`;
  - `set_protected_rpc_enabled`;
  - inbound `send` / `post` unwrap;
  - outbound `send` / `post` wrap;
  - protected `send` response wrap/unwrap.
- `transport_sgx_coroutine_enclave` links the enclave attestation library so
  the protected-envelope helpers compile in SGX-sim and fake-SGX enclave
  builds. The base `rpc::service` remains free of attestation-specific logic.
- Raw stream POC coverage exists in
  `c++/streaming/attestation/tests/attestation_stream_test.cpp`.
- Attestation service coverage exists in
  `c++/security/attestation/tests/attestation_service_test.cpp`. It checks:
  - session ids are scoped to enclave pairs rather than zone pairs;
  - a stable golden vector for the fake-session AEAD key and nonce prefix;
  - key derivation agreement between both peers;
  - direction and zone-pair separation;
  - nonce construction from the per-key nonce prefix plus counter;
  - monotonic counter allocation;
  - replay/out-of-order receive-counter rejection;
  - configured counter exhaustion.
  - route attestation state decisions for unknown, handshaking, failed,
    explicitly allowed unattested, and attested routes.
  - OpenSSL/SGXSSL-backed 32-byte attestation nonce generation.
  - generated type ids and round-tripping for route-attestation handshake
    request and response payloads using every agreed payload encoding compiled
    into the current build: YAS, protocol buffers, and/or nanopb.
  - protected send request/response wrapping and unwrapping;
  - protected `try_cast`, `add_ref`, `release`, `object_released`, and
    `transport_down` wrapping and unwrapping;
  - protected `try_cast`, `add_ref`, `release`, `object_released`, and
    `transport_down` typed payloads preserve and authenticate their public
    `payload_encoding` while carrying encrypted payload bytes in the caller's
    agreed encoding;
  - positive application-domain `send` result codes are recovered only after
    decrypting the protected response;
  - positive public carrier statuses on protected `send` responses are
    rejected as protocol errors;
  - protected outer route carriers hide object ids while preserving
    decrypt-time object reconstruction;
  - tampered protected ciphertext rejection.
- RPC-level POC coverage exists in
  `c++/tests/test_host/attested_streaming_transport_poc_suite.cpp`.
  It proves that generated RPC traffic can run over `rpc::stream_transport`
  after the attestation stream handshake in two cases:
  - mutually attested SPSC peer services;
  - unattested client to attested server.
- Service-level route-attestation integration coverage also exists in
  `c++/tests/test_host/attested_streaming_transport_poc_suite.cpp`. It builds
  two `rpc::enclave_service` instances, drives an unknown-route `add_ref`
  through `rpc::stream_transport`, and proves that:
  - mutual fake Evidence marks both route-state maps `attested` with
    established security contexts;
  - an explicitly permitted no-Evidence client marks the responder route
    `unattested_allowed` while the client still verifies the attested server.
- Generated-RPC protected-runtime coverage exists in
  `c++/tests/test_host/attested_streaming_transport_poc_suite.cpp`. It builds
  two `rpc::enclave_service` instances with pre-established fake security
  contexts, drives generated `send`, `[post]`, and `add_ref` calls through
  `rpc::stream_transport`, drives a generated `try_cast`, observes cleanup
  `release` traffic, and verifies that:
  - inbound generated `send` traffic arrives as an encrypted outer
    `rpc::encrypted_payload`;
  - protected `send` responses carry encrypted payloads back to the caller;
  - generated `[post]` traffic also arrives as an encrypted outer payload;
  - generated `try_cast` traffic carries an encrypted payload carrier;
  - generated `add_ref` traffic carries an encrypted payload carrier;
  - generated `release` traffic carries an encrypted payload carrier;
  - no protected message exposes a nonzero public object id;
  - no generated `send`, response, `[post]`, `try_cast`, `add_ref`, or
    `release` traffic is observed in plaintext while protection is enabled.
  - no positive non-RPC public status is observed on protected `send`,
    `try_cast`, or `add_ref` responses.
  - positive protected control statuses returned by a transport for
    `try_cast`, `add_ref`, and `release` are sanitised to `PROTOCOL_ERROR()`
    by `rpc::enclave_service`.
  - stream sign-on messages are observed during generated RPC connection
    setup, stream close messages are observed during cleanup, and no non-RPC
    public status is observed on setup/handshake/control response paths.
  - service-level route handshakes use the generated
    `route_attestation_handshake_request` and
    `route_attestation_handshake_response` type ids.

### Verified

- `cmake --preset Debug`
- `cmake --preset Debug_Coroutine`
- `cmake --preset Debug_Coroutine_SGX_Sim`
- `cmake --build build_debug --target attestation_service_test`
- `cmake --build build_debug_coroutine --target attestation_service_test rpc_test`
- `cmake --build build_debug_coroutine_sgx_sim --target attestation_service_test rpc_test`
- `build_debug/output/attestation_service_test`
- `build_debug_coroutine/output/attestation_service_test`
- `build_debug_coroutine/output/rpc_test --gtest_filter=attested_streaming_transport_poc_test/* --gtest_brief=1`
- `build_debug_coroutine_sgx_sim/output/attestation_service_test`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_filter=attested_streaming_transport_poc_test/* --gtest_brief=1`
- `cmake --preset Debug_Coroutine`
- `cmake --build build_debug_coroutine --target rpc_test attestation_stream_test`
- `build_debug_coroutine/output/attestation_stream_test`
- `build_debug_coroutine/output/rpc_test --gtest_filter=attested_streaming_transport_poc_test/* --gtest_brief=1`
- `ctest --test-dir build_debug_coroutine -R attested_streaming_transport_poc_test --output-on-failure`
- `ctest --test-dir build_debug_coroutine -R attestation_stream_test --output-on-failure`
- `cmake --build build_debug --target security_attestation`
- `cmake --build build_debug --target rpc security_attestation`
- `ctest --test-dir build_debug_coroutine -R 'attestation_stream_test|attested_streaming_transport_poc_test' --output-on-failure`
- `cmake --build build_debug --target attestation_service_test security_attestation`
- `build_debug/output/attestation_service_test`
- `ctest --test-dir build_debug -R attestation_service_test --output-on-failure`
- `cmake --build build_debug_coroutine --target attestation_service_test attestation_stream_test rpc_test`
- `ctest --test-dir build_debug_coroutine -R 'attestation_service_test|attestation_stream_test|attested_streaming_transport_poc_test' --output-on-failure`
- `cmake --build build_debug --target attestation_service_test`
- `build_debug/output/attestation_service_test`
- `cmake --build build_debug_coroutine --target attestation_service_test attestation_stream_test`
- `build_debug_coroutine/output/attestation_service_test`
- `build_debug_coroutine/output/attestation_stream_test`
- `cmake --build build_debug_sgx_sim --target security_attestation_enclave attestation_service_test`
- `build_debug_sgx_sim/output/attestation_service_test`
- `cmake --build build_debug_coroutine_sgx_sim --target security_attestation_enclave transport_sgx_coroutine_enclave attestation_service_test attestation_stream_test`
- `build_debug_coroutine_sgx_sim/output/attestation_service_test`
- `build_debug_coroutine_sgx_sim/output/attestation_stream_test`
- `cmake --build build_debug_coroutine_fake_sgx --target security_attestation_enclave transport_sgx_coroutine_enclave attestation_service_test attestation_stream_test`
- `build_debug_coroutine_fake_sgx/output/attestation_service_test`
- `build_debug_coroutine_fake_sgx/output/attestation_stream_test`
- `cmake --build build_debug --target attestation_service_test`
- `cmake --build build_debug_coroutine --target attestation_service_test`
- `build_debug/output/attestation_service_test`
- `build_debug_coroutine/output/attestation_service_test`
- `cmake --build build_debug_coroutine_sgx_sim --target transport_sgx_coroutine_enclave security_attestation_enclave`
- `cmake --build build_debug --target attestation_service_test`
- `build_debug/output/attestation_service_test`
- `cmake --build build_debug_coroutine --target attestation_service_test`
- `build_debug_coroutine/output/attestation_service_test`
- `cmake --build build_debug_coroutine_sgx_sim --target transport_sgx_coroutine_enclave security_attestation_enclave`
- `cmake --build build_debug_coroutine_fake_sgx --target transport_sgx_coroutine_enclave security_attestation_enclave`
- `cmake --build build_debug_coroutine_sgx_sim --target rpc_test`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_list_tests`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_filter=attested_streaming_transport_poc_test/*`
- `cmake --build build_debug_coroutine_sgx_sim --target rpc_test`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_filter=ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/*`
- `cmake --build build_debug_coroutine_fake_sgx --target rpc_test`
- `build_debug_coroutine_fake_sgx/output/rpc_test --gtest_filter=ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/*`
- `cmake --build build_debug_coroutine_sgx_sim --target rpc_test`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_filter=ProtectedRpcRuntime.*:ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/*`
- `cmake --build build_debug_coroutine_fake_sgx --target attestation_service_test`
- `build_debug_coroutine_fake_sgx/output/attestation_service_test --gtest_filter=AttestationService.ProtectsObjectReleasedRequest:AttestationService.ProtectsTransportDownRequest:AttestationService.ProtectsTryCastRequest:AttestationService.ProtectsReleaseRequest:AttestationService.ProtectedRequestsAllowMutablePublicBackChannels:AttestationService.ProtectsAddRefRequest:AttestationService.ProtectsSendRequestAndResponse:AttestationService.ProtectedSendRejectsTamperedCiphertext`
- `cmake --build build_debug_coroutine_fake_sgx --target rpc_test`
- `build_debug_coroutine_fake_sgx/output/rpc_test --gtest_filter=ProtectedRpcRuntime.*:ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/*`
- `cmake --build build_debug_coroutine_sgx_sim --target rpc_test attestation_service_test`
- `cmake --build build_debug_coroutine_fake_sgx --target rpc_test attestation_service_test`
- `build_debug_coroutine_sgx_sim/output/attestation_service_test --gtest_brief=1`
- `build_debug_coroutine_fake_sgx/output/attestation_service_test --gtest_brief=1`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_filter=attested_streaming_transport_poc_test/*.*:ServiceLevelRouteAttestation.*:StreamRouteControl.*:TransportRouteControl.*:LocalRouteControl.*:EnclaveLocalRouteControl.*:ProtectedRpcRuntime.* --gtest_fail_fast`
- `build_debug_coroutine_fake_sgx/output/rpc_test --gtest_filter=attested_streaming_transport_poc_test/*.*:ServiceLevelRouteAttestation.*:StreamRouteControl.*:TransportRouteControl.*:LocalRouteControl.*:EnclaveLocalRouteControl.*:ProtectedRpcRuntime.* --gtest_fail_fast`
- `cmake --build build_debug --target rpc_test attestation_service_test`
- `cmake --build build_debug_sgx_sim --target rpc_test attestation_service_test`
- `build_debug/output/attestation_service_test --gtest_brief=1`
- `build_debug_sgx_sim/output/attestation_service_test --gtest_brief=1`
- `build_debug/output/rpc_test --gtest_fail_fast`
- `build_debug_sgx_sim/output/rpc_test --gtest_fail_fast`
- `build_debug_coroutine_fake_sgx/output/rpc_test --gtest_fail_fast --gtest_brief=1`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_filter=type_test/41.* --gtest_brief=1`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_fail_fast --gtest_brief=1`
- `cmake --build build_debug_coroutine_sgx_sim --target rpc_test`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_filter=ProtectedRpcRuntime.*:ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/*`
- `cmake --build build_debug_coroutine_fake_sgx --target rpc_test`
- `build_debug_coroutine_fake_sgx/output/rpc_test --gtest_filter=ProtectedRpcRuntime.*:ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/*`
- `cmake --build build_debug_coroutine --target rpc_test`
- `build_debug_coroutine/output/rpc_test --gtest_filter=attested_streaming_transport_poc_test/*`
- `cmake --build build_debug --target attestation_service_test`
- `build_debug/output/attestation_service_test --gtest_filter=AttestationService.ProtectsObjectReleasedRequest:AttestationService.ProtectsTransportDownRequest:AttestationService.ProtectsTryCastRequest:AttestationService.ProtectsReleaseRequest:AttestationService.ProtectedRequestsAllowMutablePublicBackChannels:AttestationService.ProtectsAddRefRequest:AttestationService.ProtectsSendRequestAndResponse:AttestationService.ProtectedSendRejectsTamperedCiphertext`
- `cmake --build build_debug_coroutine_fake_sgx --target attestation_service_test`
- `build_debug_coroutine_fake_sgx/output/attestation_service_test --gtest_filter=AttestationService.ProtectsObjectReleasedRequest:AttestationService.ProtectsTransportDownRequest:AttestationService.ProtectsTryCastRequest:AttestationService.ProtectsReleaseRequest:AttestationService.ProtectedRequestsAllowMutablePublicBackChannels:AttestationService.ProtectsAddRefRequest:AttestationService.ProtectsSendRequestAndResponse:AttestationService.ProtectedSendRejectsTamperedCiphertext`
- `cmake --build build_debug_coroutine_fake_sgx --target rpc_test`
- `build_debug_coroutine_fake_sgx/output/rpc_test --gtest_filter=ProtectedRpcRuntime.*:ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/*`
- `cmake --build build_debug_coroutine_fake_sgx --target rpc_test`
- `build_debug_coroutine_fake_sgx/output/rpc_test --gtest_filter=ProtectedRpcRuntime.*:ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/*`
- `cmake --build build_debug_coroutine_sgx_sim --target attestation_service_test`
- `build_debug_coroutine_sgx_sim/output/attestation_service_test --gtest_filter=AttestationService.ProtectsObjectReleasedRequest:AttestationService.ProtectsTransportDownRequest:AttestationService.ProtectsTryCastRequest:AttestationService.ProtectsReleaseRequest:AttestationService.ProtectedRequestsAllowMutablePublicBackChannels:AttestationService.ProtectsAddRefRequest:AttestationService.ProtectsSendRequestAndResponse:AttestationService.ProtectedSendRejectsTamperedCiphertext`
- `cmake --build build_debug_coroutine_sgx_sim --target rpc_test`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_filter=ProtectedRpcRuntime.*:ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/*`
- `cmake --build build_debug_coroutine_sgx_sim --target attestation_service_test`
- `build_debug_coroutine_sgx_sim/output/attestation_service_test --gtest_filter=AttestationService.ProtectsReleaseRequest:AttestationService.ProtectedRequestsAllowMutablePublicBackChannels:AttestationService.ProtectsAddRefRequest:AttestationService.ProtectsSendRequestAndResponse:AttestationService.ProtectedSendRejectsTamperedCiphertext`
- `cmake --build build_debug_coroutine_sgx_sim --target rpc_test`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_filter=ProtectedRpcRuntime.*:ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/*`
- `cmake --build build_debug_coroutine_fake_sgx --target attestation_service_test`
- `build_debug_coroutine_fake_sgx/output/attestation_service_test --gtest_filter=AttestationService.ProtectsReleaseRequest:AttestationService.ProtectedRequestsAllowMutablePublicBackChannels:AttestationService.ProtectsAddRefRequest:AttestationService.ProtectsSendRequestAndResponse:AttestationService.ProtectedSendRejectsTamperedCiphertext`
- `cmake --build build_debug_coroutine_fake_sgx --target rpc_test`
- `build_debug_coroutine_fake_sgx/output/rpc_test --gtest_filter=ProtectedRpcRuntime.*:ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/*`
- `cmake --build build_debug --target attestation_service_test`
- `build_debug/output/attestation_service_test --gtest_brief=1`
- `cmake --build build_debug_coroutine_sgx_sim --target attestation_service_test attestation_stream_test rpc_test`
- `build_debug_coroutine_sgx_sim/output/attestation_service_test --gtest_brief=1`
- `build_debug_coroutine_sgx_sim/output/attestation_stream_test --gtest_brief=1`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_filter=ProtectedRpcRuntime.*:ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/* --gtest_fail_fast --gtest_brief=1`
- `cmake --build build_debug_coroutine_sgx_sim --target sgx_attestation_test_host attestation_service_test rpc_test`
- `build_debug_coroutine_sgx_sim/output/sgx_attestation_test_host --gtest_brief=1`
- `build_debug_coroutine_sgx_sim/output/attestation_service_test --gtest_brief=1`
- `build_debug_coroutine_sgx_sim/output/rpc_test --gtest_filter=attested_streaming_transport_poc_test/*.*:ServiceLevelRouteAttestation.*:ProtectedRpcRuntime.*:StreamRouteControl.*:TransportRouteControl.*:LocalRouteControl.*:EnclaveLocalRouteControl.* --gtest_brief=1`
- `cmake --build build_debug_coroutine_fake_sgx --target attestation_service_test attestation_stream_test rpc_test`
- `build_debug_coroutine_fake_sgx/output/attestation_service_test --gtest_brief=1`
- `build_debug_coroutine_fake_sgx/output/attestation_stream_test --gtest_brief=1`
- `build_debug_coroutine_fake_sgx/output/rpc_test --gtest_filter=ProtectedRpcRuntime.*:ServiceLevelRouteAttestation.*:attested_streaming_transport_poc_test/* --gtest_fail_fast --gtest_brief=1`

### Not Yet Implemented

- Separate `cmw.h`, `backend.h`, `policy.h`, and `security_context.h` header
  split. The current POC keeps these passive types together in `types.h`.
- Full production CMW / attestation context IDL split. The current route
  handshake has minimal generated IDL carriers for fake Evidence and
  backend-neutral identity.
- Backend selection for TDX, SEV-SNP, and TrustZone/PSA. The current
  build-time factory selects `FAKE`, `SGX_SIM`, `SGX_EPID`, `DCAP`, or `NULL`.
  The SGX-sim backend is development/simulation evidence with
  `security_level::simulation`; it is not remote attestation. The SGX EPID
  backend is a legacy hardware-compatibility skeleton with CMW/schema, policy,
  and provider/verifier seams, but no Intel PSW/AESM or IAS adapter wired yet.
  The SGX DCAP backend has a generated CMW schema, fail-closed default,
  production build selection, and quote-provider/verifier seams, but no Intel
  QL/QvL/QvE adapter wired yet.
- SGX quote or `sgx_ttls` simulation helpers. The current SGX-sim slice uses
  local-report mechanics only. Quote and TLS-certificate carriers should be
  added only where the Intel simulation libraries run without hardware
  services, and they must remain explicitly non-production evidence.
- Strict end-to-end enforcement for every `transport_down`. The protected
  endpoint-originated payload carrier and inbound unwrap support exist, but
  there is no production outbound service hook or sender yet. Route-layer
  plaintext `transport_down` remains valid for intermediate-synthesized
  liveness notifications.
- `add_ref` route-control options and `release_options` intentionally remain
  visible public route-control fields. `build_out_param_channel`, including
  the optimistic flag, `requesting_zone_id`, and `release_options` are live
  transport/passthrough routing and lifetime-accounting inputs before the
  endpoint service hook can unwrap the encrypted payload. These fields are
  authenticated as protected-RPC AAD and repeated in encrypted plaintext for
  endpoint binding checks.
- Broader topology coverage for enclave-local marshaller operations. The
  generic `rpc::local` transports should remain policy-light in-process links;
  enclave policy should live in enclave-specific local transport wrappers or
  adapters. The current wrapper marker is
  `rpc::sgx::coro::enclave::local_route_transport`, with
  `local_child_transport` and `local_parent_transport` as the concrete
  parent/child local wrappers. The generic local child transport exposes only
  an overridable child-side parent-transport factory; its default behavior is
  unchanged, and the enclave-local wrapper uses that factory to create a marked
  parent transport during real `connect_to_zone` child creation. The marked
  parent transport creates an `rpc::enclave_service` for that child zone rather
  than a plain `rpc::child_service`. A local B-to-C hop inside one enclave does
  not need attestation just because it is local, but if A passes an interface
  owned by D through B to C, C still has to validate D as the referenced
  route/security subject. Outbound `add_ref` and `release` now use the
  referenced owner route over marked enclave-local transports instead of the
  adjacent local peer. Runtime coverage verifies marked local creation,
  enclave-service child creation, generated `send` and `post` over the marked
  local child, reference-control route subject selection, protected
  `try_cast`, protected `object_released`, plaintext route-layer
  `transport_down`, generic
  control-status guardrails, and local `get_new_zone_id` sanitisation.
- Full audit coverage for coroutine dynamic-library transports that override
  marshaller outbound methods without using `rpc::stream_transport::transport`.
  C ABI is intentionally excluded from this slice because that implementation
  is expected to be rewritten for Rust.
- Future route/transport lifetime audit. `rpc::service` already has intricate
  transport registration and lifetime rules around `transports_`,
  service-proxy ownership, passthroughs, parent/child transports, and teardown.
  Do not merge `attestation_route_states_` into that structure as part of the
  current attestation work. Later, after reviewing the lifetime invariants in
  `documents/architecture/cpp/reference-ownership-invariants.md` and related
  transport documents, consider whether a generic per-route registry entry can
  simplify lifetime management. If that refactor happens, the base entry should
  remain attestation-neutral and allow transport liveness to be independent
  from route-security/audit state.
- Transport naming cleanup. `c++/transports/sgx_coroutine` is increasingly a
  stream-backed enclave transport with SGX as the first runtime, while
  `c++/transports/sgx` remains the ECALL/OCALL-specific blocking transport.
  Rename the coroutine directory only as a dedicated low-risk churn pass, likely
  toward an `enclave_streaming` style name with SGX-specific adapters beneath
  it.
- Non-zero `service_request_id` semantics.
- TLS exporter or ephemeral key-exchange binding. The current development
  binding is transcript id, identity, role, and nonce based; hardware-grade
  sessions now require explicit shared key material before protected RPC can be
  established.
- `sgx_ttls`, DCAP, production SGX local attestation, EPID PSW/AESM quote
  provider, IAS verifier, TDX, SEV-SNP, or TrustZone/PSA backends.

### Current Best Next Step

Telemetry and `post_report` are demo/diagnostic surfaces and are intentionally
left out of the current production attestation path. Stream sign-on and
service-level route handshakes now distinguish "peer Evidence is not required"
from "an unattested peer is explicitly allowed", and the backend factory can
select the current `FAKE` or `NULL` development backends with `NULL` as the
fail-closed build default for fresh CMake configurations. The current
route-control fields documented in `intermediate-visibility-audit.md` should
stay visible while passthroughs depend on them, and protected RPC should keep
authenticating them. The SGX-sim backend slice now has verifier-challenge CMW
blobs carrying peer `sgx_target_info_t`, so reports can be targeted to the
verifier where the two-message route handshake allows it. The EPID slice now
has the generated CMW schema, backend selection, fail-closed default behavior,
and quote-provider/verifier seams. It still needs a transcript-bound key
exchange before it can establish protected-RPC AEAD keys. The DCAP slice now
has the generated CMW schema, backend selection, hardware policy defaults,
fail-closed default behavior, and quote-provider/verifier seams. Next, on the
best available SGX hardware, wire either the EPID provider/verifier path for
legacy demos or the DCAP provider/verifier path for SGX-FLC machines, and bind
that evidence to an agreed shared secret before establishing protected-RPC
AEAD keys.

## Architectural Layers

The implementation is split into eight components with narrow contracts.
Each component depends only on what is below it. Cross-layer reaching is
forbidden.

```mermaid
flowchart TD
    L8["L8 Application / rpc::enclave_service<br/>Consumes security_context to authorise calls"]
    L7["L7 Protected RPC Envelope<br/>Encrypts and decrypts marshaller payloads"]
    L6["L6 Transport / Streaming Decorator<br/>attestation_stream wraps secure_stream"]
    L5["L5 Wire Format<br/>Moves CMW Evidence on the chosen carrier"]
    L4["L4 Attestation Service<br/>Per-enclave sessions, keys, counters, kind selection"]
    L3["L3 Attestation Backend<br/>Per-TEE produce_evidence / verify_evidence"]
    L2["L2 CMW Envelope<br/>Backend-neutral Evidence container"]
    L1["L1 Policy + Security Context + Zone Classifier<br/>Passive types and pure functions"]

    %% Visual stack order only. The dependency graph below is authoritative.
    L8 --- L7
    L7 --- L6
    L6 --- L5
    L5 --- L4
    L4 --- L3
    L3 --- L2
    L2 --- L1
```

### What Each Layer Does, And Does Not Do

- **L1 Policy / Security Context / Zone Classifier.**
  - *Does*: hold compile-time policy values (`MRSIGNER` allow-lists,
    `ISVSVN` minima, TCB acceptance), define enclave-wide defaults plus
    per-zone, per-service, per-interface, and per-method policy overlays,
    define accepted peer backend ids and security levels for each destination,
    define the passive `security_context` record, expose
    `attestation_kind required_for(local, peer)` as a pure function of
    two `zone_address` values.
  - *Does not*: hold runtime state, perform I/O, know about any
    specific TEE, talk to any backend.

- **L2 CMW Envelope.**
  - *Does*: define the `cmw` value type
    `{media_type, content_format, payload}` and its serialisation in
    Canopy's existing encodings.
  - *Does not*: interpret payload bytes, know about TLS, generate or
    verify signatures.

- **L3 Attestation Backend.**
  - *Does*: implement one TEE technology (Fake, SGX local, SGX DCAP,
    SGX EPID, simulation, future TDX, future SEV-SNP, future
    TrustZone/PSA). Produce CMW Evidence binding a given key,
    transcript, or native report-data field. Verify CMW Evidence and
    return a typed verdict plus attested identity. Advertise which CMW
    content formats it can produce and which it can verify, so a local SGX
    enclave can verify peer SEV-SNP Evidence when the verifier is available.
    Refuse production policy when the backend is development-grade.
  - *Does not*: own sessions, derive session keys, manage counters,
    decide local-versus-remote, touch TLS, touch RPC, log to
    application telemetry.

Production release builds add a second guard at configuration time.
`CANOPY_PRODUCTION_RELEASE` defaults to `OFF`, and formal release presets opt
in explicitly. When enabled, it rejects fake attestation, SGX-simulation
attestation, fake SGX, and SGX simulation hardware mode. It also disables
`CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS`, which removes the fake and
SGX-simulation attestation backend implementation files from the production
`security_attestation` target. Release-like simulation presets are still
allowed as build tests only by setting `CANOPY_PRODUCTION_RELEASE=OFF`
explicitly.

- **L4 Attestation Service.**
  - *Does*: one per enclave. Establish and cache enclave-pair
    sessions. Choose a local Evidence backend per session using L1's
    classifier, and choose a peer verifier backend from the received CMW type
    plus destination policy.
    Derive per-session AEAD keys (per caller-zone, destination-zone,
    direction). Own the monotonic counter store. Publish a
    `security_context` to the enclave service. Expose a typed handle for
    L7 to encrypt and decrypt with.
  - *Does not*: know which TEE produced the Evidence (delegated to
    L3). Know how Evidence travels on the wire (delegated to L5).
    Know what RPC payload looks like (delegated to L7). Grant a peer
    permission to call every zone in the enclave just because the peer
    enclave identity was cryptographically established.

- **L5 Wire Format.**
  - *Does*: serialise and deserialise the CMW Evidence onto a
    specific carrier. Phase 2: in-tunnel development exchange.
    Phase 3: `sgx_ttls` X.509 certificate extension. Phase 8:
    IETF TLS extensions. Negotiate carrier-level features such as
    attestation capability advertisement, including supported Evidence content
    formats and verifier capabilities for cross-TEE handshakes.
  - *Does not*: produce or verify Evidence itself (delegated to L3 via
    L4). Make policy decisions. Touch RPC payloads.

- **L6 Transport / Streaming Decorator.**
  - *Does*: `streaming::attestation::stream` wraps a
    `streaming::secure::stream`. After the secure stream handshake,
    drives the L5 wire exchange, calls L4 for session establishment,
    and exposes the resulting `security_context` for enclave-owned
    connection setup code to publish.
  - *Does not*: peek inside CMW payload bytes. Cache sessions
    (L4 does that). Encrypt application data (L7 does that).

- **L7 Protected RPC Envelope.**
  - *Does*: ask L4 for the AEAD key for a given (caller, destination,
    direction). Build the outer envelope with the public routing
    header and the encrypted plaintext inner block. Validate inbound
    envelopes by asking L4 to decrypt and check the counter, then
    hand the inner request to the existing stub dispatch.
  - *Does not*: derive keys, talk to a backend, terminate TLS, decide
    whether a peer is allowed (L8 decides).

- **L8 Application / `rpc::enclave_service`.**
  - *Does*: read `security_context` and apply application-level
    authorisation policy. Decide whether a peer enclave is allowed to
    call a specific interface or object. Apply the destination zone's
    stricter policy even when the caller is a local gateway zone that
    admitted an unattested browser or websocket client.
  - *Does not*: cryptographic work. Session management.
    Wire-format decisions.

### Dependency Direction

A layer may call only into layers numerically below it. Specifically:

```mermaid
flowchart LR
    L8["L8 Application"] --> L7["L7 Protected RPC Envelope"]
    L7 --> L4["L4 Attestation Service"]

    L6["L6 Transport / Streaming Decorator"] --> L5["L5 Wire Format"]
    L5 --> L4
    L4 --> L3["L3 Attestation Backend"]
    L3 --> L2["L2 CMW Envelope"]
    L2 --> L1["L1 Policy + Security Context"]

    L5 --> L2
```

There is no reverse arrow. L3 never sees L4's session state. L4 never
sees L5's wire bytes. L7 never instantiates a backend.

### Why The Split Matters

- Swapping the wire format (`sgx_ttls` to IETF extensions) changes only
  L5.
- Adding a new TEE (DCAP to TDX) changes only L3.
- Adding a new transport (TCP to io_uring) changes only L6.
- Tightening production policy changes only L1.
- Adding a non-attested encrypted route mode, such as certificate-authenticated
  encryption between non-enclave zones, should change L1/L4/L5 policy and
  handshake/session establishment only. It must reuse the L7 protected RPC
  envelope rather than adding a second encrypted RPC path.
- The protected RPC envelope (L7) is agnostic to all of the above:
  given a session handle, it encrypts and decrypts. This is the property
  that lets phases 1, 2, 3, and 4 progressively swap backends and wire
  formats without touching the envelope code.

## Phase / Layer Map

```mermaid
flowchart TD
    P0["Phase 0<br/>Foundations and CMW<br/>L1, L2, L3-null<br/>No SGX needed"]
    P1["Phase 1<br/>Fake backend end-to-end<br/>L3-fake, L4, L7<br/>No SGX needed"]
    P2["Phase 2<br/>In-tunnel attestation<br/>L5-in-tunnel, L6<br/>No SGX needed"]
    P3["Phase 3<br/>sgx_ttls X.509 carrier<br/>L5-sgx_ttls<br/>SGX hardware or simulation"]
    P4["Phase 4<br/>DCAP backend<br/>L3-dcap<br/>SGX FLC hardware + PCCS"]
    P5["Phase 5<br/>Local attestation<br/>L3-local<br/>Any SGX-capable host"]
    P6["Phase 6<br/>Routed attestation<br/>L4 routed session<br/>Any SGX-capable host"]
    P7["Phase 7<br/>Policy hardening and operations<br/>L1 hardened<br/>Production prep"]
    P8["Phase 8<br/>IETF carrier, EPID, TDX,<br/>SEV-SNP, TrustZone/PSA<br/>Deferred"]

    P0 --> P1
    P1 --> P2
    P2 --> P3
    P3 --> P4
    P4 --> P5
    P5 --> P6
    P6 --> P7
    P7 --> P8
```

Each phase adds or replaces a layer without disturbing the layers above
or below it. This is the property that lets the team merge phase 1 (no
SGX) before phase 4 (real SGX) without rework.

Phases 0-2 can be developed and merged on machines with no SGX hardware
because everything compiles against the Fake backend.

## Phase 0 -- Foundations And CMW

Layers added: **L1, L2, L3-null.** No state, no I/O.

Goal: introduce the backend-neutral attestation interface, the CMW
envelope type, the security context type, and the routing classification
function. No backend yet beyond a `null` one that always refuses to
attest.

### Deliverables

- New CMake target tree under `c++/security/attestation/`:
  - `include/security/attestation/cmw.h` -- CMW value type with
    `media_type`, `content_format`, and `payload` fields, plus
    YAS/serialisation helpers.
  - `include/security/attestation/backend.h` -- abstract
    `attestation_backend` interface (`produce_evidence`,
    `verify_evidence`, `capabilities`).
  - `include/security/attestation/policy.h` -- `policy` value type from
    `attestation-backends.md`.
  - `include/security/attestation/security_context.h` -- per-session
    record (attested identity, session id, key id, backend id, security
    level).
  - `include/security/attestation/kind.h` -- `attestation_kind` enum and
    `kind required_for(local, peer)` derived from `zone_address` per
    `overview.md` "Routing Classification".
  - `src/null_backend.cpp` -- backend that returns
    `SUPPORTS_PRODUCTION_POLICY=false` and refuses every evidence call.
- Build option `CANOPY_ATTESTATION_BACKEND` with values
  `NULL` (fail-closed build default and explicit no-attestation policy for
  demos/browser clients) and `FAKE` (development Evidence, selected
  explicitly).
- Conditional compilation: when set to `NULL`, Canopy still builds without
  hardware attestation. Production call sites must not treat this as an
  attested route; they must explicitly opt in to any no-Evidence policy.
- IDL additions in `interfaces/rpc/`:
  - `cmw` value type (matches the C++ definition).
  - `attestation_context` and `security_failure_context` back-channel
    types, per `back-channel-context.md` (definitions empty for now if
    full schema is not yet pinned).
- Stub unit tests under `tests/attestation/` that exercise the CMW
  serialiser and the routing classifier.

### Verification

- `cmake --build build_debug --target attestation_test` runs the new
  unit tests.
- `rpc_test` and all other existing tests pass unchanged.

### Exit Criteria

- The `attestation_backend` interface is the only entry point used by
  later phases. No other Canopy code references SGX, DCAP, or sgx_ttls
  yet.
- CMW round-trips through the Canopy serialisers used elsewhere in the
  RPC stack.

### Notes

- The exact `encrypted_payload` IDL shape is pinned in phase 1, not here,
  to keep this phase scope-tight.
- Anything that would later require a real backend (sealed keys, the
  `tee_*` calls, AESM) is left out. This phase is pure abstraction.

## Phase 1 -- Fake Backend End-To-End

Layers added: **L3-fake, L4, L7.** No wire-format or transport changes
yet; sessions are established by direct method call between two
attestation services in the same process.

Goal: complete the in-process attestation, key-exchange, counter, and
protected-envelope code paths using a development backend that does not
need SGX hardware. After this phase, an enclave-pair session can be
established, keys derived, and a protected `send` round-tripped through
the existing Canopy transport, all on a plain Linux host.

### Deliverables

- `src/fake_backend.cpp` -- implements `attestation_backend` with:
  - deterministic-or-configured fake enclave identity;
  - fake evidence signed with a build-time development key (so
    `verify_evidence` exercises a real signature check);
  - explicit `backend_id == "fake"` and
    `security_level == development`;
  - refusal to attest when `policy.production` is true.
- `attestation_service` in
  `c++/security/attestation/src/service.cpp`:
  - one instance per enclave (or, for now, per process);
  - tracks active sessions keyed by enclave-pair identity;
  - per-session: peer identity, session id, per-key counters, key
    derivation context;
  - exposes `establish_session(peer_evidence_cmw)` and
    `produce_self_evidence(session_handle, key_to_bind)`.
- Key-derivation helper:
  - HKDF-Extract over the session shared secret;
  - HKDF-Expand-Label over `(enclave-pair session, caller zone,
    destination zone, direction)` per `protected-rpc-envelope.md`;
  - canonical KDF input encoding with golden vectors;
  - no direct dependence on YAS or protocol-buffer byte output for KDF inputs;
  - one AEAD key per derived label.
- Counter store: in-memory, per derived key, with monotonic guarantee
  and per-key exhaustion handling.
- `encrypted_payload` IDL type pinned in `interfaces/rpc/`:
  - outer marker fingerprint via
    `rpc::id<encrypted_payload>::get(version)`;
  - inner protected plaintext as defined in
    `protected-rpc-envelope.md`.
- Service hooks:
  - `outbound_send`, `outbound_post` paths in `c++/rpc/src/service.cpp`
    learn to call into the attestation service to wrap when a session
    exists;
  - inbound dispatch in `c++/rpc/src/service.cpp` learns to unwrap an
    inbound `encrypted_payload` and recover the inner request before
    handing to the existing stub-dispatch code.
  - `add_ref`, `release`, `try_cast`, `object_released` are wrapped
    behind a feature flag in this phase; default off until phase 5
    routed attestation is in place, because their semantics interact
    with route construction.
- `CANOPY_ATTESTATION_BACKEND=FAKE` makes the configured factory use the fake
  backend for development tests.
- Tests under `tests/attestation/`:
  - session establishment between two `attestation_service` instances
    in the same process;
  - key derivation determinism and direction separation;
  - counter monotonicity and exhaustion behaviour;
  - protected `send` round-trip through a loopback transport;
  - replay rejection;
  - fraud test: backend rejects evidence with a tampered preimage.

### Verification

- New tests pass on a developer laptop without SGX hardware.
- `rpc_test` continues to pass.
- A demo can be added under `c++/demos/` that runs two services in one
  process and prints "attested fake session established" before
  exchanging a protected call.

### Exit Criteria

- The protected-RPC envelope is on the hot path. Once a session exists
  between two services, ordinary `send`/`post` traffic and endpoint
  `add_ref` / `release` payloads are encrypted and authenticated end-to-end.
- The attestation service is the only producer of session keys. Nothing
  in transport code derives keys directly.
- The full CMW shape is exercised end-to-end. Phase 3 will replace the
  payload bytes with real DCAP quote bytes; nothing else above the
  backend interface changes.

### Notes

- Fake evidence is signed but the key is not a production root of trust.
  Production policy in phase 7 must reject `backend_id == "fake"`.
- AES-GCM is the provisional AEAD; nonce derivation is per
  `protected-rpc-envelope.md` ("Encryption"). Per-key fixed prefix plus
  monotonic counter.

## Phase 2 -- Streaming-Layer Attestation Decorator

Layers added: **L5-in-tunnel, L6.** This is the first phase where bytes
specific to attestation appear on a transport. L4 and below are
unchanged.

Goal: introduce the `attestation_stream` decorator that wraps
`streaming::secure::stream`, runs the attestation exchange immediately
after the TLS handshake, and makes the resulting `security_context`
available to `rpc::enclave_service`. Still uses the Fake backend; no SGX yet.

### Deliverables

- New CMake target `streaming_attestation` under
  `c++/streaming/attestation/`:
  - `include/streaming/attestation/stream.h` -- decorator that wraps a
    `std::shared_ptr<streaming::stream>` (the underlying secure
    stream).
  - `src/stream.cpp` -- runs the handshake exchange: client sends
    `evidence_proposal`-shaped CMW, server replies with selected
    evidence kind and its Evidence, client replies with its Evidence
    (if the session is mutual), both run their attestation service
    `verify_evidence`, both publish `security_context`.
- For phase 2 the exchange runs *inside* the established TLS tunnel as
  length-prefixed application bytes. This is the simplest form that can
  bind Evidence to the already-negotiated TLS session using a TLS exporter
  value plus Canopy transcript context. Phase 3 replaces this with the
  certificate-extension form using `sgx_ttls`, which uses a different
  report-data binding.
- Wire framing for phase 2 only: length-prefixed YAS binary frames using the
  four message kinds in
  [wire-format.md, "In-Tunnel Development Carrier"](wire-format.md).
- Transport plumbing:
  - enclave-owned connection setup publishes a `security_context` from
    an attested stream into `rpc::enclave_service`;
  - `rpc::enclave_service` exposes a `security_context` accessor keyed
    by adjacent zone. The base `rpc::service` remains attestation-neutral.
- Update the websocket demo
  (`c++/demos/websocket/server/enclave_websocket_server.cpp`) to wrap
  its TLS stream in `attestation_stream` when the listener policy
  requests attestation. Browser-facing listeners still pass the
  `kind::none` policy and behave unchanged.
- Tests:
  - two services connected by a loopback secure stream perform the
    attestation exchange, both sides verify fake Evidence, both
    publish a `security_context`;
  - failure tests: the verifier rejects mismatched binding, missing
    Evidence, downgraded backend.

### Verification

- The fake-backend demo from phase 1 now runs through a TLS-terminated
  loopback connection rather than direct loopback. Verification of the
  phase-2 session binding goes through real TLS exporter material.
- All existing TLS/websocket tests pass.

### Exit Criteria

- Every transport that uses `streaming::secure::stream` can opt into
  attestation by wrapping with `streaming::attestation::stream` and a
  policy.
- The TLS layer continues to terminate inside the enclave; no plaintext
  reaches the host.

### Notes

- The phase-2 in-tunnel exchange may remain as a development and fallback
  carrier, but it is not the same binding as `sgx_ttls`. Phase 3 moves the
  SGX production-shaped path into the X.509 certificate extension, where the
  attested certificate key signs the TLS handshake.

## Phase 3 -- sgx_ttls X.509 Carrier

Layers changed: **L5 replaced.** L1-L4 and L6-L8 are unchanged.

Goal: replace the in-tunnel CMW exchange with Intel's `sgx_ttls`
certificate extension carrying the Evidence. This is the
current SGX carrier described in [wire-format.md](wire-format.md). Still no DCAP
backend yet (the Fake backend can produce a synthetic "quote-like" CMW
that the verify path treats as if it came from the X.509 extension).

### Deliverables

- Add a certificate-extension flow for SGX production-shaped attestation:
  - Attester (server, and optionally client) builds an X.509 cert via
    `tee_get_certificate_with_evidence` whose extension carries a DCAP
    quote over the helper's public-key claims;
  - Relying Party installs a TLS verify callback that calls the
    DCAP/SGX certificate-evidence verifier during the standard TLS
    handshake, then applies Canopy policy to the attested identity.
- For the Fake backend, provide
  `fake_certificate_extension_helpers.cpp` that emits and parses a
  Canopy-owned development extension with the same carrier role. Do not route
  fake evidence through `tee_verify_certificate_with_evidence`; that helper
  verifies real Intel quote evidence.
- Build wiring:
  - link `libsgx_ttls.a` (in-enclave) and any host-side helpers when
    `CANOPY_BUILD_ENCLAVE=ON`;
  - import `submodules/confidential-computing.sgx/common/inc/sgx_ttls.edl`
    into `c++/transports/sgx_coroutine/edl/`.
- Tests:
  - the X.509 extension can be produced and consumed by the Fake
    backend;
  - the existing protected `send` test from phase 1 still passes via
    this new wire shape;
  - rejection tests: bad signature in extension, mismatched binding,
    unknown media type.

### Verification

- The websocket demo now uses the certificate-extension wire format.
  The TLS handshake itself fails closed when policy rejects the
  Evidence; there is no application-layer round-trip required.

### Exit Criteria

- The wire format matches what real DCAP will use, so phase 4 is purely
  a backend swap.

### Notes

- Document the custom OID used for the SGX RA quote extension in
  `wire-format.md` once it is pinned in code. `sgx_ttls` already picks
  one; Canopy should not invent a different one.
- This phase still does not require SGX hardware: the Fake backend
  emits its own extension layout, and `sgx_ttls` is only called when
  the DCAP backend is selected.

## Phase 4 -- DCAP Backend On Real Hardware

Layers added: **L3-dcap.** L4-L8 are unchanged because L3's contract did
not move.

Goal: implement the DCAP backend so production hardware can produce and
verify real Evidence. After this phase, an enclave-to-enclave session
between SGX-FLC machines is attested and protected.

Current status: the DCAP schema/backend seam is implemented. The code now has
`sgx_dcap_protocol.idl`, `sgx_dcap_backend`, `sgx_dcap_quote_provider`,
`sgx_dcap_quote_verifier`, CMake/backend-factory selection for
`CANOPY_ATTESTATION_BACKEND=DCAP`, fail-closed behavior when no provider or
verifier is installed, and host tests with injected synthetic quote material.
The remaining work in this phase is the hardware adapter that calls Intel DCAP
APIs and maps QL/QvL/QvE results to Canopy policy verdicts.

### Deliverables

- `src/sgx_dcap_backend.cpp` implements `attestation_backend`:
  - complete: hashes canonical `sgx_dcap_report_binding` to produce the
    32-byte value that must be embedded in SGX `report_data`;
  - complete: emits and parses a Canopy-owned CMW wrapper with
    `media_type == "application/canopy-sgx-dcap-evidence"`, preserving the raw
    quote bytes separately from Canopy binding and optional appraisal summary;
  - complete: fails closed when no quote provider or quote verifier is
    installed;
  - remaining: provider `produce_quote(report_data)` calls
    `sgx_qe_get_target_info` inside the host, `sgx_create_report` inside the
    enclave, and `sgx_qe_get_quote` back in the host;
  - remaining: verifier `verify_quote(...)` calls `sgx_qv_verify_quote` (or
    `tee_verify_quote`, see [DCAP Operations](dcap-operations.md)) with
    `qve_report_info`, then inside the enclave calls
    `sgx_tvl_verify_qve_report_and_identity`, then enforces application policy
    over the embedded report;
  - remaining: map `sgx_ql_qv_result_t` to backend verdicts per the
    failure-mode catalog in `dcap-operations.md`.
- Build wiring:
  - link `libsgx_dcap_ql` and `libsgx_dcap_quoteverify` on the host;
  - link `libsgx_dcap_tvl.a` inside the enclave;
  - import `<dcap>/QuoteVerification/dcap_tvl/sgx_dcap_tvl.edl` into the
    enclave EDL (`<dcap>` is the `dcap_source` path used in
    `dcap-operations.md`);
  - new build option `CANOPY_ATTESTATION_BACKEND=DCAP` for SGX HW
    presets.
- Threading: `sgx_qe_get_quote` and `sgx_qv_verify_quote` are run on a
  dedicated worker thread off the io_uring proactor, per the threading
  guidance in `dcap-operations.md`.
- Policy enforcement plumbing:
  - production policy values (`MRSIGNER`, `ISVPRODID`, minimum
    `ISVSVN`, debug-bit policy, acceptable TCB statuses, QvE ISVSVN
    threshold) live in a compile-time header in `c++/security/policy/`
    that the enclave links against;
  - debug builds may provide a relaxed policy and must declare so via
    a build option.
- Operator runbook (developer-facing):
  - install `sgx-aesm-service`, `libsgx-dcap-*`, point
    `/etc/sgx_default_qcnl.conf` at a PCCS instance;
  - build and run the vendored PCCS from
    `submodules/confidential-computing.sgx/external/dcap_source/QuoteGeneration/pccs/`;
  - run `PCKRetrievalTool` (or `SGXPlatformRegistration` for server
    CPUs) to register the platform;
  - this runbook can live as a section in `dcap-operations.md` or as
    a new file under `documents/security/attestation/runbooks/`.
- Tests:
  - on an SGX-FLC machine: end-to-end attested enclave-to-enclave
    session via the websocket demo;
  - failure-mode tests using deliberately corrupted Evidence;
  - TCB out-of-date policy test: confirm the backend refuses by
    default and accepts only when the policy explicitly allows it.

### Verification

- The websocket demo, built with `CANOPY_ATTESTATION_BACKEND=DCAP`,
  produces a valid quote, the peer verifies it, and the resulting
  session carries protected RPC traffic.
- The same demo built with `=FAKE` continues to work on developer
  machines without SGX hardware. No code outside the backend module
  branches on backend choice.

### Exit Criteria

- A first production-shaped attestation path exists for direct
  enclave-to-enclave SGX connections.
- The build matrix gains a "real-SGX integration" job that exercises
  this path.

### Notes

- TCB collateral lifecycle (refresh, offline snapshots) is operationally
  managed by PCCS; nothing in this phase tries to schedule that inside
  the enclave. Phase 7 may add an in-enclave freshness scheduler.
- This is the earliest phase where a real production deployment is
  conceivable, modulo the policy-hardening work in phase 7.

## Phase 5 -- Local Attestation

Layers added: **L3-local.** Adds the kind selection branch in L4 to
choose between local and remote backends, but no other layer changes.

Goal: support same-platform sibling enclaves attesting to each other
without DCAP collateral, using `EREPORT`/`EVERIFYREPORT`. This is the
fast path for SGX coroutine transports between enclaves on the same
host.

### Deliverables

- `src/local_backend.cpp` implements `attestation_backend` for local
  attestation:
  - `produce_local(target_info, report_data)` calls
    `sgx_create_report`;
  - `verify_local(report)` calls `sgx_verify_report` (no collateral
    needed; CPU report key is the root of trust);
  - emits CMW with `media_type == "application/sgx-report"`.
- Selection logic in `attestation_service::establish_session` uses
  `attestation_kind required_for(local, peer)` from phase 0 to decide
  whether to attempt local first.
- Fallback path: if local attestation fails because the peer is not on
  the same platform (the routing-prefix heuristic was wrong), retry
  with remote attestation using the DCAP backend.
- Transport plumbing: the SGX coroutine transport between sibling
  enclaves on the same host wires the local backend into its
  attestation stream by default.
- Tests:
  - two enclaves in the same process attest mutually;
  - mixed test: one local, one remote peer; the selector picks the
    correct kind for each.

### Verification

- The SGX coroutine transport demo runs two sibling enclaves and shows
  a local-attested session forming.

### Exit Criteria

- Both local and remote attestation paths share the same
  `attestation_service`, `security_context`, and protected-RPC
  envelope. The only difference is the backend and the CMW media type.

### Notes

- Local attestation does not need PCCS or AESM (AESM is still needed
  for any path that touches QE/PCE).
- Local backend should be allowed independently of DCAP at build time:
  some deployments may use local-only.

## Phase 6 -- Routed Attestation

Layers changed: **L4 extended**, new IDL interface in L8. L5 is bypassed
for routed sessions because the Evidence travels as RPC payload over an
existing transport rather than as TLS bytes.

Goal: support the case where zone A learns about zone C through an
existing route via zone B. A and C establish their own end-to-end
attested session over that route without B being able to read the
payloads.

### Deliverables

- IDL: `interface i_remote_attestation` in `interfaces/rpc/` with one
  reserved attestation method that carries CMW evidence both ways.
- Service: route the reserved object id (max value at active
  `zone_address` object-id bit width) to the in-enclave attestation
  service rather than the normal stub dispatcher.
- Speculative call shape: caller sends an RPC to the reserved
  attestation object on the peer zone over the existing route; the
  payload is the same CMW exchange used in direct attestation; the
  Evidence binding now covers the routed transport's session keys
  instead of TLS handshake keys.
- End-to-end key exchange: Diffie-Hellman (or X25519) over the routed
  RPC payloads, bound to the Evidence at each end.
- Cache: the resulting session is cached by enclave-pair identity in
  the local `attestation_service`. Subsequent RPC traffic between A
  and C, over any route, uses that session's keys.
- Tests:
  - A and C attest each other through B; B cannot decrypt A's
    payloads;
  - revocation: when A's enclave restarts, the cached session is
    invalidated and the next call re-attests;
  - multi-route: A reaches C via B1 today and B2 tomorrow; the same
    attested session covers both.

### Verification

- Three-zone integration test passes: A `send`s to C through B; B's
  service observes only routing headers and CMW-wrapped ciphertext;
  C's service decrypts and dispatches.

### Exit Criteria

- The protected envelope works end-to-end across multi-hop routes.
- Reference-protocol messages (`add_ref`, `release`, `try_cast`,
  `object_released`) can now be protected end-to-end; their flag from
  phase 1 is enabled by default for attested sessions.

### Notes

- The reserved interface should be marked so that ordinary `try_cast`
  reflection does not expose it unless a policy explicitly permits
  bootstrap discovery. See `back-channel-context.md`.
- This is the first phase where Canopy needs an in-enclave secure
  random number source for ephemeral DH keys. Use the enclave's RDRAND
  via the SGX SDK; do not use host-supplied entropy.

## Phase 7 -- Policy Hardening And Operations

Layers hardened: **L1, plus operational tooling around L3 and L4.** No
layer is removed or replaced; existing layers are tightened.

Goal: move from "works on my machine" to "would survive an audit." All
the soft edges in earlier phases are pinned down with policy, logging,
audit, and operational tooling.

### Deliverables

- Compile-time policy header `c++/security/policy/policy.h`:
  - `MRSIGNER` allow-list;
  - `ISVPRODID` allow-list;
  - minimum `ISVSVN` per product;
  - debug-bit policy;
  - TCB status allow-list and audit policy for `OUT_OF_DATE`,
    `CONFIG_NEEDED`, `SW_HARDENING_NEEDED`;
  - QvE ISVSVN threshold sourced from the QvE identity collateral as of
    build time;
  - allowed backend list (must reject `fake`/`simulation` in
    production builds);
  - enclave-wide default policy plus destination-zone overrides, so a
    public gateway zone can explicitly allow unattested clients while a
    treasury or control zone requires bidirectional attestation and stricter
    caller authorization;
  - cross-TEE peer backend allow-lists, so a destination can accept SGX DCAP,
    SEV-SNP, TDX, TrustZone/PSA, EPID compatibility, certificate-authenticated,
    or verify-only routes under distinct rules.
- Policy evaluation API that takes the local destination zone/service,
  caller identity, route-security status, interface id, method id, and
  delegated gateway context when present. A local gateway call must not
  launder an unattested browser client into a fully attested caller unless the
  destination zone explicitly accepts that delegation.
- Backend registry hardening: separate `can_produce_evidence` from
  `can_verify_evidence`, record supported CMW content formats, and ensure
  mutual attestation may use different producer and verifier backends on each
  side. A route is bidirectionally attested only when both sides appraise the
  other side's native Evidence or trusted Attestation Result under policy.
- Audit logging path that emits `security_failure_context` back-channel
  entries on policy failure, with field redaction applied per
  `failure-policy.md`.
- Route-state DDoS hardening: `attestation_route_states_` must not grow
  without bound from unauthenticated handshakes or forged `add_ref` route
  subjects. Add per-service caps for total route states, handshaking states,
  and failed unauthenticated states; add TTL/LRU eviction for failed unknown
  routes; limit concurrent route handshakes; and return
  `RESOURCE_EXHAUSTED()` or another public control error without storing new
  state once policy limits are reached. Established attested contexts should
  not be evicted by unauthenticated traffic.
- Protected-message memory limits: review the current protected field,
  encrypted payload, and back-channel entry caps against enclave EPC pressure.
  Keep generous host/test limits if useful, but allow enclave production
  policy to set smaller per-message and per-route allocation ceilings before
  authentication succeeds.
- Replay-window policy: the current receive-counter check is strictly
  monotonic per protected key scope and therefore assumes in-order delivery for
  each `(session, caller, destination, direction)`. Ordered stream transports
  satisfy that rule. If any coroutine, io_uring, passthrough, or future
  transport can deliver one protected scope out of order, add a bounded replay
  bitmap/window and tests before enabling pipelined protected traffic on that
  path.
- Public control-status downgrade review: public route-control failures are
  intentionally visible to intermediates and may be unauthenticated in some
  paths. Audit every public control status to ensure it cannot carry
  application-domain result codes, decrypted payload information, or sensitive
  policy details; document remaining denial-of-service or forced-retry
  behavior as an accepted liveness risk.
- Attestation-service lock-order note: document mutex ownership and lock
  ordering for session state, route state, and policy state. Future helpers
  must not call public methods that reacquire the same non-recursive mutex, and
  no mutex may be held across `CO_AWAIT`.
- TCB collateral freshness: a scheduled task inside the enclave that
  asks the host to refresh PCCS collateral on a known cadence; refusal
  to accept stale collateral past the policy window.
- Re-attestation timers: per-session policy may require a fresh
  attestation handshake after a configurable interval; rekey path uses
  the same backend interface.
- AESM contention rate-limiting in the host shim: a per-process
  semaphore around `sgx_qe_get_quote` to bound concurrent QE calls.
- QvE enclave-load policy: select `PERSISTENT` on long-lived servers,
  `EPHEMERAL` on bursty test runs.
- Performance dashboards: handshake latency, AESM queue depth, QvE
  enclave load events. Exposed via existing Canopy telemetry hooks
  (see [Telemetry And Logging Security](../telemetry-and-logging.md)).
- Negative-test suite: deliberately corrupted quotes, expired
  collateral, downgrade attempts, fake-evidence-in-production
  rejection, replay across epoch boundaries, out-of-order protected messages
  under any transport that claims pipelining support, and public-control
  status injection attempts.

### Verification

- Production builds reject every backend except DCAP (and Local, if
  enabled).
- Replay across an enclave restart is rejected and the session is
  re-established.
- Stale collateral past the policy window is rejected.

### Exit Criteria

- A reviewer can read the policy header and the `failure-policy.md`
  document together and see exactly what the system will and will not
  accept.
- Audit logs contain enough to investigate a failed handshake without
  containing anything sensitive.

## Phase 8 -- Deferred

Tracked here so the design is not forgotten.

- **EPID/IAS runtime adapter.** The CMW envelope, backend interface,
  fail-closed backend selection, and policy layer now exist. The deferred work
  is the platform adapter: obtain EPID quotes from the SGX PSW/AESM stack on
  SGX1 hardware, call or consume IAS verification, validate report_data against
  the Canopy transcript hash, and map IAS quote status/advisories into
  destination-zone policy. Once the real EPID provider/verifier path exists,
  repeat the crypto and concurrency review currently applied to the fake and
  simulation paths, with particular attention to transcript-bound key exchange,
  session epoch handling, and avoiding any fallback to development-derived
  secrets.
- **IETF wire format migration.** Replace the `sgx_ttls`
  certificate-extension envelope with the
  `evidence_proposal`/`evidence_request`/`attestation_evidence` TLS
  extensions when:
  - the IETF draft is adopted by the TLS WG (filename becomes
    `draft-ietf-tls-attestation-NN`);
  - IANA codepoints are allocated;
  - either Mbed TLS or SGXSSL ships upstream support, or Canopy
    decides to maintain a patch.
- **Certificate-authenticated encrypted routes without TEE attestation.**
  Preserve the current protected-RPC envelope, key derivation, counters,
  replay protection, and public routing model, but allow the session that feeds
  the envelope to come from a certificate-authenticated handshake instead of
  TEE Evidence. This is for non-attested zones that still require confidential
  and tamper-evident RPC payloads, including server-to-server links where SGX,
  TDX, SEV-SNP, TrustZone, or another TEE is not available. This must not be
  modelled as `unattested_allowed`: `unattested_allowed` remains the explicit
  no-Evidence/no-protected-session exception. The new mode should have its own
  route-security status and policy, such as `certificate_authenticated`, with
  `peer_attested == false` but `security_context.established == true`.
  Certificate exchange prevents MITM only when the certificate chain, pinned
  certificate, pinned public-key fingerprint, or configured zone allow-list is
  already trusted out of band. Self-signed certificates exchanged only in-band
  are not sufficient. The existing route handshake IDL carriers should be
  evolved rather than replaced: keep the request/response shape, CMW-style
  proof container, transcript id, nonce, identity, backend/mechanism id,
  security level, and session epoch, but add an explicit route-security kind
  so the same carrier can represent hardware attestation, development
  attestation, certificate-authenticated encryption, pinned-public-key
  encryption, or future PSK/resumption modes. If the current names become too
  narrow, introduce versioned neutral names such as
  `route_security_handshake_request` while keeping compatibility shims for the
  existing `route_attestation_handshake_*` types during migration.
- **Cross-TEE interoperability tests.** Once at least two production-grade
  backends exist, add tests where the two sides do not use the same TEE. The
  first useful pairs are SGX DCAP to SEV-SNP, SGX DCAP to TDX, and a
  verify-only host/browser client to SGX DCAP. These tests should prove that
  the route handshake can carry different request and response Evidence
  formats, that each side chooses the correct verifier backend from CMW
  metadata, and that destination-zone policy can accept or reject each peer
  backend independently.
- **Protected split add_ref atomicity.** Plain routed `add_ref` can currently
  compensate a committed destination leg by synthesizing a plaintext `release`
  if the caller leg fails. Protected `add_ref` cannot safely use that repair
  because an intermediate pass-through sees only an opaque typed payload and
  does not own the endpoint keys needed to authenticate the matching protected
  release. Keep the current fail-closed rule for now: do not synthesize
  plaintext compensation for opaque/protected payloads, roll back local
  pass-through state, and leave any owner-side partial state to teardown or
  later endpoint logic. A later protocol revision should add owner-side
  provisional reference counts plus endpoint-authored protected commit and
  abort/release messages, or an equivalent expiry-based repair, without making
  intermediates cryptographic authorities.
- **TDX backend.** When TDX support is required, add a backend that
  emits TDX quotes via the same DCAP host APIs (`tee_verify_quote` is
  already TDX-aware). CMW media types extend with
  `application/td-quote-v4` and similar.
- **AMD SEV-SNP backend.** Add a backend that maps SNP attestation
  reports and certificate-chain validation into the same CMW and verdict
  interface. Policy should use neutral measurement/version/debug fields
  so the RPC layer does not learn SNP-specific structures.
- **Arm TrustZone/PSA backend.** Add a backend for PSA/EAT-style
  attestation tokens or platform-specific TrustZone evidence. The
  transport and protected-RPC envelope should see only CMW plus the
  backend-neutral verdict.

## Cross-Phase Concerns

These run alongside the phase work and are not assigned to a single
phase.

### Build And CI

- A "no-SGX" CI job exercises phases 0-2 with `FAKE` backend on every
  PR.
- A "real-SGX" CI job exercises phases 3-7 on a SGX-FLC machine with
  PCCS available. Initially this job can be manual; once stable it
  should gate merges that touch the attestation code.
- A release-guard CI job configures at least one production preset with
  `CANOPY_PRODUCTION_RELEASE=ON` and verifies that fake SGX, SGX simulation,
  fake attestation, and SGX-sim attestation are rejected. Separate simulation
  release-like build tests must set `CANOPY_PRODUCTION_RELEASE=OFF`
  explicitly.
- Add a security-audit CI/test backlog for areas not covered by the first deep
  review: io_uring scheduler behavior, SGX coroutine host transport, IDL and
  protobuf/nanopb generator security changes, and documentation consistency
  against live CMake/code.

### Compatibility With Existing Code

- Until phase 4 ships, production presets should use
  `CANOPY_ATTESTATION_BACKEND=NULL` unless they deliberately enable fake or
  hardware attestation.
- The websocket demo continues to ship a browser-facing listener that
  does not attest, regardless of phase.

### Direct HTTP And WebSocket Clients

- Direct HTTP and WebSocket clients should be treated as stream-adjacent
  attestation users, not as a separate protected-RPC envelope. They should
  reuse the attestation service, CMW Evidence carriers, policy vocabulary, and
  route-security state where possible.
- A direct stream that completes attestation before RPC routing starts may
  publish its `security_context` to the owning `rpc::enclave_service` as the
  adjacent peer context. Any later remote interface or routed zone introduced
  across that stream still needs the normal service-level route handshake for
  the referenced zone; stream attestation proves the adjacent peer, not every
  future route behind it.
- Browser and JavaScript clients usually cannot produce enclave Evidence. They
  must therefore be admitted only by explicit listener policy, such as the
  existing no-Evidence development/demo mode, or by a future
  certificate-authenticated encrypted route mode. That mode should be distinct
  from `unattested_allowed` because it still establishes a protected session
  and a MITM-resistant peer identity.
- HTTP and WebSocket transports should not learn protected-RPC internals such
  as application object ids, method ids, interface ids, decrypted payloads, or
  application-domain result codes. Their security role is to authenticate and
  protect the direct stream, expose the resulting session state, enforce stream
  resource limits, and leave route/reference authorization to
  `rpc::enclave_service`.
- Direct non-RPC HTTP endpoints eventually need their own API-level policy:
  which routes require attestation, which can use certificate-authenticated
  encryption only, which are public, and what public failure information may be
  returned to the caller. This should be specified before adding attested REST
  or browser-facing diagnostic endpoints.

### Handshake Protocol Identity And Canonical Encoding

- Handshake messages should not be versioned primarily by free-form strings
  such as `name.v2`. Each transmitted handshake blob should carry the generated
  IDL fingerprint for the exact message schema, the selected `rpc::encoding`,
  and the exact payload bytes.
- Multi-message handshakes should be grouped by an IDL-rooted protocol file,
  not by a separate runtime suite string or suite-id field. For example,
  SGX-v1, SGX-v2, TDX-v1, SEV-SNP-v1, TrustZone/PSA-v1, and
  certificate-route-v1 handshakes can each live in their own IDL file with
  their own request/response/challenge/verdict structures. The ordered set of
  generated message fingerprints is the protocol identity.
- New handshake IDL files must put protocol messages under an `[inline]`
  namespace such as `v4`. A breaking wire-schema change should bump that
  inline namespace and therefore change every affected generated fingerprint;
  an extra in-struct `wire_version` field is unnecessary when the carrier
  already includes `(type_id, payload_encoding, payload bytes)`.
- MCP, A2A, and similar agent/orchestration surfaces should be integrated by
  exposing clear IDL field descriptions, generated fingerprints, and generated
  schema metadata. They can use that metadata to interpret service identity,
  Evidence blobs, media/content-format labels, and policy results, but they do
  not define the attestation handshake protocol themselves.
- JSON-facing RPC services must represent opaque binary fields from these
  handshake structures as base64 strings. Custom binary security formats should
  therefore appear as base64 when carried through JSON. If exact floating-point
  fidelity is security-relevant for a JSON-facing type, the field should be
  described as canonical IEEE-754 bytes carried as base64, or replaced with an
  integer/fixed-point representation; security checks must not rely on lossy
  JSON number round-trips.
- Transcript hashes, signatures, MACs, and KDF context must bind each message
  type fingerprint, payload encoding, exact transmitted payload bytes, role or
  direction, nonce material, and backend/security-level context where
  applicable. Implementations must not parse and reserialise a non-canonical
  message and then sign or verify the reserialised bytes.
- Unknown generated fingerprints are compatibility failures, not fraud by
  themselves. They should fail closed as `INVALID_VERSION` or an equivalent
  unsupported-schema result because a newer peer may be using a newer IDL file
  or inline namespace. Fraud classification is reserved for authenticated
  tamper, replay, downgrade, and impossible protocol sequencing, since fraud
  handling may feed blacklists.
- YAS and protobuf remain valid negotiated RPC/application encodings, and may
  be valid handshake payload encodings when both endpoints explicitly support
  them. They are not automatically canonical cryptographic encodings. Any
  serializer whose output is signed, MACed, hashed into a transcript, or fed
  into KDF context must either bind exact transmitted bytes or be profiled as
  canonical with test vectors.
- Add a future generator track for a restricted security-IDL profile that can
  emit canonical attestation/control bytes. The preferred candidates are:
  - ASN.1 DER where interoperability with X.509, keys, certificate extensions,
    or existing crypto tooling is useful;
  - a simpler Canopy canonical security encoding for internal handshake
    structures where DER's ASN.1 complexity is not justified.
- A DER/security encoder is not a replacement for YAS or protobuf in normal
  Canopy RPC. It should reject unsupported IDL constructs, enforce bounded
  fields, and ship with golden byte-level vectors before it is used for
  security decisions.

### Documentation

- After each phase, update the relevant design doc in this directory if
  the implementation revealed something the design did not anticipate.
- `dcap-operations.md` gains the operator runbook content from phase 4.
- `wire-format.md` gains the concrete OID for the SGX RA quote
  extension once pinned in code.

## Risks And Mitigations

- **TLS library limits.** Mbed TLS and SGXSSL may not expose hooks
  Canopy needs for custom certificate extensions. Mitigation: prototype
  the verify callback in phase 3 against both backends; if one cannot
  support it, restrict the SGX-attestation profile to the working
  backend rather than block the project.
- **AESM availability in containers.** AESM serialisation can be a
  performance ceiling. Mitigation: phase 7 includes a per-process
  semaphore and dashboards; production deployments should not be
  surprised by AESM behaviour.
- **PCCS provisioning friction.** Every developer who needs phase 4
  must register their machine with a PCCS at least once. Mitigation:
  the phase-4 runbook covers this, and developers without SGX can stay
  on phase-2 paths via the FAKE backend.
- **CMW spec churn.** `draft-ietf-rats-msg-wrap` is itself a draft.
  Mitigation: the CMW representation Canopy uses internally is just a
  triple `(media_type, content_format, payload)`; if the wire encoding
  changes when the draft stabilises, only the framing module is
  affected.
- **Phase ordering.** Each phase requires the previous to be merged.
  Mitigation: the gates are real (exit criteria) and CI enforces them.
  If business priorities reorder phases, document the deviation here
  rather than silently skipping criteria.

## See Also

- [Overview](overview.md)
- [Attestation Backends](attestation-backends.md)
- [Wire Format](wire-format.md)
- [DCAP Operations](dcap-operations.md)
- [Protected RPC Envelope](protected-rpc-envelope.md)
- [Failure Policy](failure-policy.md)
