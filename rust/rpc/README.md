# `rust/rpc` Port Scope

This directory is the first implementation target for the Rust port.

## Source Mapping

The initial port should cover the behaviour currently implemented by:

- `c++/rpc/include/rpc/`
- `c++/rpc/include/rpc/internal/`
- `c++/rpc/src/`

The Rust port should preserve protocol behaviour, not C++ surface syntax.

Where there is no severe issue, keep:

- a similar directory structure
- a similar module/function breakdown
- a similar conceptual mapping for `service`, `service_proxy`, `transport`, `pass_through`, `object_proxy`, `object_stub`, and `i_marshaller`

The most important interface to mirror closely is [`marshaller.h`](/var/home/edward/projects/Canopy/c++/rpc/include/rpc/internal/marshaller.h).

## Porting Order

1. Core value types and wrappers from `rpc_types.idl`
2. Address helpers and `zone_address` bit packing
3. Marshalling parameter types and protobuf-only serialization hooks
4. Object proxy and object stub lifecycle logic
5. Service and service proxy routing
6. Transport base abstraction and passthrough accounting
7. Logging/telemetry shims only where required by tests

## Non-Goals For The First Pass

- YAS support
- Full transport parity
- SGX support
- Streaming stack parity
- Perfect API resemblance to C++

Note:

- non-coroutine dynamic-library interop with C++ is important and should be designed early, but it depends on a neutral FFI ABI rather than the current C++-only ABI representation.

## Required Invariants

- Generated fingerprints must come from the C++ generator and match C++ exactly.
- Rust must only advertise `protocol_buffers` support.
- Wire-visible enum values and control-flow semantics must remain compatible with C++.
- Interop tests must precede broad API polish.
- Error code numeric values and meanings must match C++.
- Addressing and object identity behaviour from [`rpc_types.idl`](/var/home/edward/projects/Canopy/interfaces/rpc/rpc_types.idl) must match C++.
- The Rust marshalling interface should stay as close as practical to [`marshaller.h`](/var/home/edward/projects/Canopy/c++/rpc/include/rpc/internal/marshaller.h).
- The Rust runtime must be able to participate in the non-coroutine dynamic-library transport in both directions:
  - C++ host <-> Rust child
  - Rust host <-> C++ child

## Dynamic-Library Interop

The current C++ dynamic-library transport is the right conceptual model, but its ABI is C++-specific and cannot be treated as stable Rust FFI because it passes `rpc::*` C++ structs directly across the boundary.

For Rust support:

- keep the existing host/child transport structure and call flow
- define a neutral FFI layer for the `canopy_dll_*` boundary
- adapt C++ runtime structs to that ABI on the C++ side
- adapt Rust runtime structs to that ABI on the Rust side

The intent is direct in-process calls through the transport, not a separate protocol.

## Generation Rules

- Shared IDL should eventually support Rust-specific escape hatches analogous to `#cpp_quote`, using a `#rust_quote` form.
- Generated Rust should be build output, not committed source.
- The first runtime target is non-coroutine behaviour; async/coroutine support is a later extension and should be designed with Tokio compatibility in mind.
