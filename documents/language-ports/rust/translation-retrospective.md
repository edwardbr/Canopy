# Rust Translation Retrospective

This document records what was missing from the original Rust translation
instructions, what misunderstandings occurred during the migration, and which
Rust conventions pushed the implementation in the wrong direction.

It should be read before writing a new language-translation guide.

It is not a replacement for the original plan in
[`original-translation-instructions.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/original-translation-instructions.md).
That original plan remains useful because it shows what was known at the start.

## Why This Document Exists

The original plan correctly covered:

- wire compatibility
- generator ownership of fingerprints
- protobuf schema ownership
- the need for a neutral dynamic-library ABI
- the need to stay structurally close to the C++ tree

What it did not capture well enough was the amount of semantic information that
was not merely "C++ implementation detail", but part of the intended design.

Those missing semantics caused avoidable drift in:

- naming
- trait layering
- pointer design
- service/proxy/stub boundaries
- public API shape
- test-harness expectations

## Main Gaps In The Original Instructions

### 1. The public API boundary was under-specified

The original plan said the Rust port should be wire-compatible and structurally
close to C++, but it did not state clearly enough what application developers
should and should not see.

This mattered because the implementation drifted toward exposing runtime
concepts that should have remained hidden.

The missing rule was:

- application-facing code should see interfaces and interface pointers
- application-facing code should not need to reason about local vs remote
  representation
- application-facing code should not see `ObjectStub`, `ObjectProxy`,
  `ServiceProxy`, or `LocalProxy`

The correct public surface turned out to be:

- generated interface traits
- `SharedPtr<dyn Interface>`
- `OptimisticPtr<dyn Interface>`
- `RootService`
- `ChildService`

### 2. Service-role semantics were under-specified

The original plan did not state some critical service semantics explicitly
enough:

- there must be only one root service in the fuzz application
- child Rust zones must be `ChildService`s, not ad hoc roots
- child zone-id allocation must flow back to the real root service
- `connect_to_zone`, `attach_remote_zone`, and `create_child_zone` have
  distinct meanings and should not be conflated

This omission caused early Rust APIs and bootstrap helpers to use root-specific
terminology and behavior in places where child semantics were required.

### 3. Pointer semantics were under-specified

The original plan called out ownership/lifetime mismatch as a risk, but it did
not specify the intended pointer abstraction strongly enough.

What needed to be explicit from the start:

- `shared_ptr` and `optimistic_ptr` are the public abstraction
- local-vs-remote is an internal implementation detail
- `LocalProxy` is not public API; it is hidden inside optimistic-pointer
  behavior
- local optimistic failure must behave like remote optimistic failure:
  return `OBJECT_GONE`, not local weak-pointer mechanics
- remote shared and optimistic pointers to the same object should share one
  hidden remote control object with separate counts

Without these rules stated explicitly, it was too easy to overfit the Rust
implementation to ordinary Rust `Arc`/`Weak` thinking rather than the actual
Canopy pointer model.

### 4. Entity boundaries were under-specified

The original plan said "stay structurally close to C++", but it did not say
clearly enough that equivalent entities should also have roughly equivalent
responsibilities.

The missing guidance was:

- `ServiceProxy` should be a service-to-transport channel, not interface-aware
  glue
- `Service` should orchestrate, not absorb object-stub or pointer policy
- `ObjectProxy` should own remote object identity and remote pointer state
- `ObjectStub` should own local dispatch handoff
- `outbound_send` means outbound remote send, not generic service dispatch

Without that sharper rule, the Rust runtime accumulated extra glue in `service`
and `service_proxy` that the C++ design had already separated correctly.

### 5. Locking and re-entrancy rules were under-specified

The original plan referred to marshaller semantics, but did not translate that
into an explicit operational rule for the Rust runtime.

The missing rule was:

- no runtime lock may remain held across marshaller-style I/O or re-entrant
  dispatch functions

That includes:

- `send`
- `post`
- `try_cast`
- `add_ref`
- `release`
- `object_released`
- `transport_down`
- `get_new_zone_id`

This rule was already embedded in the C++ design intent, especially for
coroutine safety, but it was not stated directly enough for the Rust runtime.

### 6. Verification expectations were under-specified

The original plan described good verification goals, but not the concrete mixed
C++/Rust validation pressure points that matter in practice.

What needed to be explicit:

- debug validation is not enough
- mixed C++/Rust tests can fail in coroutine builds even if Rust itself remains
  blocking-only
- fuzz parity needs to compare the same instruction source across both
  implementations
- release-mode and coroutine-harness behavior matter early because they expose
  data races and teardown mistakes that debug builds may hide

## Main Misunderstandings During The Migration

### 1. Over-valuing Rust-native abstractions over Canopy semantics

A recurring mistake was to ask "what is the most idiomatic Rust design?" before
asking "what is the intended Canopy design?"

That led to:

- trait splits that were technically valid in Rust but conceptually wrong
- extra helper layers that did not exist in the C++ design
- pointer APIs that reflected Rust weak-pointer habits rather than Canopy's
  locality-hiding pointer model

The correct priority is:

1. preserve Canopy semantics and boundaries
2. express them cleanly in Rust
3. only then add Rust-specific simplifications if they do not obscure the
   design

### 2. Treating some C++ design choices as accidental implementation detail

Several things that first looked like C++-specific detail were actually design
requirements:

- `service_proxy` being interface-agnostic
- `outbound_send` being specifically the remote/proxy/transport path
- `local_proxy` being hidden inside optimistic-pointer behavior
- one root allocator in the fuzz application
- interface identity living on the interface, not the implementation

These should have been treated as first-class requirements, not as optional
structure that Rust could "simplify away".

### 3. Underestimating how much naming carries semantics

Several wrong names led directly to wrong designs or wrong expectations:

- root-specific exported-object helpers
- `zone_from_ffi(...)`
- `interface_view`
- `ProxySkeleton`
- `GeneratedRustInterface`
- excessive `Rpc` / `Generated` prefixes inside `canopy_rpc::internal`

The C++ naming was not perfect either, but the migration showed that naming is
part of the architecture. A wrong name quickly becomes a wrong mental model.

### 4. Assuming local and remote optimistic behavior must look different in Rust

A Rust-first instinct suggested APIs such as:

- `upgrade()`
- `Option`
- explicit local liveness checks

That was wrong for Canopy.

The correct model is:

- `OptimisticPtr<T>` should still behave as an interface pointer
- local liveness failure is normalized to `OBJECT_GONE`
- the caller should not branch on local weak-pointer state

This was a key semantic point that the original plan did not state explicitly
enough.

### 5. Assuming Rust trait-object limitations required extra public seams

Rust trait-object/object-safety concerns pushed the implementation toward
intermediate traits such as:

- `InterfaceBinding`
- `CreateLocalProxy`
- `CreateRemoteProxy`

Some hidden helper traits are reasonable, but the migration showed that these
were too eagerly elevated into important architectural concepts.

The better model was closer to the C++ design:

- interface-associated static functions where appropriate
- hidden runtime machinery staying hidden
- fewer semantically empty binding traits

## Rust Conventions That Sent The Work In The Wrong Direction

### 1. Idiomatic Rust tends to prefer smaller trait seams

That instinct is often useful, but here it encouraged decomposition that made
the runtime harder to compare to C++.

In this codebase, splitting traits or helpers should not be the default move if
it makes the equivalent C++ entity boundaries less obvious.

### 2. Rust's `Arc` / `Weak` mental model is not the Canopy pointer model

Rust makes it natural to think:

- strong pointer -> direct object access
- weak pointer -> explicit upgrade or `Option`

But Canopy's optimistic pointer semantics are not ordinary Rust weak-pointer
semantics. The pointer layer is part of the RPC abstraction, not just memory
management.

Following standard Rust intuition too closely led to the wrong public API
direction for `OptimisticPtr`.

### 3. Rust naming conventions can obscure architectural intent if applied too mechanically

Rust naming conventions are still correct:

- `snake_case` for modules and functions
- `UpperCamelCase` for traits and types

But applying Rust conventions mechanically is not enough. Names also need to
preserve Canopy's conceptual boundaries.

For example:

- changing `__Generated` to `__generated` was correct
- but "Rust-friendly" names that obscure the C++ role of an entity are not
  necessarily improvements

### 4. Hiding complexity in generic helpers can make the architecture worse

Rust makes it easy to introduce generic helpers and conversion traits.
That can reduce local boilerplate while making global architecture harder to
understand.

This happened in several places where:

- pointer binding
- service runtime access
- remote interface creation

were initially expressed through helper layers that made the runtime less
directly comparable to the C++ implementation.

## What A Better Translation Guide Must State Explicitly

Before writing another language-translation guide, it should state clearly:

### Public API rules

- what application developers should see
- what must remain hidden internals
- how local-vs-remote abstraction is meant to disappear from the public API

### Service and transport semantics

- which service is the root allocator
- the exact meanings of zone connection/attachment/child-creation operations
- the intended roles of `service`, `service_proxy`, `object_proxy`,
  `object_stub`, `transport`, and `pass_through`

### Pointer semantics

- strong vs optimistic semantics
- local optimistic failure behavior
- control-block ownership expectations
- whether an existing entity such as `object_proxy` is intended to serve as the
  remote control object

### Dispatch semantics

- what `outbound_send` means
- what local dispatch path should look like
- which operations are allowed to be interface-aware and which must remain
  interface-agnostic

### Locking and coroutine rules

- no locks across marshaller/I/O boundaries
- coroutine-specific nuances
- teardown and scheduler-drain expectations for coroutine test harnesses

### Naming guidance

- preserve C++ conceptual names where they carry architecture
- follow target-language casing conventions
- avoid target-language "cleanup" that erases real semantics

### Verification guidance

- minimum debug checks
- minimum release/coroutine checks
- required mixed-language parity tests
- expected scope limitations during early bring-up

## Recommended Principle For Future Language Ports

For a future language port, the default rule should be:

- preserve Canopy semantics first
- preserve Canopy entity boundaries second
- preserve target-language style third

The target language should make the implementation maintainable, but it should
not be allowed to erase the parts of the design that were already solved in the
C++ implementation.
