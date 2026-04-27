# Reference Protocol Matrix

Date: 2026-04-18

## Purpose

This matrix describes the practical reference-management permutations relevant to current Canopy routing.

The useful matrix dimensions are:

- parameter direction
- pointer kind
- route mode
- topology shape

The current investigation focuses most strongly on the 12 practical cases formed by:

- 3 ownership shapes:
  - ordinary direct ownership
  - destination-side route setup
  - caller-side route setup
- 2 pointer kinds:
  - shared
  - optimistic
- 2 transfer classes:
  - ordinary object ownership
  - out-param / routed handoff

`build_destination_route | build_caller_route` is listed separately because it is semantically special.

## Canonical Interpretation

### Ordinary Direct Ownership

| Pointer | Route mode | Intended owner | Transport bucket | Matching release |
| --- | --- | --- | --- | --- |
| shared | `normal` | direct holder zone | `proxy_count` | remove direct proxy ownership |
| optimistic | `optimistic` | direct holder zone | `proxy_count` if transport chooses to track optimistic route holds, otherwise none | remove matching optimistic ownership only |

Notes:

- this is the cleanest case
- service-proxy lifetime still exists separately

### Destination-Side Route Setup

| Pointer | Route mode | Intended owner | Transport bucket | Matching release |
| --- | --- | --- | --- | --- |
| shared | `build_destination_route` | destination-facing holder | candidate for `proxy_count` only if not already covered by service-proxy lifetime | remove destination-facing ownership only |
| optimistic | `build_destination_route | optimistic` | destination-facing holder | same rule as shared case, but optimistic semantics | remove optimistic destination-facing ownership only |

Notes:

- this is the most ambiguous current case
- some destination-only events are ordinary direct holds
- others are split legs of routed out-param handoff
- this case must not double-count against existing service-proxy lifetime

### Caller-Side Route Setup

| Pointer | Route mode | Intended owner | Transport bucket | Matching release |
| --- | --- | --- | --- | --- |
| shared | `build_caller_route` | caller-facing reverse path | candidate for `stub_count` | remove caller-facing ownership only |
| optimistic | `build_caller_route | optimistic` | caller-facing reverse path | candidate for `stub_count` if optimistic reverse-route state is tracked | remove optimistic caller-facing ownership only |

Notes:

- semantically this aligns much more closely with stub-side ownership than proxy-side ownership
- current code does not encode this on `release_params`

### Downstream Handoff

| Pointer | Route mode | Intended owner | Transport bucket | Matching release |
| --- | --- | --- | --- | --- |
| shared | `build_destination_route | build_caller_route` | downstream zones, not intermediate zone | no long-term intermediate ownership | release must not remove intermediate ownership that was never acquired |
| optimistic | `build_destination_route | build_caller_route | optimistic` | downstream zones, optimistic semantics | no long-term intermediate ownership | same rule as shared handoff |

Notes:

- this is the core out-param handoff case
- current protocol relies on downstream zones to establish their own local state

## Parameter-Direction View

### In Parameter

- caller already owns the reference
- destination may need stub-side state
- ordinary direct tracking is acceptable

### Out Parameter

- callee creates or forwards a reference to caller
- routed topologies may require destination-side setup, caller-side setup, or both
- this is where handoff semantics matter most

### In-Out Parameter

- consists of:
  - release or replacement of incoming ownership
  - creation or forwarding of outgoing ownership
- must be modeled as two logical transitions, not one merged one

### Return Value

- semantically equivalent to an out parameter
- any protocol distinction should be treated as framing only, not ownership semantics

## Topology View

### Direct

- caller and destination are directly connected
- `normal` is usually sufficient

### Tree

- intermediate parent participates
- destination-only and caller-only legs may be split across different transports

### Y Topology

- caller and destination may share an ancestor without sharing a direct line
- `requesting_zone_id` is needed to repair identity and select correct route semantics

## Current Known Ambiguities

1. `release_params` does not preserve route mode.
2. `destination_only` can mean:
   - direct ownership
   - forwarded leg of a handoff
3. `caller_only` has no explicit release-side bucket today.
4. optimistic routing semantics are under-specified relative to shared routing semantics.

## Current Safe Interpretation

Until the protocol is extended, the safest interpretation is:

- `normal`
  - direct proxy ownership
- `build_destination_route`
  - destination-facing preparation, but only owner-tracked if clearly not already covered elsewhere
- `build_caller_route`
  - caller-facing preparation, candidate for stub-side ownership
- `build_destination_route | build_caller_route`
  - downstream handoff, not intermediate long-term ownership
