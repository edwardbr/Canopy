# Reference Topology Examples

Date: 2026-04-18

## Purpose

These examples describe the intended ownership and routing meaning for direct, tree, and Y-topology cases.

## Example 1: Direct Parent Child

```text
Zone 1 <-> Zone 2
```

Case:

- zone 2 holds a remote proxy to an object in zone 1

Expected behavior:

- add-ref uses `normal`
- zone 2 owns direct destination-facing state
- zone 2 transport may hold `proxy_count` toward zone 1
- zone 1 holds stub-side state for zone 2

## Example 2: Tree Out Parameter

```text
    Zone 1
   /     \
Zone 2  Zone 3
```

Case:

- zone 2 receives an object that actually lives in zone 3
- zone 1 is the intermediate zone

Desired semantics:

1. zone 1 may establish a route toward zone 3
2. zone 1 may establish a reverse route toward zone 2
3. zone 1 should not automatically become the long-term owner of the handed-off reference
4. zone 2 and zone 3 should establish their own steady-state ownership

Relevant route mode:

- `build_destination_route | build_caller_route`

This is the canonical downstream handoff case.

## Example 3: Tree Split Legs

```text
Zone 2 -> Zone 1 -> Zone 3
```

During forwarding, the combined handoff may be observed as two separate legs:

- destination-only leg toward zone 3
- caller-only leg toward zone 2

Important rule:

- splitting the handoff into two legs does not automatically mean the intermediate zone owns both legs long-term

This is where current implementation ambiguity appears.

## Example 4: Y Topology

```text
      Zone 1
     /     \
 Zone 2   Zone 3
     \     /
      Zone 4
```

Case:

- reference traffic may travel through a shared ancestor or repaired path
- caller and destination identities are not enough on their own to distinguish the intended route relationship

Why `requesting_zone_id` matters:

- it identifies the route context that caused the operation
- it allows a zone to decide whether a route should be created, reused, or rejected
- it reduces the risk of confusing:
  - direct ownership
  - reverse-route setup
  - identity repair traffic

## Example 5: In-Out Parameter Replacement

```text
caller provides object A
callee returns object B
```

This must be treated as two transitions:

1. unwind or preserve the incoming ownership for object A
2. establish outgoing ownership for object B

If the protocol treats this as one combined state change, release symmetry becomes ambiguous.

## Example 6: Return Value Through Intermediate Zone

```text
Zone 2 <- Zone 1 <- Zone 3
```

Case:

- zone 3 returns an object to zone 2 through zone 1

Desired semantics:

- zone 1 may help create route state
- zone 2 becomes the eventual holder
- zone 3 remains the destination owner
- zone 1 should not retain ownership after the handoff unless another independent reason exists

## Topology Rules

1. Intermediate routing does not imply intermediate ownership.
2. Reverse-route preparation is not the same as destination-facing proxy ownership.
3. Identity repair requires explicit provenance metadata.
4. Service-proxy lifetime and per-object reference ownership must be modeled independently.
