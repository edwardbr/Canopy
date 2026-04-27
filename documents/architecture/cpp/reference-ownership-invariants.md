# Reference Ownership Invariants

Date: 2026-04-18

## Purpose

This document records the invariants that should govern routed reference ownership in Canopy.

It is intended to keep the logic simple:

- ownership transitions are isolated from each other
- add-ref and release do not need to infer historical intent from each other
- handoff is modeled as acquisition of a new ownership fact followed by release of an old ownership fact

## Core Principle

Each reference relationship must stand on its own.

An add-ref establishes a local ownership fact.

A release removes a local ownership fact.

The correctness rule is not:

- "release understands what add-ref used to mean"

The correctness rule is:

- "the ownership fact that release removes must already exist independently"

## Acquire-Before-Release Rule

Whenever a reference is being handed off from one relationship to another, the runtime must treat that as two separate transitions:

1. acquire the new relationship
2. only after that acquisition is established, release the old relationship

This applies even if both transitions occur in the same stack or as part of one logical operation.

They are temporally close, but semantically separate.

## Isolation Rule

Two distinct relationships must not be merged merely because they happen to involve the same object.

Examples of distinct relationships:

- parent zone -> destination zone
- child zone -> destination zone
- caller-facing reverse route
- destination-facing direct hold
- passthrough join lifetime between two transports

If two relationships are distinct, they must remain independently representable in local transport state.

## Original Corner Case

The motivating case is the `create_foo` / `create_local_zone` style stack-unwind path.

The intended sequence is:

1. a new add-ref links the parent with the remote destination zone
2. that new parent-to-destination relationship is confirmed in transport state
3. a release then removes the old child-to-child relationship
4. the destination remains alive because the parent-owned relationship already exists

This is correct only if:

- the parent-owned relationship and the child-owned relationship are isolated facts
- the release of the child-owned relationship cannot accidentally consume the parent-owned relationship

## Required Invariants

### Invariant 1: Independent Ownership Facts

If zone A and zone B have a new reference relationship, that relationship must exist independently of any prior relationship involving zone C.

The new relationship must not depend on the old relationship remaining present in order to be recognized as real.

### Invariant 2: Release Must Be Local And Exact

A release may only remove the local ownership fact that corresponds to the releasing relationship.

It must not:

- remove a different relationship that happens to target the same destination
- remove a relationship that was created earlier in the same higher-level operation
- remove a relationship merely because it shares route infrastructure

### Invariant 3: Passthrough Join Lifetime Is Separate

A passthrough is a real reference-counted join between two transports.

It is not the same thing as direct object ownership, and it is not the same thing as transport-wide zone lifetime.

It may be kept alive by service proxies and by routed object references that still need that join.

So:

- passthrough state is not a substitute for direct ownership state
- direct ownership teardown must not accidentally destroy a still-needed passthrough join
- passthrough teardown must only happen once no remaining references in either direction depend on that join

### Invariant 4: Direct Ownership Must Survive Teardown Of Replaced Links

If a direct or parent-owned relationship has been established, teardown of an older child-owned or passthrough-assisted relationship must not remove it.

### Invariant 5: Service-Proxy Lifetime Is Separate

Service-proxy lifetime keeps a zone-to-zone route alive.

Per-object reference ownership keeps a specific object relationship alive.

Those are related but not interchangeable.

The runtime must not double-count them or let one silently consume the other.

### Invariant 6: Transport Lifetime Is Broader Than Passthrough Lifetime

A transport may carry many independent zone-to-zone relationships.

A passthrough only represents one joined relationship between two transports.

So:

- a passthrough may die when no references in either direction depend on that join
- the transport must remain alive while any other zone-to-zone relationship still uses it
- transport counters must therefore reflect aggregate usage, not just one passthrough instance

## Transport-State Consequence

The transport layer must be able to represent:

- a newly acquired parent-to-destination relationship
- an older child-to-child relationship pending release

as separate state facts.

If they collapse into one bucket prematurely, the acquire-before-release rule cannot be enforced.

## Practical Decision Rule

When a code path performs:

- add-ref to establish a new holder
- release to drop an old holder

the implementation should ask:

- "are these the same ownership fact?"

If the answer is no, they must not share one undifferentiated lifetime entry.

## Examples

### Example 1: Parent Replaces Child Holder

```text
old:
  child -> destination

new:
  parent -> destination
```

Required behavior:

1. record `parent -> destination`
2. confirm it exists
3. release `child -> destination`

Forbidden behavior:

- treat the release of `child -> destination` as eligible to remove `parent -> destination`

### Example 2: Routed Out Parameter Handoff

```text
caller <- intermediate <- destination
```

Required behavior:

1. intermediate helps establish destination-facing and caller-facing route state
2. caller-owned relationship becomes real
3. any superseded intermediate relationship is released afterwards

Forbidden behavior:

- letting teardown of intermediate routing destroy the already-established caller-owned relationship

## Design Preference

The preferred design is to keep ownership logic simple and local:

- acquisition means acquisition
- release means release
- handoff means acquire, then release

This is preferred over designs that require release to recover the original meaning of add-ref from incomplete metadata.
