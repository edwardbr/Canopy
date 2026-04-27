<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Reference Protocol Security

Canopy object references are capabilities. A proxy or stub reference is not just
a convenience pointer; it represents authority to call, release, or forward an
object across a zone boundary.

## Attack Vectors

### Forged Add-Ref

Attack:

- request an add-ref for an object id the caller was never given
- request a back-channel reference with forged caller or destination metadata

Mitigation:

- validate caller zone and destination zone
- validate object ownership before creating a proxy or stub mapping
- reject unknown object ids as fraud when they are security-sensitive

### Forged Release

Attack:

- release an object the caller does not own
- cause premature destructor execution
- trigger host-interface release from inside enclave at the wrong time

Mitigation:

- releases must match known references
- reference underflow is a protocol violation
- destructor-triggered releases must run before transport teardown
- release handling should be idempotent only where the protocol explicitly
  permits it

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

Mitigation:

- validate route creation against known parent/child relationships
- decrement passthrough counts on both sides of release
- reject route use after either endpoint is gone

## Security Invariants

- Every remote object reference must have an owning zone and an authorised
  caller.
- Reference count transitions must never underflow.
- Object destructors may send security-relevant releases and must be allowed to
  run during clean shutdown.
- A transport must remain alive until release call stacks have unwound.
- A failed transport and a clean reference drain are different security states.

Related protocol documents:

- [Add-Ref Protocol](../protocol/add-ref.md)
- [Release Protocol](../protocol/release.md)
- [Zone Shutdown Sequence](../protocol/zone_shutdown_sequence.md)

