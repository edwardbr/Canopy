# Canopy Rust Port Plan

## Goal

Create a parallel Rust implementation under `rust/` that can interoperate with the existing C++ implementation through the current IDL and RPC protocol.

Initial constraints:

- Protocol Buffers only. No YAS support in Rust.
- The IDL generator remains in C++.
- The generator must emit Rust code as well as C++ code.
- Generated Rust and generated C++ must agree on:
  - protobuf schema and wire format
  - interface ordinals, method ordinals, and type fingerprints
  - ownership and reference-count protocol semantics used over RPC

The first implementation milestone after planning is `rust/rpc`, which is the Rust equivalent of `c++/rpc`.

## Structural Constraints

The Rust port should stay as close as practical to the C++ layout and function structure unless there is a severe technical reason not to.

That means:

- mirror the directory shape of `c++/` under `rust/` where feasible
- keep module boundaries close to the current C++ library boundaries
- preserve the conceptual role of key classes/interfaces as Rust traits/structs/modules
- preserve function naming and decomposition where that improves cross-referencing during the port

The goal is that a developer familiar with `c++/rpc` should be able to navigate `rust/rpc` without learning a completely different architecture.

## What Must Stay Stable

The Rust port should preserve the language-independent protocol contracts already defined by the repository:

- Shared IDL source in `interfaces/`
- Fingerprints generated from the parsed IDL/C++ AST in `generator/src/fingerprint_generator.cpp`
- Protobuf schema generation in `generator/src/protobuf_generator.cpp`
- RPC control/data model in `interfaces/rpc/rpc_types.idl`
- Runtime marshalling concepts in `c++/rpc/include/rpc/internal/`
- The marshalling interface contract in `c++/rpc/include/rpc/internal/marshaller.h`
- Error code behaviour in `c++/rpc/src/error_codes.cpp`
- Addressing and object identity semantics in `interfaces/rpc/rpc_types.idl`

This means the Rust implementation should not invent a new wire protocol. It should implement the existing one.

## Core Compatibility Rules

### 1. Fingerprints are generator-owned

The current fingerprint algorithm is defined in C++ in the generator, not in the runtime. That is the correct source of truth.

Plan:

- Keep fingerprint computation in the C++ generator.
- Generate Rust constants for every interface/struct/template instantiation fingerprint needed at runtime.
- Do not reimplement fingerprint generation in Rust during the first phase.

This avoids semantic drift caused by trying to reproduce C++-flavoured IDL interpretation in two languages.

### 2. Protobuf schema is canonical

Rust/C++ compatibility depends on both sides using the same generated `.proto` schema and compatible code generation options.

Plan:

- Continue generating `.proto` files from the C++ generator.
- Treat generated `.proto` as the canonical cross-language schema artifact.
- Add Rust code generation from those `.proto` files using a Rust protobuf toolchain.
- Verify that the generated message names, packages, field numbers, enum values, bytes handling, and nested type layout match the current C++ expectations.

### 3. IDL intent is interpreted once

The IDL syntax is intentionally C++-shaped. The parsed AST already captures the required semantics.

Plan:

- Extend the existing C++ generator with a Rust backend.
- Map parsed entities into Rust output rather than parsing IDL again in Rust.
- Generate Rust bindings from the same AST used for C++ headers/stubs/protobuf output.

### 4. Runtime behaviour is wire-compatible, not source-compatible

Rust does not need to copy the C++ class hierarchy literally. It does need to preserve observable protocol behaviour.

The Rust runtime must match:

- request/reply framing
- encoding selection, restricted to `protocol_buffers`
- add-ref / release / try-cast control flow
- service / service_proxy / stub / proxy semantics
- zone addressing and bit packing
- marshaller interface semantics
- error code values and meanings
- transport status transitions and shutdown semantics where visible on the wire
- dynamic-library transport semantics for in-process plugin loading

Of these, `interfaces/rpc/rpc_types.idl` is the primary object-identity contract and `c++/rpc/include/rpc/internal/marshaller.h` is the primary runtime plumbing contract.

### 5. Dynamic-library interop needs a language-neutral ABI

There is already a non-coroutine C++ `dynamic_library` transport for in-process child zones. It is the right transport concept for direct C++ <-> Rust calls in one process.

However, the current ABI in `c++/transports/dynamic_library/include/transports/dynamic_library/dll_abi.h` is C++-specific:

- it passes `rpc::*` C++ structs directly across the boundary
- it assumes both sides are compiled against the same C++ runtime types
- it relies on moving C++ objects through the function-pointer boundary

That is acceptable for C++ <-> C++ dynamic libraries, but it is not a stable Rust FFI.

Plan:

- keep the existing transport semantics and call flow
- introduce a language-neutral C ABI for the dynamic-library transport
- represent boundary payloads using plain C ABI types:
  - opaque context handles
  - integer enums and error codes
  - explicit pointer+length buffers
  - POD request/response structs
- make both implementations adapt between their native runtime types and this neutral ABI
- keep the shared ABI specification under `c_abi/`, with language-specific implementations under their own trees

This must support both directions:

- C++ host loading a Rust shared-library child zone
- Rust host loading a C++ shared-library child zone

## Proposed Rust Layout

The initial Rust tree should become a Cargo workspace.

Planned structure:

- `rust/Cargo.toml`
  - Cargo workspace root
- `rust/rpc/`
  - core runtime equivalent of `c++/rpc`
- `rust/generated/`
  - checked-in or build-generated Rust from IDL and protobuf
- `rust/transports/`
  - Rust transport implementations matching the C++ transport concepts
- `rust/tests/`
  - interop and conformance tests
- `rust/tools/`
  - helper scripts for codegen verification

Additional target:

- `rust/transports/dynamic_library/`
  - Rust equivalent of the non-coroutine in-process plugin transport

Shared ABI location:

- `c_abi/`
  - language-neutral ABI specifications and headers

Directory mirroring target:

- `c++/rpc` -> `rust/rpc`
- `c++/transports` -> `rust/transports`
- `c++/tests` -> `rust/tests`
- generated outputs should live under a build/generated path, not as committed source

Suggested crate split for the first phase:

- `canopy-rpc-types`
  - generated Rust types/constants from `interfaces/rpc/rpc_types.idl`
- `canopy-rpc-generated`
  - generated Rust proxies/stubs/fingerprint constants for all IDL
- `canopy-rpc`
  - handwritten runtime
- `canopy-rpc-interop-tests`
  - C++/Rust compatibility tests

If that split proves too heavy early on, start with a single `rust/rpc` crate and split later.

## Execution Plan

### Phase 0: Freeze the contracts

Before porting behaviour, document and verify the parts that cannot drift:

- fingerprint generation inputs and outputs
- protobuf package naming and file layout
- enum numeric values
- method ordinal generation
- address bit packing in `zone_address`
- error code meanings
- reference-count back-channel payloads
- dynamic-library transport call flow and lifecycle semantics

Deliverables:

- compatibility notes per IDL feature
- golden generated artifacts for a small representative IDL set

### Phase 1: Generator support for Rust

Extend the C++ generator to emit Rust-targeted outputs.

Required outputs:

- Rust constants for fingerprints and method/interface ordinals
- Rust native representations for generated structs/enums/interfaces
- Rust proxy and stub glue matching the current RPC control model
- build metadata describing which `.proto` files belong to which generated Rust module

Generator extension requirement:

- Introduce a Rust analogue to the existing `#cpp_quote(R^__( ... )__^)` escape hatches, for example `#rust_quote(R^__( ... )__^)`, so the shared IDL can carry Rust-specific generated fragments where needed.

Important rule:

- Rust generation should consume the same AST and helper logic as C++ generation where possible.
- Shared naming, sanitisation, and type-mapping logic should be centralized in the generator to reduce drift.
- Machine-generated Rust code should not be committed to the repository.
- Generated Rust should be written into build output directories or other ignored generated paths.

### Phase 2: Protobuf-first type layer

Build the smallest Rust surface that can round-trip shared types.

Scope:

- generated Rust protobuf modules for `interfaces/rpc/rpc_types.idl`
- handwritten adapters where protobuf messages and ergonomic Rust types differ
- exact handling for:
  - `std::vector<uint8_t>` / `std::vector<char>` as protobuf `bytes`
  - enums by explicit numeric value
  - nested structs
  - maps and sequences
  - 128-bit integer compatibility if exposed through protobuf helper messages

Acceptance criteria:

- C++ serialise -> Rust deserialise passes
- Rust serialise -> C++ deserialise passes
- fingerprints in generated Rust match C++ exactly

### Phase 2.5: Neutral dynamic-library ABI

Before full runtime parity, define the cross-language ABI for the non-coroutine dynamic-library transport.

Required outcome:

- a shared ABI document/header pair for C/C++ and Rust FFI
- entry points equivalent to the current `canopy_dll_*` surface
- no direct cross-language passing of C++ runtime structs

Repository location rule:

- shared ABI specifications belong in `c_abi/`
- implementation adapters stay in `c++/` and `rust/`

Recommended ABI shape:

- `void*` or opaque handles for runtime context
- `uint64_t` / `uint32_t` / `int32_t` for IDs and status
- explicit buffer views for serialized protobuf and back-channel payload data
- POD structs for send/post/try_cast/add_ref/release/object_released/transport_down/get_new_zone_id

Important rule:

- preserve the current host/child transport semantics from `c++/transports/dynamic_library/`
- change only the ABI representation, not the transport behaviour

### Phase 3: `rust/rpc` runtime port

Port `c++/rpc` into `rust/rpc` in behaviour-oriented layers.

Recommended order:

1. `rpc_types`-dependent primitives
   - encoding
   - error handling
   - object / method / interface ordinal wrappers
   - zone / destination zone / remote object helpers
2. address utilities
   - port `address_utils.cpp`
   - port `zone_address` bit packing semantics exactly
3. marshalling data structures
   - request/response params
   - back-channel entries
   - preserve `marshaller.h` structure as closely as Rust allows
4. proxy/stub core
   - object proxy
   - object stub
   - casting interface equivalent
5. zone/service layer
   - service
   - service proxy
   - lifecycle routing
6. transport base abstraction
   - status machine
   - passthrough logic
   - destination/reference counting
7. logging/telemetry adapters
   - preserve externally visible behaviour where needed
8. dynamic-library transport
   - port non-coroutine `dynamic_library`
   - implement the neutral ABI adapter on the Rust side
   - preserve current host/child zone lifecycle semantics

### Phase 4: Generated Rust proxies/stubs against the Rust runtime

Once the runtime exists, generated Rust code should target it directly.

This phase validates:

- generated trait/interface surface
- outbound calls from Rust to C++
- inbound calls from C++ to Rust
- try-cast and reference-count flows

### Phase 5: Transport parity

Port transports incrementally after `rust/rpc` is stable.

Recommended order:

- direct/local test transport equivalents first
- then dynamic-library interop transport
- then network transports
- streaming/coroutine equivalents later, after the synchronous path is proven

## `rust/rpc` First-Milestone Scope

The first implementation step should not aim for total feature parity. It should aim for a minimal interoperable runtime core.

`rust/rpc` v1 should support:

- protocol buffers only
- synchronous request/reply path first
- core rpc types from `interfaces/rpc/rpc_types.idl`
- object proxy and object stub lifetime protocol
- service/service_proxy routing
- base transport abstraction sufficient for interop tests

Runtime style requirement:

- Target non-coroutine functionality first.
- Keep the design open for a later coroutine/async port, likely via Tokio or a thin runtime abstraction over Tokio-compatible futures.
- Avoid early design choices that make later async integration unnatural.

`rust/rpc` v1 should explicitly defer:

- YAS
- SGX-specific behaviour
- streaming stack
- advanced telemetry parity
- optional convenience APIs not required for wire compatibility

## High-Risk Areas

### Fingerprint drift

Risk:

- Reimplementing fingerprint logic in Rust would likely diverge because the IDL semantics are C++-flavoured.

Response:

- Keep fingerprint computation in the generator and emit constants into Rust.

### Protobuf drift

Risk:

- Package names, message names, field numbering, bytes handling, nested type mapping, or template-instantiation naming can diverge.

Response:

- Reuse the existing protobuf generator as the canonical schema source.
- Add interop golden tests that compare encoded bytes for representative messages where determinism is expected.

### Ownership/lifetime mismatch

Risk:

- Rust ownership is not the same as C++ shared/optimistic pointer behaviour.

Response:

- Model the wire protocol explicitly.
- Keep local Rust ownership idiomatic, but preserve remote reference-count protocol exactly.
- Treat remote reference semantics as protocol state, not as a direct mirror of Rust references.

### Coroutine model mismatch

Risk:

- The C++ code supports synchronous and coroutine builds, while Rust will naturally prefer async.

Response:

- Start with a synchronous or single-executor-compatible runtime surface.
- Add async-friendly abstractions only after the base protocol is stable.
- Avoid baking executor-specific assumptions into generated code.

## Verification Strategy

Every phase should prove Rust/C++ interoperability, not only Rust self-consistency.

Required test layers:

- fingerprint constant tests against generated C++ values
- protobuf round-trip tests between C++ and Rust
- service call interop tests:
  - Rust client -> C++ server
  - C++ client -> Rust server
- try-cast/add-ref/release protocol tests
- zone address packing tests using shared golden vectors
- in-process dynamic-library interop tests:
  - C++ host -> Rust child DSO
  - Rust host -> C++ child DSO

Golden fixtures should be derived from generated code and representative IDL examples, not handwritten guesses.

Generated-code policy:

- No machine-generated Rust should be committed.
- Tests and build scripts should regenerate required Rust outputs as part of the local build/test flow.

## Recommended Immediate Next Steps

1. Create the Cargo workspace under `rust/`.
2. Add a small Rust codegen target to the C++ generator for constants and scaffolding only.
3. Generate Rust bindings for `interfaces/rpc/rpc_types.idl`.
4. Implement `rust/rpc` primitives and `zone_address` compatibility first.
5. Add C++/Rust protobuf interop tests before porting the full service layer.
6. Port `c++/rpc` to `rust/rpc` in the order described above.

## Definition Of Success For The First Major Milestone

The first milestone is complete when:

- `rust/rpc` exists as a buildable crate
- the generator can emit Rust constants and Rust RPC scaffolding from existing IDL
- Rust and C++ agree on generated fingerprints
- Rust and C++ exchange protobuf payloads for shared RPC types successfully
- a minimal Rust service can call, and be called by, a C++ service using the existing IDL
