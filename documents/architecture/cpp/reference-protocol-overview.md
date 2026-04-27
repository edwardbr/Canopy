# Reference Protocol Overview

Date: 2026-04-18

## Purpose

This document defines the intended wire-level meaning of reference-management traffic in Canopy.

It is written to support:

- future non-C++ implementations
- analysis of hierarchical transport corner cases
- validation of malformed or hostile traffic

This document describes the protocol intent. It does not treat the current C++ implementation as automatically correct.

## Core Entities

- origin zone:
  - the zone where the current logical operation began
- caller zone:
  - the zone presented as the holder or releaser of a reference
- destination zone:
  - the zone that owns the target object
- adjacent zone:
  - the directly connected zone on a transport
- requesting zone:
  - the zone whose identity is used to disambiguate route setup in routed and repaired topologies

## Core Operations

- `add_ref_params`
  - requests creation or preparation of reference state
- `release_params`
  - requests the inverse of an already established ownership state
- `object_released_params`
  - signals that a shared reference count reached zero and optimistic-only users may need notification

## Pointer Kinds

- shared:
  - creates strong ownership
  - object lifetime must be preserved
- optimistic:
  - does not guarantee object survival
  - object may disappear before a later call

## Parameter Directions

- in parameter:
  - caller already owns the reference and is lending visibility to the callee
- out parameter:
  - callee creates or forwards a reference to the caller
- in-out parameter:
  - caller provides a reference and callee may replace or return a new one
- return value:
  - semantically similar to an out parameter, but framed as call result traffic

## Route Modes

The current wire model uses `add_ref_options` to express route intent.

- `normal`
  - ordinary direct ownership acquisition
- `build_destination_route`
  - prepare the path needed to reach the destination side
- `build_caller_route`
  - prepare the reverse path toward the caller side
- `build_destination_route | build_caller_route`
  - downstream handoff where intermediate zones help establish routes but should not necessarily own the reference

## Ownership Layers

Reference state exists at more than one layer. Those layers must not be conflated.

- object-proxy ownership:
  - local user-visible ownership of a remote object
- service-proxy lifetime:
  - transport-level hold that keeps a zone-to-zone route alive while service proxies exist
- stub ownership:
  - ownership on the destination side representing references held by remote callers
- passthrough route state:
  - a reference-counted join between two transports for intermediate forwarding
  - maintained by service proxies and routed object references that still depend on that join

## Canonical Counter Intent

At the transport level the current model has two ownership buckets and two passthrough buckets.

- `proxy_count`
  - local proxies referencing remote-zone objects
- `stub_count`
  - local stubs referenced by remote-zone proxies
- `outbound_passthrough_count`
  - passthrough joins using this transport to reach a destination
- `inbound_passthrough_count`
  - passthrough joins using this transport from a source

Protocol intent:

- destination-facing ownership belongs in `proxy_count`
- caller-facing ownership belongs in `stub_count`
- passthrough counts represent transport-join lifetime, not direct object ownership
- service-proxy lifetime and object-link lifetime may both contribute to whether a passthrough join must stay alive

## High-Level Rules

1. A zone must not infer ownership from topology alone when explicit route intent exists on the wire.
2. Route setup, passthrough join lifetime, and object ownership are related but distinct.
3. Intermediate zones may participate in route establishment without becoming long-term owners.
4. A release must only remove ownership that was actually acquired.
5. Service-proxy lifetime accounting must remain separate from per-object reference accounting.
6. Passthrough lifetime must remain separate from both direct ownership and transport-wide zone lifetime.

## Role Of `requesting_zone_id`

`requesting_zone_id` exists to disambiguate cases where caller and destination alone are insufficient.

This is especially important for:

- identity repair
- Y-topology routing
- routed out-parameter handoff
- cases where a zone must decide which downstream route is the intended one

Without `requesting_zone_id`, two topologically similar paths can be misinterpreted as the same ownership event.

## Intended Directional Meaning

- direct destination ownership:
  - the zone that will hold a remote object should acquire destination-facing ownership
- reverse-route preparation:
  - if later callbacks or releases must travel back, the caller-facing path may need preparation
- downstream handoff:
  - an intermediate zone may establish both directions while delegating eventual ownership to downstream zones

## Non-Goals

This document does not define:

- exact binary encoding layout
- transport framing bytes
- serialization details for payload buffers

Those belong in a lower-level wire-format document.
