# Canopy Python Port Plan

## Purpose

This document describes how Canopy should be translated into Python.

It is written after the Rust migration work and incorporates the lessons from
that effort. It should be read together with:

- [`original-translation-instructions.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/original-translation-instructions.md)
- [`translation-retrospective.md`](/var/home/edward/projects/Canopy/documents/language-ports/rust/translation-retrospective.md)

The goal is not to make Python "feel native" at the cost of losing Canopy
semantics. The goal is to preserve Canopy's protocol, entity boundaries, and
public abstraction model while expressing them cleanly in Python.

## Goal

Create a Python implementation that can interoperate with the existing C++
implementation through the current IDL and RPC protocol.

Initial constraints:

- Protocol Buffers only
- the IDL generator remains in C++
- the generator must emit Python code as well as C++ code
- generated Python and generated C++ must agree on:
  - protobuf schema and wire format
  - interface ordinals, method ordinals, and fingerprints
  - ownership and reference-count protocol semantics used over RPC

The first milestone should be a Python runtime equivalent of the blocking
non-coroutine parts of `c++/rpc`.

## Non-Negotiable Rules

### 1. Preserve Canopy semantics first

The Python port must preserve:

- request/reply framing
- encoding selection, restricted initially to `protocol_buffers`
- add-ref / release / try-cast control flow
- service / service_proxy / object_proxy / object_stub semantics
- zone addressing and bit packing
- error code values and meanings
- dynamic-library transport semantics where they are visible over the wire

Do not redesign the protocol around Python conventions.

### 2. Preserve entity boundaries second

Equivalent Python entities should have approximately the same role as the C++
entities.

That means:

- `service` orchestrates
- `service_proxy` is a service-to-transport channel and object-proxy cache
- `object_proxy` owns remote object identity and remote pointer state
- `object_stub` owns local dispatch handoff
- `transport` owns transport routing/state
- `pass_through` owns intermediate-zone routing behavior

If a Python entity becomes much larger or more conceptually fuzzy than its C++
counterpart, treat that as a design smell.

### 3. Public API boundary must stay clean

Application-facing Python code should see:

- generated interface classes or protocols
- shared interface pointers
- optimistic interface pointers
- root and child services

Application-facing Python code should not need to know about:

- object stubs
- object proxies
- service proxies
- local optimistic forwarding proxies
- transport-local pointer/control details

The Python API should hide local-vs-remote mechanics in the same way the C++
and corrected Rust design intend to.

### 4. No lock may cross marshaller/I-O boundaries

Any Python lock or equivalent synchronization primitive must be released before
calling marshaller-style runtime functions such as:

- `send`
- `post`
- `try_cast`
- `add_ref`
- `release`
- `object_released`
- `transport_down`
- `get_new_zone_id`

This rule applies even in a blocking runtime.

If async support is added later, the rule becomes more important, not less.

## Python-Specific Design Constraints

### 1. Python should follow the blocking runtime first

The first Python runtime should be blocking.

Do not start with:

- `asyncio`
- background task frameworks
- streaming transports
- coroutine-first APIs

Canopy's blocking runtime semantics should be made correct before an async
surface is considered.

### 2. Python should not "simplify away" pointer semantics

Python does not have `shared_ptr` or `weak_ptr` in the C++ sense, but the
Canopy protocol does.

So the Python runtime must model:

- strong shared interface references
- optimistic interface references
- separate shared vs optimistic remote reference counts
- local optimistic failure returning `OBJECT_GONE`

Do not replace optimistic pointers with:

- `Optional`
- `None` checks
- explicit app-facing liveness probes

The pointer wrappers are part of the RPC abstraction, not merely memory
management.

### 3. Python code generation should own Python-facing interface construction

Generated Python interfaces should carry the type-level metadata and helper
construction needed by the runtime.

That includes:

- interface id lookup
- method metadata lookup
- remote proxy construction hooks
- generated local dispatch metadata

Do not force handwritten runtime code to recover interface semantics from
implementation classes if the generator already knows them.

### 4. Python naming should preserve architecture

Use Python naming conventions, but do not erase Canopy roles.

Reasonable Python conventions:

- `snake_case` for functions and modules
- `CapWords` for classes
- constants in `UPPER_CASE`

But preserve conceptual names where they matter:

- `Service`
- `ServiceProxy`
- `ObjectProxy`
- `ObjectStub`
- `RootService`
- `ChildService`
- `SharedPtr`
- `OptimisticPtr`

Do not rename architectural concepts merely to sound more Pythonic.

## What Must Stay Generator-Owned

The Python runtime must not reimplement:

- fingerprint generation
- IDL parsing
- method ordinal generation
- interface ordinal generation
- protobuf schema generation

The generator should emit Python-targeted outputs from the same AST used for
C++ generation.

The generator should also support a Python analogue of the existing quote
escapes, for example:

- `#python_quote(...)`

if language-specific generated fragments are needed.

## Public Python API Rules

The intended public surface should be:

- generated interface base classes or protocols
- `SharedPtr[Interface]`
- `OptimisticPtr[Interface]`
- `RootService`
- `ChildService`

The public API should satisfy:

- local and remote shared pointers are used the same way
- local and remote optimistic pointers are used the same way
- if an optimistic target is gone, the call returns `OBJECT_GONE`
- application code should not branch on local-vs-remote representation

If Python syntax or typing limitations force compromises, those compromises
should remain at the implementation level, not leak into the normal app-facing
API.

## Pointer Design Requirements

### Shared pointers

Shared interface pointers must be able to represent either:

- a local object in the current zone
- a remote object reached through the transport/runtime path

The caller should interact with them uniformly.

### Optimistic pointers

Optimistic interface pointers must also hide locality.

For a local target:

- the hidden local optimistic forwarding path should attempt the call
- if the target no longer exists, return `OBJECT_GONE`

For a remote target:

- use the normal remote optimistic control flow
- preserve optimistic add-ref/release semantics on the wire

### Remote control object

The Python design may allow `ObjectProxy` itself to serve as the hidden remote
control object if that keeps the implementation simpler and still preserves the
same semantics:

- remote object identity
- shared count
- optimistic count
- per-interface remote proxy cache

Do not introduce extra control-block layers unless they materially simplify the
implementation.

## Service And Zone Semantics

The translation guide must enforce the following meanings:

- `connect_to_zone` is for hierarchical and peer-to-peer zone instantiation
- `attach_remote_zone` is for accepting a request from another zone to connect
  to this zone in a peer-to-peer fashion
- `create_child_zone` is for child zones attaching to their parent

For applications with a single root allocator:

- only one root service should exist
- child services must obtain child zone IDs through the actual root path
- child-zone bootstrap helpers must not quietly create independent roots

These rules must be encoded in Python helper naming and behavior from the
start.

## Dynamic-Library And FFI Guidance

If Python participates in in-process child loading, it must do so through a
language-neutral ABI.

Do not attempt to share C++ runtime structs directly with Python.

The Python side should adapt between:

- plain C ABI data
- Python runtime objects

using:

- opaque handles
- integer status/error codes
- explicit buffer views
- POD request/response structures

The transport semantics must remain those of Canopy's dynamic-library transport,
not an invented Python plugin protocol.

## Proposed Python Layout

The initial Python tree should live under a dedicated home, for example:

- `python/`
  - Python workspace/package root
- `python/canopy_rpc/`
  - handwritten runtime
- `python/canopy_generated/`
  - ignored/generated Python from IDL and protobuf
- `python/transports/`
  - Python transport implementations
- `python/tests/`
  - Python/C++ interop and conformance tests

The exact packaging shape may use:

- a Python package layout
- a virtual environment
- a build backend such as `setuptools` or `hatchling`

Those choices are secondary. The important constraint is that the directory and
module roles should stay close to the C++ tree.

## Execution Plan

### Phase 0: Freeze the contracts

Before implementing runtime behavior, document and verify:

- fingerprint values
- protobuf package/module layout
- enum numeric values
- method ordinal generation
- address packing in `zone_address`
- error code meanings
- reference-count back-channel payloads
- dynamic-library transport call flow and lifecycle semantics

Deliverables:

- compatibility notes per representative IDL feature
- golden generated artifacts for a small representative IDL set

### Phase 1: Generator support for Python

Extend the existing C++ generator to emit Python-targeted outputs.

Required outputs:

- Python constants for fingerprints and ordinals
- Python representations for generated structs/enums/interfaces
- generated proxy and stub scaffolding
- build metadata describing generated protobuf ownership and module placement

Important rules:

- consume the same AST and helper logic as the C++ generator where practical
- centralize naming, sanitization, and type-mapping logic in the generator
- do not commit machine-generated Python

### Phase 2: Protobuf-first type layer

Implement the smallest Python surface that can round-trip shared types.

Scope:

- generated Python protobuf modules for `interfaces/rpc/rpc_types.idl`
- adapters where protobuf messages and ergonomic Python types differ
- exact handling for:
  - bytes fields
  - enums by explicit numeric value
  - nested structs
  - maps and sequences

Acceptance criteria:

- C++ serialize -> Python deserialize passes
- Python serialize -> C++ deserialize passes
- generated fingerprint constants match C++ exactly

### Phase 2.5: Neutral dynamic-library ABI

If Python will participate in dynamic-library transport scenarios, implement
the neutral ABI adapter before attempting full runtime parity.

Required outcome:

- shared ABI document/header usage
- Python-side adapter layer over the neutral ABI
- no cross-language passing of C++ runtime structs

### Phase 3: Blocking `rpc` runtime port

Port the blocking runtime in behavior-oriented layers.

Recommended order:

1. rpc-type primitives
2. address utilities
3. marshalling parameter/result structures
4. object proxy / object stub / casting-interface equivalent
5. service / service_proxy / lifecycle routing
6. transport base abstraction
7. pass-through logic
8. dynamic-library transport adapter

### Phase 4: Generated proxies/stubs against the Python runtime

Once the runtime exists, generated Python code should target it directly.

This phase validates:

- generated interface surface
- outbound calls from Python to C++
- inbound calls from C++ to Python
- try-cast and reference-count flows

### Phase 5: Transport parity

Port transports incrementally after the Python runtime is stable.

Recommended order:

- local/direct-style test transports first
- dynamic-library interop transport
- network transports later
- streaming/async transports only after the blocking path is proven

## Verification Strategy

Every phase must prove Python/C++ interoperability, not only Python
self-consistency.

Required test layers:

- fingerprint constant tests against generated C++ values
- protobuf round-trip tests between C++ and Python
- service call interop tests:
  - Python client -> C++ server
  - C++ client -> Python server
- try-cast/add-ref/release protocol tests
- zone-address packing tests using shared golden vectors
- in-process dynamic-library interop tests if Python participates there

Validation must include:

- debug-style checks
- optimized/release-style checks where possible
- mixed-language deterministic behavior checks when a fuzz or replay harness is
  available

## Common Failure Modes To Avoid

- treating target-language idioms as more important than Canopy semantics
- exposing local-vs-remote implementation details in the public API
- allowing `service_proxy` to become interface-aware glue
- collapsing local stub dispatch back into generic service outbound paths
- representing optimistic pointers as ordinary weak references
- keeping locks held across marshaller or transport boundaries
- inventing Python-only runtime layers when an existing C++ concept already
  solves the problem

## Definition Of Success For The First Major Python Milestone

The first milestone is complete when:

- a Python runtime exists as a buildable package
- the generator emits Python constants and scaffolding from existing IDL
- Python and C++ agree on generated fingerprints
- Python and C++ exchange protobuf payloads for shared RPC types successfully
- a minimal Python service can call, and be called by, a C++ service using the
  existing IDL
- the application-facing API already hides local-vs-remote mechanics behind the
  pointer and service abstractions
