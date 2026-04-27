# Reference Validation Rules

Date: 2026-04-18

## Purpose

These rules describe what a runtime may trust, what it must validate, and what combinations should be treated as invalid or suspicious.

## Trust Model

A receiving zone must not assume that caller-provided metadata is semantically valid merely because it is well-formed.

It may trust:

- transport adjacency
- local routing table state
- whether the addressed object is local

It must validate:

- whether caller, destination, and requesting zone make sense together
- whether route mode is legal for the topology
- whether a release corresponds to known ownership

## Required Checks

### On Add Ref

1. `remote_object_id` must be set and syntactically valid.
2. `caller_zone_id` must be set for non-local reference traffic.
3. `requesting_zone_id` must be set when routed or repaired topology semantics are required.
4. `build_caller_route` must not be accepted blindly as destination-facing ownership.
5. `build_destination_route | build_caller_route` must not automatically create long-term ownership in an intermediate zone.

### On Release

1. release must map to an ownership state that is actually known locally
2. release must not decrement a bucket that was never incremented
3. unknown releases should be distinguishable as:
   - benign stale cleanup
   - protocol mismatch
   - suspicious traffic

### On Object Released

1. the sender must be plausible relative to known optimistic holders
2. the receiver must not treat object-released as equivalent to release

## Suspicious Patterns

The following should be treated as suspicious or at least diagnostically important:

- repeated release for unknown ownership
- release arriving with impossible caller/destination relationship
- route-building combinations that do not match known topology
- attempts to create reverse routes without a plausible requesting zone
- attempts to convert handoff traffic into direct ownership without a matching downstream state transition

## Recommended Runtime Responses

- reject structurally invalid traffic
- log semantically inconsistent but non-fatal traffic
- avoid asserting on untrusted route metadata in production paths
- preserve enough context in logs to reconstruct topology and intent

## Security-Oriented Principle

If the runtime cannot prove that a release corresponds to a real local ownership state, it should not destroy ownership as if it had proven that fact.
