<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Reference Protocol Security

Canopy object references are capabilities. A proxy or stub reference is not just
a convenience pointer; it represents authority to call, release, or forward an
object across a zone boundary.

The existing reference protocol is routed through `service::outbound_add_ref`
and `service::outbound_release` before transport code sends messages to another
zone. Security extensions should preserve that path so derived services can
enforce capability, authentication, authorisation, and audit policy even during
destructor-triggered cleanup.

## Attack Vectors

### Forged Add-Ref

Attack:

- request an add-ref for an object id the caller was never given
- request a back-channel reference with forged caller or destination metadata
- inflate the owner-side count to keep a target object alive indefinitely
- use `requesting_zone_id` to force route discovery through a zone that should
  not be in the trust path

Mitigation:

- validate caller zone and destination zone
- validate object ownership before creating a proxy or stub mapping
- reject unknown object ids as fraud when they are security-sensitive
- bind the caller, requesting zone, destination, object id, and reference kind to
  an authenticated capability token
- apply per-peer and per-object limits to outstanding add-ref counts

### Forged Release

Attack:

- release an object the caller does not own
- cause premature destructor execution
- trigger host-interface release from inside enclave at the wrong time
- send repeated releases to underflow per-caller counts

Mitigation:

- releases must match known references
- reference underflow is a protocol violation
- destructor-triggered releases must run before transport teardown
- release handling should be idempotent only where the protocol explicitly
  permits it
- service outbound release hooks must remain in the path for late cleanup, so a
  secure service can verify release authority before a transport sees it

### Premature Transport-Down

Attack:

- report transport failure during a normal clean release cascade
- invalidate service proxies while destructors are still sending releases

Mitigation:

- distinguish clean refcount-zero shutdown from transport failure
- suppress `transport_down` for graceful close
- reserve `transport_down` for real failure where the peer cannot be trusted to
  complete release unwinding

### Passthrough Abuse

Attack:

- create a route through an intermediate zone without authority
- retain a passthrough after one side released its route
- use stale route data to reach objects outside the intended graph
- create a route that discloses payloads to a host or intermediary that should
  only see routing metadata

Mitigation:

- validate route creation against known parent/child relationships
- decrement passthrough counts on both sides of release
- reject route use after either endpoint is gone
- authenticate route-building messages and bind them to route epochs
- keep application payloads encrypted across untrusted passthrough zones
- when io_uring host buffers are used as staging memory, write only
  authenticated ciphertext records there; enclave plaintext remains in enclave
  caller buffers

### Forged Object-Released

Attack:

- send `object_released` to optimistic holders even though the owner object is
  still alive
- use a stale object release notification to make a proxy discard a usable
  reference

Mitigation:

- accept `object_released` only from the authenticated owner-side route
- match notifications to a known optimistic reference and object epoch
- treat unexpected notifications as fraud or transport failure across hostile
  boundaries

## Security Invariants

- Every remote object reference must have an owning zone and an authorised
  caller.
- Reference count transitions must never underflow.
- `add_ref`, `release`, and `object_released` must be bound to an authenticated
  zone identity or to a validated passthrough capability.
- Object destructors may send security-relevant releases and must be allowed to
  run during clean shutdown.
- A transport must remain alive until release call stacks have unwound.
- A failed transport and a clean reference drain are different security states.
- Possessing a route is not the same as possessing authority to call or transfer
  an object.
- Untrusted clients should not receive general authority over distributed object
  lifetime. Public gateways should translate client-visible handles and callback
  subscriptions into server-owned Canopy references.

## Public Client Boundary

Browsers, public API clients, and other low-trust peers should usually interact
with a flat gateway interface instead of the raw reference protocol. The gateway
can own `rpc::shared_ptr` references internally while exposing short-lived
client handles externally.

At this boundary:

- `add_ref` and `release` should be server-originated consequences of gateway
  state, not public client commands
- `object_released` should not be accepted from the public client side
- `try_cast` should be disabled or mapped to explicit public operations
- passthrough route creation should be unavailable
- callbacks should be registered as bounded subscriptions with server-owned
  cancellation, expiry, and backpressure

`c++/demos/websocket/server` is the concrete demo shape that should evolve in
this direction before it is used as a public-facing pattern. Its WebSocket
transport can expose a flat calculator/request API and a constrained callback
API while the server-side `websocket_service` owns any real Canopy references
and route state.

Related protocol documents:

- [Add-Ref Protocol](../protocol/add-ref.md)
- [Release Protocol](../protocol/release.md)
- [Zone Shutdown Sequence](../protocol/zone_shutdown_sequence.md)
