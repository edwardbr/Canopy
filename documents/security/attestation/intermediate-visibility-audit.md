# Intermediate Visibility Audit

This note records what an intermediate transport, passthrough, or route zone
can currently see in protected RPC traffic, and what it actually needs in
order to forward that traffic.

The audit conclusion is:

- application object ids are not needed by intermediates;
- real application `interface_id` values are not needed by intermediates;
- real application `method_id` values are not needed by intermediates;
- payload bytes and return values are not needed by intermediates;
- public back-channel entries are intentionally visible and mutable;
- route endpoints, carrier type, and narrowly scoped route-control state remain
  visible.

The current protected envelope already follows the first three rules for
protected `send`, `post`, `try_cast`, `add_ref`, `release`, and
`object_released`. `transport_down` has protected endpoint-originated parsing
support but no production outbound sender yet. It also has an intentionally
plaintext route-layer form because an intermediate may have to report that its
route to a downstream zone failed.

Most current runtime assertions exercise stream transports. That is not the
same as proving every coroutine-capable transport is covered. Local transports
can be compiled for enclaves and can link zones inside the same enclave; those
paths forward marshaller parameters directly between parent/child transports
instead of serialising a stream envelope. Treat them as a separate local-route
audit surface. Generic `rpc::transport` final-method guardrails now sanitise
public `handshake()` and `get_new_zone_id()` statuses for all transports using
the shared `rpc::error` public-control status helper, but that does not replace
the local marshaller-operation audit.

The current enclave-local wrapper coverage exercises generated `send`, `post`,
`try_cast`, `add_ref`, `release`, `object_released`, plaintext route-layer
`transport_down`, wrapper creation, and child-service creation over the marked
local route. The generic `rpc::local` transports remain attestation-neutral;
the enclave-specific wrappers identify the in-enclave next hop so the
`rpc::enclave_service` policy can validate the referenced endpoint route.

## Classification

Fields are classified as:

- **Public route**: required by transports or passthroughs to choose the next
  hop.
- **Public carrier**: required by the receiver to identify or authenticate the
  protected payload carrier, but not application data.
- **Public mutable**: intentionally readable and appendable by intermediates.
- **Public route-control**: currently visible because the transport or
  passthrough uses it for route or lifetime accounting.
- **Encrypted**: endpoint-only data recovered from the protected plaintext.
- **Residual leak**: visible today but not needed by intermediates.
- **Local-only**: should not cross a route boundary.

## Field Rules

### Zone Identity

`caller_zone_id` and destination route zone are public route fields. Current
transport and passthrough forwarding code uses the pair of caller zone and
destination zone to choose or create the route.

The destination route zone is carried as either:

- `remote_object_id.as_zone()` for object-addressed operations; or
- `destination_zone_id` for route-addressed operations.

Only the zone part is needed by intermediates. The object-id bits are endpoint
data.

### Object Ids

Object ids are encrypted endpoint data for protected traffic. Intermediates
route on the destination zone only. The public `remote_object_id` for a
protected message should therefore have object id zero or an equivalent
route-only representation.

The destination endpoint reconstructs the full `remote_object_id` from the
decrypted plaintext and verifies that the visible route-only destination is
compatible with it.

### Interface And Method Ids

Real application `interface_id` and `method_id` values are encrypted endpoint
data for protected traffic.

For `send` and `post`, the public `interface_id` is only the
`rpc::encrypted_payload` carrier fingerprint and the public `method_id` is the
protected outer method sentinel. For `try_cast`, the requested interface id is
inside the encrypted payload. This means intermediates can identify "this is a
protected carrier" but cannot identify the application interface or method.

### Back Channels

Back-channel vectors are public mutable metadata. Intermediates may append
entries such as telemetry context. Protected RPC must not compare the received
back-channel vector with the sender's encrypted snapshot, and must not include
the outer back-channel vector in AEAD associated data.

Security-sensitive back-channel entries need their own issuer identity and
signature or MAC inside the entry payload.

### Counters And Request Ids

`session_id`, `session_epoch`, and `e2e_counter` in `rpc::encrypted_payload`
are public carrier fields. They are needed by the receiver to find the session,
derive the AEAD key, build the nonce, and reject replay. They are not
application data.

`send_params::request_id` is currently interpreted as a public
`transport_request_id` in protected traffic. It is a transport correlation
value, not the end-to-end service request id. The protected plaintext reserves
`service_request_id` for future endpoint-only semantics.

### Error Codes

Application result codes only apply to `send` in the RPC model. `post` is
one-way and has no response code. Protected `send` encrypts the real
`send_result::error_code` inside the protected response, including positive
application-domain values. The public outer response error is only the carrier
status. If that public carrier status is non-OK it must be a built-in
`rpc::error::*` value; a positive application-domain value in the public
carrier is invalid.

`standard_result::error_code` on `try_cast`, `add_ref`, and `release` is a
transport/control result. It should be either `rpc::error::OK()` or a value in
the built-in `rpc::error::*` range. Positive values on these control paths are
not valid application results; they should be treated as invalid data or a
protocol/security error rather than forwarded to an intermediate.

## Operation Matrix

| Operation | Required public fields | Currently public carrier/control fields | Encrypted fields | Residual exposure |
| --- | --- | --- | --- | --- |
| `send` | caller zone, destination zone | protocol version, fixed protected tag, fixed carrier encoding, transport request id, encrypted payload session/counter, public back-channel | object id, real interface id, method id, application tag, application encoding, input data, sender back-channel snapshot, service request id | transport request id and session/counter timing metadata |
| `send` response | return route from transport call | outer carrier success code, encrypted payload session/counter, public out-back-channel | real result error code, output buffer, sender out-back-channel snapshot | response timing and carrier success/failure |
| `post` | caller zone, destination zone | protocol version, fixed protected tag, fixed carrier encoding, encrypted payload session/counter, public back-channel | object id, real interface id, method id, application tag, application encoding, input data, sender back-channel snapshot | session/counter timing metadata |
| `try_cast` | caller zone, destination zone | protected payload type id, carrier interface id, encrypted payload session/counter, public back-channel, RPC control result | object id, requested interface id, sender back-channel snapshot | positive result codes must not appear on this path |
| `add_ref` | caller zone, destination zone | requesting zone, request id, `build_out_param_channel`, protected payload type id, encrypted payload session/counter, public back-channel, RPC control result | object id, payload, sender back-channel snapshot | requesting zone and route-control options are public until route construction is refactored; positive result codes must not appear on this path |
| `release` | caller zone, destination zone | `release_options`, protected payload type id, encrypted payload session/counter, public back-channel, RPC control result | object id, payload, sender back-channel snapshot | release option is public until passthrough lifetime accounting is refactored; positive result codes must not appear on this path |
| `object_released` | caller zone, owner route zone | protected payload type id, encrypted payload session/counter, public back-channel | released object id, payload, sender back-channel snapshot | no protected response body; timing still visible |
| protected `transport_down` | caller zone, destination zone | protected payload type id, encrypted payload session/counter, public back-channel | endpoint-originated private payload and sender back-channel snapshot | route failure timing visible |
| route-layer `transport_down` | caller zone, destination zone | public back-channel | none | intentionally unauthenticated route-liveness statement unless policy adds hop trust |
| `handshake` | caller zone, destination zone | handshake type id, payload, public back-channel, RPC control result | none in current route-level bootstrap | attestation evidence and verdict metadata are visible to intermediates on routed handshakes; positive result codes must not appear |
| stream sign-on | adjacent stream endpoints | initial remote object, expected interface ids, explicit connection encoding, adjacent zone id, RPC control result | none in current stream setup | initial descriptors are visible until stream setup is bound to attested transport policy |
| stream close | adjacent stream endpoints | empty close/ack control messages | none | no payload by construction |
| `post_report` | route to reporting service | telemetry event | none | intentionally diagnostic; needs separate redaction policy |
| `get_new_zone_id` | route to allocator/root | RPC control result, allocated zone id, public back-channel | none | allocator influence and returned zone id need policy review; positive result codes must not appear |

## Non-Marshaller Traffic

Protected RPC interposes between generated/internal proxies and transports,
and between destination transports and stubs. That leaves a small set of
transport/service communications that are intentionally outside normal
application dispatch and need separate policy:

- **Stream connection setup.** `init_client_channel_send`,
  `init_client_initial_channel_response`, and `init_client_channel_response`
  establish adjacency, initial remote objects, expected interface ids, and the
  explicit connection encoding selected by the initiating service. In an
  attested stream this belongs to the stream sign-on path. For routed
  post-connect references, the subsequent `add_ref` route-attestation path must
  validate the referenced zone before application code can use it. The current
  runtime tests observe these setup messages and assert that their public
  status fields remain RPC control statuses.
- **Route handshakes.** `i_marshaller::handshake()` carries attestation
  request/response payloads. It is bootstrap/control traffic, not application
  RPC. Intermediates may see its route fields and currently may see the
  attestation payload unless a later privacy layer is added. Stream transport
  now sanitises routed handshake statuses so positive application-domain codes
  cannot be emitted as public handshake results, and tests assert that only the
  generated route-attestation request/response type ids appear on this path.
- **Route liveness.** Plaintext route-layer `transport_down` is allowed only as
  a scoped statement by an intermediate about the route it controls. It must
  not be interpreted as a protected statement from the failed downstream
  enclave. Runtime tests keep this route-layer form plaintext; a protected
  endpoint-originated sender should be added only when a real production call
  site needs it.
- **Telemetry and diagnostics.** `post_report` is intentionally diagnostic.
  It needs a redaction policy distinct from application payload encryption.
- **Zone allocation.** `get_new_zone_id` is allocator/root control traffic.
  It should not carry application payloads. Stream transport sanitises its
  public result so only `OK()` or negative RPC control errors can be observed,
  and failures do not carry allocator back-channel data or a zone id. Future
  policy must bind any allocation decision to enclave/route identity.
- **Stream close.** `close_connection_send` and `close_connection_ack` are
  transport lifecycle messages. They should carry no application object,
  interface, method, or payload data. They are empty wire structs in
  `stream_transport.idl`, and runtime tests observe the close path.
- **Local in-enclave routing.** `rpc::local::parent_transport` and
  `rpc::local::child_transport` can connect zones inside an enclave without a
  stream envelope. They directly call peer `inbound_*` handlers for `send`,
  `post`, `try_cast`, `add_ref`, `release`, `object_released`, and
  `transport_down`, and the parent side forwards `get_new_zone_id` to the
  parent zone. These generic transports should stay policy-light: a local link
  between two zones in the same enclave is not itself an attestation boundary.
  The enclave layer should add enclave-specific local transport wrappers or
  adapters if it needs to enforce attestation policy on local routing. In the
  A/B/C/D forwarding case, where B and C are local zones in one enclave but A
  passes an interface owned by D through B to C, B-to-C does not need
  attestation. C still has to preserve and validate the true referenced route
  zone D before application code can use that interface. The adjacent local
  peer B is only the next hop, not the security subject. The generic transport
  guardrail now prevents positive public `get_new_zone_id` statuses on the
  local path, and runtime coverage includes that parent-side local allocator
  case. `rpc::sgx::coro::enclave::local_child_transport` and
  `rpc::sgx::coro::enclave::local_parent_transport` mark this enclave-local
  next-hop case without changing generic `rpc::local` semantics. The generic
  local child transport now has an overridable child-side parent-transport
  factory; the default still constructs `rpc::local::parent_transport`, while
  the enclave-local wrapper constructs a marked `local_parent_transport` during
  real `connect_to_zone` child creation. The marked parent transport then
  creates an `rpc::enclave_service` for that child zone instead of a plain
  `rpc::child_service`. Outbound `add_ref` and `release` over those marked
  transports validate the referenced owner route instead of the adjacent local
  peer. Runtime coverage also checks generated `send`, generated `post`,
  protected `try_cast`, protected `object_released`, and plaintext route-layer
  `transport_down` over the marked local route.

Any new transport message outside this list should be treated as suspicious
until it is classified as public route/control metadata, encrypted endpoint
payload, or local-only state.

## Current Residual Leaks

The following data is still visible but is not required as application data by
intermediate transports:

- `add_ref_params::build_out_param_channel`;
- `add_ref_params::requesting_zone_id`;
- `release_params::options`;
- route-attestation `handshake_params::type_id` and payload on routed
  handshakes;
- stream setup `init_client_channel_send` initial remote object and interface
  ids;
- endpoint-originated protected `transport_down` has encrypted payload carrier
  helpers and inbound unwrap support, but no production outbound service hook
  or sender currently uses it;
- telemetry/logging fields when intermediate telemetry is enabled.

These are not all equally serious. The most important distinction is that none
of them should expose application object ids, real application method ids, real
application interface ids, or encrypted application payload bytes.

`add_ref_params::build_out_param_channel`, including the optimistic flag, and
`release_params::options` are still live passthrough lifetime-accounting inputs.
They must remain public while passthroughs maintain separate shared and
optimistic route counts. Protected RPC therefore authenticates these fields as
AAD and repeats them in encrypted plaintext for endpoint binding checks. Hiding
them is not part of the current plan; it would require a replacement
route-lifetime protocol, not just field zeroing.

## Recommended Next Refactors

1. Extend the control-status guardrail to any future marshaller operation that
   gains a public response status. Current regression coverage checks protected
   `send` response carriers plus `try_cast`, `add_ref`, and the one-way
   `release` outbound hook.
2. Add a protected `standard_result` carrier for `try_cast`, `add_ref`, and
   `release` only if those control paths later gain legitimate positive
   application-domain results. For the current model they should expose only
   `OK()` or built-in `rpc::error::*` values.
3. Keep `add_ref` route construction public where passthroughs need it. Today
   the direction bits, requesting zone, and optimistic flag are live inputs
   before the endpoint can decrypt; the object id is already hidden for
   protected traffic.
4. Keep `release_options` public and authenticated while passthroughs maintain
   separate shared and optimistic counts. Hiding the `add_ref` optimistic bit,
   `release_options`, or `object_released` count state is not a standalone
   refactor; changing only one of them would unbalance route lifetime
   accounting.
5. Decide whether stream sign-on descriptors need privacy beyond the
   peer-to-peer attested stream policy. If they do, initial object/interface
   descriptors need their own setup envelope before `add_ref` can validate
   routed references.
6. Audit telemetry callbacks and logs so intermediate telemetry records only
   public route fields and carrier metadata, never decrypted endpoint fields.
7. Keep coroutine dynamic-library transports on the audit list if they are ever
   allowed at an enclave boundary. They pass full `rpc::*_params` structs
   across a dynamically loaded boundary and are therefore external transport
   surfaces, not enclave-local trusted links. The C ABI work is intentionally
   excluded from this slice because that implementation is expected to be
   rewritten for Rust.
8. Decide whether routed attestation handshakes need privacy beyond integrity.
   Evidence is often intended to be verifiable, but policy may still consider
   measurements, backend identifiers, debug flags, or verdict detail sensitive.

## Test Expectations

Runtime observers for protected traffic should fail if they see:

- a non-zero public object id in protected object-addressed operations;
- a real application method id in protected `send` or `post`;
- a real requested interface id in protected `try_cast`;
- plaintext application payload bytes in protected request or response
  carriers;
- a positive `standard_result::error_code` on protected control paths;
- back-channel mutation causing AEAD failure.

The observer should allow:

- public caller and destination route zones;
- public mutable back-channel append operations;
- protected payload type fingerprints;
- public encrypted-payload session and counter metadata;
- public `get_new_zone_id` allocator control results and allocated route zone
  ids;
- plaintext route-layer `transport_down` when generated by an intermediate.
