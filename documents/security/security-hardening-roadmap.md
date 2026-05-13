<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Security Hardening Roadmap

Canopy is an RPC system where object references can cross process, network,
plugin, and enclave boundaries. The security model must therefore treat every
remote reference as a capability and every transport input as hostile until it
has been authenticated, deserialised, authorised, and checked against current
route and lifetime state.

This page is a planning document for the security pivot. It records the attack
classes that apply to Canopy and the design points where hardening can be
injected. It is not a claim that these mitigations are fully implemented.

## Threat Model Summary

The attacker may control a remote zone, a network peer, a plugin, a host process
outside an enclave, a shared queue, or a compromised intermediary zone. The
attacker may send syntactically invalid data, syntactically valid but
unauthorised protocol messages, stale messages, replayed messages, route-building
messages for zones it does not own, or carefully timed requests intended to
infer secret state.

SGX protects enclave memory from direct host reads and writes. It does not make
the host zone trustworthy, it does not authenticate remote zones, and it does
not provide availability. An enclave zone must be able to use a host zone for
routing without disclosing application data, keys, object contents, or
capability validation material beyond the routing metadata required to deliver a
message.

## Security Injection Points

Canopy already has useful places to insert security policy:

- Derived `service` implementations can override outbound calls such as
  `outbound_send`, `outbound_post`, `outbound_add_ref`, and
  `outbound_release` before messages reach the transport.
- Derived transports can add authenticated framing, encryption, replay
  protection, peer identity checks, and boundary-specific validation before
  calling the base `inbound_*` paths.
- `zone_address` carries an optional trailing validation block. That block can
  hold a typed HMAC, signature, certificate-bound capability, proof token, or
  future proof format.
- Passthrough creation and route lookup are centralised enough to enforce
  parent/child topology rules, route authority, and reference-count symmetry.
- Connection handshakes can bind a zone identity, transport session key,
  attestation evidence, and accepted policy before any ordinary object call is
  dispatched.

Security logic should prefer these existing extension points over special cases
inside generated user code.

## Encrypt After Serialise And Compress

The preferred data pipeline is:

```text
typed call data
  -> generated binding validation
  -> serialisation
  -> optional compression
  -> encryption and authentication
  -> transport framing
```

Compression should happen before encryption. Encryption deliberately removes
the structure compression needs, so compressing ciphertext is ineffective and
can leak policy mistakes by encouraging plaintext to be exposed elsewhere.

The frame should separate routing headers from protected payload bytes:

- visible header: protocol version, direction, message kind, source zone,
  destination zone, route epoch or session id, sequence number, request id, and
  encrypted payload length
- authenticated associated data: every visible header field and the negotiated
  security policy
- ciphertext: the serialised and optionally compressed RPC payload, including
  method arguments, return values, back-channel entries, capability validation
  material, and reference lifecycle details that an intermediary does not need
  to inspect
- authentication tag: produced by the AEAD algorithm over the header associated
  data and ciphertext

For an enclave zone using an untrusted host zone as a router, the host should
see only the visible routing header and ciphertext. It should not see object
contents, method payloads, validation HMACs, session keys, or attestation-bound
capability material. The routing header remains visible because the host or
passthrough may need it to forward the frame, but it must be authenticated so
the host cannot redirect or rewrite the message undetected.

If Canopy later supports per-hop and end-to-end protection at the same time,
the end-to-end payload encryption should be inside the hop encryption. A
passthrough could then authenticate and route the outer frame without learning
the inner application payload.

## Composable Processing Layers

The stream stack should treat security as a chain of byte-processing layers
around the existing `stream` abstraction, not as a monolithic transport rewrite.
Each layer should have a narrow contract:

- RPC binding layer: validates typed method metadata, serialises arguments and
  return values, and deserialises only after lower layers have accepted the
  frame
- compression layer: transforms serialised plaintext bytes and records the
  selected algorithm in authenticated metadata
- security layer: performs TLS, RA-TLS, or Canopy AEAD framing, owns sequence
  numbers, authenticates headers, and encrypts payload bytes
- routing layer: exposes only the header fields needed by TCP, SPSC, SGX host
  queues, passthroughs, or future io_uring-backed delivery
- physical stream layer: moves bytes but does not make security decisions

Inbound processing runs the reverse order:

```text
transport bytes
  -> frame length and cheap bounds checks
  -> authentication and decryption
  -> decompression
  -> deserialisation
  -> semantic validation and dispatch
```

No service, transport, passthrough, proxy, or stub state should be mutated until
the inbound frame has passed the layers that protect its integrity. The layer
composition also needs a negotiated policy object so both peers agree on
encoding, compression, encryption, certificate requirements, attestation
requirements, maximum sizes, and replay windows.

This design leaves room for multiple security profiles:

- unauthenticated local testing streams
- mutually authenticated TLS streams for ordinary host zones
- RA-TLS streams where an enclave presents attestation evidence in the TLS
  certificate or handshake transcript
- Canopy-native AEAD frames over SPSC or custom transports when TLS is too heavy
  or cannot see the right routing boundary
- nested end-to-end encryption inside hop-by-hop transport security

## TLS And RA-TLS In SGX Enclaves

TLS inside an enclave means the TLS endpoint, private key, session keys,
handshake transcript, certificate validation, and decrypted application bytes
live inside enclave memory. The untrusted host may provide sockets, queues,
io_uring submission, or OCALL-backed reads and writes, but it must only see TLS
records or outer routing headers.

RA-TLS extends this by binding remote attestation evidence to the TLS identity.
The enclave generates or owns an ephemeral TLS keypair, asks SGX quote generation
to sign or report the public key hash, and places the quote or evidence chain in
the certificate extension or handshake-visible credential. The peer verifies the
normal TLS transcript and then verifies that the certificate key is bound to an
enclave quote that matches the expected measurement, signer, product id,
security version, debug policy, and TCB collateral.

Possible implementation shape:

```text
enclave service
  -> rpc::stream_transport
  -> compression stream, optional
  -> enclave TLS or RA-TLS stream
  -> enclave/host I/O shim
  -> host TCP, SPSC, or io_uring delivery
```

The TLS or RA-TLS stream must sit inside the enclave side of the boundary. A host
TLS wrapper protects traffic on the network, but it does not protect an enclave
from the host because plaintext exists in host memory after TLS termination.

Implementation options:

- SGX SSL or another enclave-safe OpenSSL build can provide the familiar TLS
  API, but it increases enclave trusted code size and must be built with enclave
  flags
- mbedTLS or wolfSSL-style enclave ports may be smaller, but require checking
  certificate, TLS 1.3, RA-TLS extension, and constant-time support
- a Canopy-native AEAD record layer is smaller than TLS and may fit SPSC or
  passthrough routing better, but it does not give standard TLS interoperability
- host-side OpenSSL remains useful for non-enclave zones and for tests, but it
  must not hold enclave TLS private keys or decrypted enclave payloads

RA-TLS policy needs to be explicit:

- which measurements, signers, products, and security versions are accepted
- whether debug enclaves are allowed
- whether DCAP collateral must be fresh and whether soft TCB warnings are
  accepted
- how certificate chains interact with attested ephemeral keys
- how the attested identity maps to Canopy zone identity and authorisation
- how sessions are resumed, rotated, or rejected after enclave restart

The secure stream constructor policy is the first non-attested version of that
shape. Server contexts support `peer_verification::none`, `optional`, and
`required`:

- browser-facing websocket servers normally use `none`
- peer-to-peer and RA-TLS-capable listeners should use `required` once the peer
  identity is expected
- `optional` is for transitional listeners: no certificate is accepted, but a
  presented certificate must verify

The `trust_anchor` in TLS credentials is the peer-validation root material. It
must be supplied when a server context enables `optional` or `required` peer
verification. It is not a substitute for RA-TLS measurement or collateral
policy; it is only the certificate-chain trust input that later RA-TLS policy
can combine with quote verification.

RA-TLS does not remove the need for Canopy capability checks. It authenticates
the peer endpoint and can prove that the endpoint is an expected enclave build;
it does not by itself decide which object references, methods, passthrough
routes, or `add_ref`/`release` operations that peer is allowed to use.

## Deferred SGX Web Services Security Concerns

The current SGX websocket and mbedTLS work is a functional stepping stone. The
following concerns are intentionally deferred to the next security branch or
sprint and should be resolved before treating the enclave web service path as a
production security boundary:

- TLS identity is not enclave identity until it is bound to RA-TLS or DCAP
  evidence. Normal TLS proves key possession only; it does not prove the peer
  measurement, signer, product id, security version, debug policy, or TCB state.
- Host-backed file-system data is untrusted. Path traversal checks protect the
  host file namespace shape, but they do not make file contents, directory
  listings, timestamps, or I/O errors trustworthy. Security-sensitive static
  data needs embedding, sealing, measurement, signatures, or another integrity
  check.
- TLS private keys must not be provisioned through ordinary host files for
  production SGX deployments. Production keys should be generated inside the
  enclave or delivered over an attested provisioning channel, then sealed if
  persistence is required.
- Certificate validity checks that depend on host-supplied time are weak under
  the SGX threat model. Production policy needs freshness from attestation
  nonces, verifier policy, trusted collateral validation, or another trusted
  time/freshness source.
- HTTP, websocket, TLS, stream, and scheduler paths need explicit resource
  limits: header bytes, body bytes, websocket frame/message bytes, connection
  counts, handshake timeouts, queued coroutine counts, and file read sizes.
- WebSocket transports should grow protocol-level ping/pong liveness handling.
  Server-side protocol pings let browser clients respond automatically with
  pongs, help detect half-open connections, and give the transport a clean
  deadline for closing idle or unresponsive peers.
- SGX side channels remain in scope. Enclave memory protection does not hide
  timing, access patterns, page faults, traffic shape, queue pressure, or
  availability from the host.
- Production logging must avoid leaking keys, decrypted payloads, attestation
  material, peer identifiers, capability material, or other security-sensitive
  data to host-visible logs.

## Invalid Serialisation Formats

Attack:

- send an invalid encoding enum, unknown message kind, invalid interface ordinal,
  truncated frame, oversized frame, malformed `zone_address` blob, invalid
  validation block length, or a payload whose declared type does not match the
  expected method schema
- trigger parser crashes, allocator exhaustion, object confusion, or partially
  applied protocol state

Mitigation:

- authenticate stream frames before deserialising payloads when the boundary is
  hostile
- reject unknown encoding values and unknown protocol message kinds before
  dispatch
- impose hard per-frame and per-call size limits
- parse into bounded temporary objects and mutate service, transport,
  passthrough, or stub state only after the whole message has passed structural
  and semantic checks
- require generated bindings to validate interface ordinals, method ordinals,
  type fingerprints, back-channel entry types, and `zone_address::from_blob`
  results before constructing proxies or stubs
- maintain negative tests and fuzzers for frame parsing, address parsing,
  serializer decoding, and generated binding entry points

## Reference Protocol Abuse

Attack:

- call `add_ref` repeatedly to inflate owner-side counts or keep objects alive
  indefinitely
- call `release` too many times to underflow counts or trigger premature object
  destruction
- send `add_ref`, `release`, or `object_released` for an object id the caller
  never received
- claim a `caller_zone_id`, `requesting_zone_id`, or destination zone that does
  not match the authenticated peer or active route
- abuse optimistic references to bypass the shared-reference ownership rule

Mitigation:

- treat references as capabilities, not plain object ids
- maintain per-caller, per-object, per-reference-kind state and reject underflow
  as fraud
- require `release` and `object_released` to match a known outstanding reference
  or an explicitly documented idempotent terminal state
- bind `caller_zone_id` and `requesting_zone_id` to the authenticated transport
  adjacency or to an authorised passthrough route
- distinguish normal adjacent-zone reference counts from route-building
  passthrough counts
- enforce a maximum reference count, maximum outstanding references per peer,
  and maximum passthrough count per route as denial-of-service controls
- ensure destructor-triggered cleanup still passes through service outbound
  virtuals so policy checks apply during shutdown

## Malicious Passthroughs

Attack:

- create a route through an intermediary zone without authority from both
  endpoint zones
- reuse a stale passthrough after one side has released its route
- make a passthrough point to two transports that do not match the claimed
  caller and destination
- use `requesting_zone_id` to route through a zone that should not be in the
  trust path
- leak protected payloads to a host or intermediary that only needed routing
  metadata

Mitigation:

- require passthrough creation to be authorised by authenticated route-building
  messages
- validate that the claimed endpoint zones match the transports or existing
  passthroughs used to reach them
- sign or MAC route tokens so an intermediary can route only the pair and
  direction it was delegated
- keep route metadata separate from encrypted application payloads
- destroy or disable a passthrough when either endpoint transport fails, the
  route count reaches zero, or the authenticated route epoch changes
- audit all combined `build_destination_route | build_caller_route` flows for
  balanced release and route teardown

## Timing Attacks

Attack:

- infer object existence, interface support, authorisation status, or secret
  data from response latency, error differences, scheduling behaviour, queue
  pressure, retry counts, or enclave page/cache effects
- use host scheduling of enclave worker ECALLs to observe progress and correlate
  it with secret-dependent code

Mitigation:

- avoid making authentication, HMAC, signature, and capability comparisons
  return early
- return coarse, policy-level errors across trust boundaries rather than
  distinguishing every internal failure
- separate secret-dependent application work from host-visible diagnostics,
  logging, and queue pressure where possible
- rate-limit unauthenticated and failed-authentication traffic
- for enclave code, assume the host can observe ECALL/OCALL timing, queue
  occupancy, and worker progress; do not treat SGX as protection against all
  side channels
- document which APIs need constant-time or time-oblivious implementations
  before accepting them as enclave-safe

## Authentication Attacks

Attack:

- connect as an unauthenticated zone
- replay an old handshake or resume a stale session
- bind a session key to one peer and use it with another
- downgrade framing, encoding, encryption, attestation, or certificate policy
- impersonate a host zone to an enclave or impersonate an enclave to a remote
  peer

Mitigation:

- make every hostile transport perform an authenticated handshake before normal
  RPC traffic
- bind the session key to both zone identities, transport direction, protocol
  version, negotiated encoding set, compression mode, and attestation policy
- use nonces and monotonically increasing sequence numbers to prevent replay and
  reordering
- reject unauthenticated `add_ref`, `release`, passthrough, and connection
  messages
- protect handshakes against downgrade by authenticating the negotiated feature
  set
- maintain explicit key rotation and session expiry rules for long-lived zones

## Authorisation Attacks

Attack:

- authenticate correctly but call a method, object, interface, or route outside
  the caller's authority
- pass a reference to a third zone that should not be allowed to receive it
- use `try_cast` to discover or obtain interfaces outside policy
- call privileged lifecycle methods such as release, route creation, or
  transport-down from a zone that lacks that authority

Mitigation:

- define authorisation around object capabilities: possession of a valid
  reference token grants only the operations encoded in that token
- validate method-level and interface-level policy in generated stubs or a
  service policy layer before implementation code runs
- validate transfer policy when generated bindings marshal remote interface
  parameters between zones
- require passthrough and route delegation to carry explicit endpoint, epoch,
  direction, and reference-kind authority
- treat `try_cast` as an authorisation boundary, not just a type query
- make service-level outbound hooks able to deny sending references, releases,
  route-building messages, and ordinary calls by policy

## Untrusted Client Profile

Not every peer should receive the full Canopy object-reference protocol. A web
browser, mobile app, public API client, or unauthenticated peer should usually
see a flat application protocol rather than the general distributed object graph.

Recommended restrictions for untrusted clients:

- expose a small root interface or gateway object, not arbitrary service
  internals
- return value objects, opaque handles, or short-lived tokens instead of general
  `rpc::shared_ptr` references wherever possible
- do not allow client-originated passthrough creation
- do not allow clients to choose arbitrary `caller_zone_id`,
  `requesting_zone_id`, object ids, route epochs, or validation blocks
- hide `add_ref`, `release`, `object_released`, `try_cast`, and transport
  lifecycle controls behind server-owned policy
- cap outstanding requests, callbacks, handles, and subscriptions per client
- expire client handles independently of distributed reference counts
- translate internal errors into coarse public API errors

For web clients, a safer shape is:

```text
web client
  -> flat request interface
  -> server gateway zone
  -> internal Canopy object graph

web client
  <- flat callback/event interface
  <- server gateway zone
```

The gateway zone owns the real Canopy references and maps them to client-visible
handles. The client can ask the gateway to perform application operations, but
it does not directly own remote object lifetimes in trusted zones. Callback
interfaces should be similarly constrained: the client may register a callback
channel or subscription, but the server owns the internal reference, cancellation
rule, timeout, and backpressure policy.

This profile is especially important when trusted zones include enclaves. An
untrusted host or web client should never be able to fabricate enclave object
ids, force enclave passthroughs, or directly control enclave object lifetime.
The enclave should see either an authenticated internal zone or a gateway
capability issued by one, not raw public-client routing claims.

Concrete current example: `c++/demos/websocket/server` is a useful demonstration
of the boundary that needs this profile. It accepts HTTP/WebSocket clients and
uses `websocket_protocol::transport::create` to expose
`i_calculator` while optionally accepting an `i_context_event` callback sink
from the browser side. Before treating this as a production-facing pattern, the
server should be hardened as a public gateway:

- expose only the calculator or application gateway methods intended for web
  clients
- wrap the browser callback sink as a bounded subscription instead of treating it
  as an unconstrained trusted Canopy object
- keep internal Canopy references owned by the server-side gateway service
- reject client-controlled zone ids, object ids, validation blocks, and route
  construction metadata
- disable passthrough creation and arbitrary `try_cast` from the web-client
  transport
- require TLS for network confidentiality, and use enclave-terminated TLS or
  RA-TLS if the target implementation object lives inside an enclave
- add per-client limits for message size, outstanding calls, callback traffic,
  subscription lifetime, and authentication failures
- ensure REST and static-file paths cannot be used to leak internal object ids,
  diagnostics, model paths, keys, or enclave measurements beyond policy
- translate internal RPC errors into coarse web API errors

The web gateway should be allowed to use the rich Canopy reference protocol on
the trusted side. The public side should look like a small request/callback API,
not like a general-purpose remote object fabric.

## Attestation Attacks

Attack:

- claim to be running enclave code without providing valid evidence
- replay an old quote from a different enclave instance
- bind a valid quote to the wrong session key
- accept an enclave with the wrong measurement, signer, product id, security
  version, debug mode, or stale TCB collateral
- allow an untrusted host zone to terminate TLS or inspect plaintext for an
  enclave zone

Mitigation:

- add a remote attestation handshake for enclave zones before application RPC
  traffic starts
- support a DCAP-oriented verifier path first for modern SGX deployments, while
  keeping EPID as a design discussion only if a deployment still needs it
- bind the attestation quote or report data to the ephemeral session public key
  and the Canopy zone identity
- validate measurement, signer, product id, security version, debug flag, TCB
  status, collateral freshness, and application policy
- derive transport encryption keys only after quote verification succeeds
- store accepted peer enclave identities in the service or transport security
  context so authorisation can use them

Open design decision: attestation can live at the transport handshake layer, at
a service security layer, or as a separate attestation service used by both. The
transport must at least expose the authenticated peer identity and session keys;
the service must be able to apply policy to the attested identity. A separate
attestation module is preferable for DCAP/EPID plumbing so ordinary transports
do not hard-code SGX policy.

## Zone Address Validation And Capability Tokens

The `zone_address` validation block is the natural place to bind routing and
object identity to cryptographic authority. A useful first scheme is a typed
HMAC over:

- validation type and version
- zone address type, routing prefix, port, subnet, and object id
- capability scope: zone-only, object-call, add-ref, release, passthrough, or
  transfer
- caller zone and destination zone when the token is not bearer-only
- interface ordinal, method ordinal, and reference kind when the token is
  operation-specific
- issue time or epoch, expiry, and key id

The HMAC key should come from local zone secrets, an enclave-sealed secret, or a
session key derived after mutual authentication or attestation. For cross-zone
verification without shared symmetric keys, the validation block can carry a
signature or a certificate-chain-bound token instead.

Important rule: `zone_only()` and `with_object()` currently drop validation.
That is useful as a compatibility shim, but a secure capability scheme must
define when validation is stripped, recomputed, or rejected. Code that creates a
new object-bearing address from a zone-only address must not accidentally turn a
zone routing hint into authority to call an object.

## Enclave Communication Model

An enclave zone should treat the host zone as an untrusted router. The host may
allocate buffers, enter worker ECALLs, move queue bytes, and forward packets, but
it must not be trusted with application plaintext or capability secrets.

Recommended model:

- run authenticated and encrypted framing inside the enclave, above any
  host-owned queue or socket machinery
- expose only routing headers required by the host to deliver frames
- encrypt payload bytes after serialisation and optional compression
- keep payloads, reference validation tokens, session keys, and application data
  encrypted from the host
- bind host-visible routing metadata into authenticated associated data so the
  host cannot redirect ciphertext without detection
- treat queue corruption, invalid sequence numbers, invalid tags, and impossible
  route state as fraud or transport failure

The planned io_uring-in-enclave work should preserve this split. If the enclave
owns the io_uring submission/completion logic, the host may still control kernel
progress and availability, but the enclave can keep parsing, encryption,
sequence checks, and capability validation inside the trust boundary.

For io_uring transfers, distinguish caller buffers from host buffers. Caller
buffers are the per-call spans supplied by the stream or RPC layer; inside an
enclave these are enclave-private and cannot be submitted directly to the
kernel. Host buffers are fixed-size host-visible slots owned by the io_uring
handle and can be used for kernel SEND/RECV operations. Those host buffers may
hold ciphertext, but plaintext must be encrypted and authenticated before it is
copied there. Record sizing must include encryption overhead such as nonce,
tag, padding, and any frame header because large logical messages may be split
across multiple fixed-size host buffer slots.

## Denial Of Service

Attack:

- exhaust memory with oversized frames, reference counts, passthroughs, pending
  requests, pending route placeholders, or diagnostic traffic
- starve an enclave by withholding worker ECALLs or not draining queues
- repeatedly force expensive failed authentication or attestation paths

Mitigation:

- impose quotas per transport, peer zone, object, route, and unauthenticated
  connection
- apply cheap syntactic checks before expensive cryptographic work when that can
  be done without trusting the input
- rate-limit failed handshakes and failed capability validations
- define fatal enclave shutdown for fraudulent host control-plane input
- keep high-volume telemetry out of hot paths, especially when telemetry shares
  the same SPSC stream as protocol traffic

## Hardening Work Order

1. Define the security context object carried by transports and visible to
   service outbound hooks: authenticated peer zone, attested enclave identity,
   session id, key id, policy flags, and route epoch.
2. Add authenticated stream framing with sequence numbers, direction binding,
   associated data, payload length limits, optional compression before
   encryption, and replay rejection.
3. Define validation block types for `zone_address`, starting with HMAC over
   zone/object/capability fields.
4. Enforce reference capability checks in `add_ref`, `release`,
   `object_released`, and passthrough creation.
5. Add mutual authentication for non-enclave transports and remote attestation
   for enclave transports.
6. Add authorisation policy hooks for method calls, `try_cast`, reference
   transfer, and route delegation.
7. Add negative tests and fuzzers for malformed frames, address blobs, reference
   underflow, passthrough fraud, replay, and downgrade attempts.
8. Review enclave code for timing, logging, queue pressure, and host-visible
   side-channel assumptions.
