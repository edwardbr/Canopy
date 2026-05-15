<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Protected RPC Envelope

## Goal

Canopy keeps the existing `i_marshaller` polymorphic shape while making RPC
payloads confidential and tamper-evident end-to-end between the caller enclave
and destination enclave.

The transport still carries familiar fields such as `caller_zone_id`,
`remote_object_id`, `interface_id`, `method_id`, `in_data`, `request_id`, and
back-channel vectors. For protected messages, the sensitive call data moves
inside an encrypted envelope stored in `in_data`, `payload`, or `out_buf`.

Intermediate zones route by visible fields and public back-channel context.
They do not decrypt or rewrite the protected application payload. A future
policy may let intermediates validate public integrity tokens, but that is an
optional route-policy feature and requires a separately specified trust/key
model.

## Relationship To TLS Authentication

TLS authenticates only the adjacent peer. On a direct connection that may be
the same party as the caller or callee. On a routed path such as `A -> B -> C`,
TLS authenticates A-to-B and B-to-C hops, but it does not by itself prove A's
end-to-end identity to C.

The protected envelope is responsible for:

- end-to-end confidentiality across multi-hop routes where intermediates
  must forward bytes without reading them;
- end-to-end integrity that is not weakened by an intermediate
  re-terminating TLS;
- binding the decrypted caller and destination fields to an attested
  end-to-end session;
- replay protection scoped to the end-to-end zone pair rather than the
  adjacent transport.

For single-hop direct attestation, TLS-layer protections may be sufficient for
the adjacent link, depending on policy. The protected envelope still matters
when the same RPC payload may later be forwarded through routing zones that
the original caller did not directly authenticate.

See [overview.md, "Binding Modes"](overview.md) and [wire-format.md](wire-format.md)
for how adjacent TLS attestation differs from routed RPC attestation.

## Outer Shape

For a protected `send`:

```text
outer send_params:
    protocol_version
    encoding_type
    tag                  = protected envelope tag/version
    caller_zone_id       = visible routing caller
    remote_object_id     = visible destination route, object id cleared
    interface_id         = rpc::id<encrypted_payload>::get(version)
    method_id            = 0
    in_data              = YAS-binary encrypted_payload carrier
    in_back_channel      = public route/security/telemetry context
    request_id           = transport_request_id for adjacent correlation
```

The existing field is still named `request_id` in `send_params`. The protected
RPC design calls the outer value `transport_request_id` to distinguish it from
the encrypted service-level request id.

`interface_id` is used here as a type discriminator, not as a normal interface
implementation id. `interface_id == 0` remains the unset/invalid sentinel and
is not a protected-envelope marker. `method_id == 0` means the receiver must
not dispatch the request directly; it must unwrap the protected payload first.

For encrypted traffic, intermediates must not see a valid application
`method_id`. The real object id, interface, and method live inside the
encrypted plaintext. The public `remote_object_id` is a route carrier only:
it preserves the destination zone and clears the object id before transmission.

The current IDL carrier is:

```text
struct encrypted_payload
{
    std::string session_id;
    uint64_t session_epoch;
    uint64_t e2e_counter;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> authentication_tag;
}
```

The carrier is serialized with YAS binary for the current implementation and
stored in `in_data` for requests or `out_buf` for protected responses.
`session_id`, `session_epoch`, and `e2e_counter` are public cryptographic
metadata: the receiver needs them to locate the session context and construct
the nonce before decryption. They are also bound into AEAD associated data and
repeated in the encrypted plaintext so tampering is detected.

`authentication_tag` is the AES-GCM authentication tag, not a public-key
signature. A second payload-format type id inside the struct is not needed
unless a future format has multiple nested payload families that cannot be
distinguished by the outer type, protocol version, encoding, or tag.

Back-channel vectors are public mutable metadata. Intermediates may append
entries such as OpenTelemetry context while forwarding a protected request or
response. Therefore protected RPC must not include the outer back-channel
vector in AEAD associated data, must not compare it with a sender snapshot, and
must pass the received outer vector to the next layer. Security-relevant
back-channel entries that need integrity must carry their own issuer identity
and signature/MAC inside that entry.

## Protected Method Visibility

The visibility rule is the same for all protected marshaller operations:
intermediates may see only the route fields required to forward the message and
public back-channel context. Method-specific details belong inside the
encrypted payload unless an intermediate genuinely needs them for routing.

```text
send/post:        outer route fields visible; object/interface/method/data encrypted
try_cast:         target route visible; object id and requested interface id encrypted
add_ref:          route fields visible; current route-control option visible and AEAD-bound
release:          route fields visible; release_options visible and AEAD-bound
object_released:  route fields visible; released object details encrypted
transport_down:   end-to-end protected form deferred
```

The current concrete outer carrier for call-like traffic is
`send_params`/`post_params`. Protected `try_cast`, `add_ref`, and `release`
use their `payload_type_id` / `payload` fields as encrypted payload carriers.
Protected `object_released` may need a method-specific carrier or a control-RPC
encoding before implementation. Protected traffic must not expose valid
application method ids to intermediates.

Current implementation status: protected `send` and `post` have concrete
AES-GCM envelope helpers and generated-RPC runtime coverage through
`rpc::enclave_service`. Host integration tests observe generated `send`,
protected `send` response, generated `[post]`, generated `try_cast`, generated
`add_ref`, and generated `release` traffic over `rpc::stream_transport` and
verify that those messages carry encrypted `rpc::encrypted_payload` blobs
rather than plaintext application calls. The same tests verify that protected
outer `remote_object_id` values carry only the destination zone and do not
expose object ids to intermediates.
`try_cast`, `add_ref`, and `release` also have AES-GCM protected payload carriers in
`payload_type_id` / `payload`; `rpc::enclave_service` wraps outbound
reference-control traffic when an attested adjacent context exists and unwraps
inbound protected reference-control traffic before route validation. Because
the current transport route construction reads `build_out_param_channel`
before the service hook receives an `add_ref`, that field remains visible in
this implementation and is authenticated as AEAD associated data plus repeated
inside the encrypted plaintext. The streaming transport also carries
`release_options` as public lifetime-control data; protected release binds that
visible value as AEAD associated data and repeats it inside the encrypted
plaintext. Hiding those route-control fields requires a later transport-route
refactor.

`add_ref` has the first policy gate in `rpc::enclave_service`: routes can be
attested, explicitly allowed unattested, failed, or marked as handshaking, and
unknown routes trigger the route-addressed `handshake()` path before failing
closed. That handshake now has generated RPC/YAS request and response payloads
for fake Evidence, backend-neutral identity, transcript id, nonce, backend id,
security level, and a structured accept/reject verdict. SGX-sim host
integration tests now drive this path through `rpc::stream_transport` from an
unknown-route `add_ref`, including both mutual fake attestation and explicitly
permitted unattested-client policy. Protected inbound `release` does not start
a new route-attestation handshake: if the caller route is unknown, failed, or
still handshaking, the release fails closed because the corresponding protected
`add_ref` should already have established the route state.
Protected `try_cast` follows the existing call route and encrypts the requested
interface id. Its current transport response still returns the `standard_result`
error code in the clear; that leaks less useful metadata once the interface id
is hidden, but a future response carrier can close that remaining metadata
leak if policy requires it.

## Plaintext Payload

The encrypted plaintext includes the original call fields:

```text
protected_plaintext:
    protected_kind       = send | post | try_cast | add_ref | release | ...
    protocol_version
    encoding_type
    tag
    caller_zone_id
    remote_object_id
    interface_id
    method_id
    in_data
    in_back_channel       = sender snapshot only; outer received vector is authoritative
    transport_request_id
    service_request_id
    e2e_counter
    session_id
    session_epoch
    selected private context
```

The destination decrypts this plaintext, validates it, and then passes the
recovered request to the internal implementation or generated stub.

The first implementation sets `service_request_id` to zero. The field is kept
in the protected plaintext for the later service-level binding work.

The attestation backend id and security level are normally properties of the
session rather than repeated in every payload. Validation must still ensure that
the session was created by a backend allowed for the requested policy.

## Response Envelope

For `send`, the response path mirrors the request path. The returned `out_buf`
contains encrypted response data from the call site. The encrypted response
should bind:

```text
original request session_id
original request e2e_counter
response e2e_counter
caller zone
callee zone
transport_request_id
service_request_id if present
error_code
out_buf
out_back_channel
selected private response context
```

The caller service decrypts the protected `out_buf`, validates the binding, and
returns the recovered result to the proxy.

For protected responses, `out_back_channel` remains outer public mutable
metadata. It is not part of the AEAD request/response binding. Private response
context must be placed inside the encrypted payload rather than in the
back-channel vector.

## Request Ids

Use separate terms for the two request-id scopes:

```text
transport_request_id = public adjacent transport/service correlation id
service_request_id   = encrypted end-to-end service or call binding id
```

The current `send_params` field is named `request_id`; in the outer protected
message that field is interpreted as `transport_request_id`. It is public
metadata and should be bound into AEAD associated data or the encrypted
plaintext so the receiver can detect tampering.

`service_request_id` is carried inside the encrypted protected payload when the
call path needs end-to-end service binding, out-param binding, or response
correlation beyond the adjacent transport hop.

## Counters And Nonces

The existing transport `sequence_number` is monotonic per adjacent streaming
transport and is used for request/response matching. The existing service
`request_id` is monotonic for pending out-param/add-ref binding and can be zero
for ordinary sends.

Neither is sufficient as the general end-to-end replay counter.

The preferred default is to derive AEAD keys per:

```text
enclave-pair session -> caller zone -> destination zone -> direction
```

Each derived key has its own monotonic counter scoped to that key and epoch.
This prevents two zone pairs inside one enclave-pair session from reusing the
same AES-GCM `(key, nonce)` pair.

The counter lives in the enclave-wide attestation/security service, not in a
single transport object, because multiple transport hops or routes may carry
messages for the same end-to-end zone pair. The counter resets only when the
session epoch and key material are fresh. Never reuse an AES-GCM key with a
reset counter.

If a future design uses one AEAD key for an entire enclave-pair session, it
must instead use enclave-pair-wide counters or put the zone pair into a
collision-free nonce domain separator. That is not the preferred default.

## KDF Encoding

Wire-facing attestation records may use Canopy IDL, YAS, or protocol buffers.
Key-derivation and transcript-binding inputs must not depend on ordinary
serializer output unless that serializer is explicitly profiled as canonical
for cryptographic use.

The implementation uses a small Canopy Attestation v1 canonical KDF encoding:

- fixed protocol id: `Canopy-Attestation-v1`;
- big-endian fixed-width integers;
- length-prefixed byte strings;
- length-prefixed identity fields;
- explicit labels for each KDF use;
- HKDF-SHA256 through the platform crypto library.

This keeps KDF inputs unambiguous across C++, JavaScript, and future TEE
backends. Any change to this encoding changes the cryptographic protocol and
must be covered by golden-vector tests.

## Encryption

AES-GCM is the provisional algorithm. This is acceptable only if nonce
uniqueness is enforced rigorously.

The preferred default is separate AEAD keys per direction. In that shape,
direction is an input to key derivation and is not also needed as a nonce bit.

Use a 96-bit AES-GCM nonce. The exact split is implementation-specific, but it
must include a per-key fixed nonce prefix or equivalent domain separation plus
the monotonic counter:

```text
nonce = per-key nonce prefix || monotonic e2e counter
```

The counter must not wrap for a live key. If the counter space is exhausted,
the session must rekey before sending another protected message.

## Validation At Destination

After decrypting a protected request, the destination checks:

- protected envelope version is supported;
- session id maps to an attested enclave-pair session;
- e2e counter is fresh and monotonic for that session/direction;
- inner caller zone belongs to the attested remote enclave;
- inner destination object resolves locally;
- visible routing fields are compatible with the decrypted fields;
- service policy allows the remote enclave/zone to call the requested
  interface and method.

If validation fails, the message is fraudulent or malformed. Handling is policy
driven; see [Failure Policy](failure-policy.md).

## Control Messages

`transport_down` and similar liveness/control messages may need different
trust treatment from ordinary application payloads. In a route `A -> B -> C`,
if C crashes and B is A's only channel, A must rely on B to report the route
failure.

That trust should be scoped narrowly. B's statement should mean "my route to C
is down" unless stronger attestation policy makes B authoritative for more.

The end-to-end protected `transport_down` from a zone is deferred. The
route-layer `transport_down` synthesized by an intermediate is a separate,
narrowly scoped message that may trigger the same service upcall but does not
prove the downstream enclave's internal state.

Whether future protected control messages use distinct envelope structs or a
single generic protected marshaller envelope with a `protected_kind` enum is
deferred.

## Legacy Peers

If policy requires protected envelopes and a peer does not support them, return
`ZONE_NOT_SUPPORTED` for now. Fallback to plaintext is allowed only when policy
explicitly permits it.
