# Wire Protocol Refcount Plan

Date: 2026-04-18

Purpose:

- turn the current hierarchical transport refcount investigation into a protocol-specification task
- define the ownership and lifetime semantics needed for:
  - future non-C++ implementations
  - corner-case debugging
  - validation of malformed or hostile traffic

## Goals

1. Codify the wire-level meaning of reference-management operations.
2. Separate transport, stub, service, and passthrough responsibilities clearly.
3. Document topology-dependent cases:
   - direct parent/child
   - tree routing
   - Y-topology / identity-repair cases
4. Make add-ref and release symmetry explicit.
5. Identify protocol gaps where current fields are not sufficient.

## Scope

The plan should cover:

- `add_ref_params`
- `release_params`
- `object_released_params`
- directionality of marshalled interface parameters:
  - in
  - out
  - in-out
  - return values
- pointer kinds:
  - shared
  - optimistic
- route-building semantics:
  - normal
  - `build_destination_route`
  - `build_caller_route`
  - `build_destination_route | build_caller_route`
- topology metadata including `requesting_zone_id`

## Deliverables

1. Reference Protocol Overview
   - one document describing the intent of each wire operation
   - terminology for caller zone, destination zone, adjacent zone, requesting zone

2. State Transition Matrix
   - a matrix covering the practical permutations:
     - pointer kind
     - route mode
     - parameter direction
     - direct vs routed topology
   - for each case, state:
     - who owns the reference after the operation
     - which local counters move
     - what release must undo

3. Topology Notes
   - direct path examples
   - intermediate tree path examples
   - Y-topology examples where identity and `requesting_zone_id` matter

4. Validation Rules
   - what a receiver may trust
   - what must be checked
   - what combinations should be rejected as invalid or suspicious

5. Gap List
   - places where the current protocol lacks enough metadata to make ownership or release behavior unambiguous

## Proposed Work Order

1. Document the current fields and their actual meaning in code.
2. Define a canonical ownership model independent of the current implementation.
3. Map current implementation behavior onto that model.
4. List mismatches and ambiguities.
5. Propose minimal protocol extensions where needed.

## Questions To Answer

1. When is a reference transport-owned versus service-proxy-owned versus stub-owned?
2. When an object is passed as:
   - in param
   - out param
   - in-out param
   - return value
   what ownership is created in each zone?
3. How should routed handoff differ from ordinary direct ownership?
4. What exact problem is `requesting_zone_id` solving in:
   - identity repair
   - Y-topology routing
   - tree handoff
5. Can a release always be interpreted from current wire data alone?
6. If not, what extra protocol data is required?

## Practical Next Documents

- `reference-protocol-overview.md`
- `reference-protocol-matrix.md`
- `reference-topology-examples.md`
- `reference-validation-rules.md`
- `reference-protocol-gaps.md`

## Constraints

- avoid encoding behavior that is only accidental in the current C++ implementation
- prefer protocol semantics over local implementation detail
- keep the model usable by other language runtimes
- keep security validation in view, not just happy-path ownership

## Immediate Follow-Up

- use the current investigation notes as raw input
- extract the 12-permutation matrix explicitly
- write the ownership transitions before attempting further fixes
