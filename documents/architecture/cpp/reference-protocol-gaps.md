# Reference Protocol Gaps

Date: 2026-04-18

## Purpose

This document lists protocol gaps discovered during the hierarchical transport refcount investigation.

## Gap 1: Release Does Not Preserve Route Intent

Current state:

- `add_ref_params` carries route intent through `add_ref_options`
- `release_params` only carries `release_options`
- `release_options` only distinguishes shared vs optimistic

Problem:

- release cannot reliably tell whether it is unwinding:
  - direct ownership
  - destination-only setup
  - caller-only setup
  - downstream handoff

Impact:

- release-side symmetry is reconstructed by inference
- this is fragile in routed topologies

## Gap 2: Destination-Only Has More Than One Meaning

Current state:

- `build_destination_route` can represent:
  - true destination-facing ownership
  - a split leg of a routed handoff

Problem:

- current wire data does not always distinguish those meanings cleanly

Impact:

- transport-owned counts can overlap with existing service-proxy lifetime accounting

## Gap 3: Caller-Only Ownership Is Under-Specified

Current state:

- caller-only semantics exist on add-ref
- there is no equally rich release-side representation

Problem:

- if caller-only is to map to stub-side transport ownership, release must be able to say so explicitly

## Gap 4: Service-Proxy Lifetime Versus Per-Object Ownership Is Implicit

Current state:

- service-proxy create/destroy already moves transport lifetime state
- per-object add-ref/release may also move transport lifetime state

Problem:

- the protocol does not declare when an object-level event is already covered by service-proxy lifetime

Impact:

- double-counting and double-decrement become likely

## Gap 5: Identity And Y-Topology Provenance Is Not Fully Codified

Current state:

- `requesting_zone_id` exists and is important

Problem:

- the protocol does not yet define exactly when it is mandatory, optional, or ignored

Impact:

- non-C++ implementations will not know how strongly to rely on it

## Gap 6: Hostile Or Malformed Traffic Rules Are Not Written Down

Current state:

- code contains assertions and local assumptions

Problem:

- there is no canonical document saying what to reject, what to log, and what to tolerate

Impact:

- behavior may differ across runtimes

## Candidate Minimal Extensions

These are candidates, not final protocol decisions.

1. Add release-side route intent mirroring the add-ref route mode.
2. Distinguish direct destination ownership from routed destination-leg setup.
3. Define explicit ownership class:
   - direct
   - reverse-route
   - handoff
4. Make `requesting_zone_id` requirements topology-specific and normative.

## Next Design Decision

Before more transport fixes are attempted, the protocol should define:

- whether ownership class is explicit on the wire
- or whether current fields are enough if interpreted more strictly
