# Dynamic-Library C ABI Draft

This directory contains the first language-neutral draft of the Canopy
non-coroutine dynamic-library ABI.

It should be readable by an implementer who has not studied the current C++
transport implementation in detail.

## Goal

Preserve the current parent/child-zone transport semantics of
`c++/transports/dynamic_library/` while replacing the existing C++-specific ABI
with a C ABI that can be implemented by both C++ and Rust.

This ABI is for direct in-process calls:

- C++ parent <-> Rust child shared library
- Rust parent <-> C++ child shared library
- future language parent <-> child combinations that can implement a C FFI

It is not an async/coroutine ABI. It is deliberately blocking and
executor-neutral.

## Design Rules

- Keep the existing `canopy_dll_*` entry point model.
- Keep opaque context handles on both sides.
- Do not pass C++ runtime structs across the language boundary.
- Do not pass coroutine/future/task types across the language boundary.
- Carry payload data as explicit pointer+length buffers.
- Carry routing/object identity as packed `zone_address` blobs.
- Keep ownership rules explicit for every buffer and handle.

## Mental Model

The transport connects two Canopy zones in one process:

- the parent side loads a shared library
- the child side lives inside that shared library
- both sides expose marshalling operations to each other through callbacks and
  exported `canopy_dll_*` entry points

The ABI does not define the Canopy runtime itself. It defines only the
cross-language seam between two runtime implementations.

## Relationship To The Existing C++ ABI

The current C++ ABI in
`c++/transports/dynamic_library/include/transports/dynamic_library/dll_abi.h`
assumes both sides share C++ runtime types such as:

- `rpc::send_params`
- `rpc::send_result`
- `rpc::remote_object`
- `rpc::zone`

That is suitable for C++ <-> C++ but not for Rust interop.

This draft keeps the same transport operations:

- init
- destroy
- send
- post
- try_cast
- add_ref
- release
- object_released
- transport_down
- get_new_zone_id

but changes the ABI representation only.

## Why The ABI Uses Packed Zone Blobs

The existing object and zone identity semantics are ultimately defined by the
packed `zone_address` representation in the Canopy protocol model.

Passing the packed blob at the ABI boundary is safer than passing a
language-shaped object model because:

- it avoids committing the ABI to one language's type layout
- it preserves the protocol-level identity representation
- it lets each implementation decode the blob into native types internally

This is the current ABI decision for the dynamic-library transport. It may be
revisited later if a more explicit shared address struct proves materially
clearer without weakening protocol compatibility, but new transport work should
assume packed blobs for now.

## ABI Shape

The shared header is:

- [`canopy_dynamic_library.h`](/var/home/edward/projects/Canopy/c_abi/dynamic_library/canopy_dynamic_library.h)

Implementers should read the inline comments in that header, not just the type
names. The header carries local preconditions for pointer validity, borrowed
buffer lifetime, callback usage, and result ownership. Those comments are part
of the ABI contract and should be preserved in future language bindings and
generated FFI wrappers.

The ABI uses:

- opaque `void*` handles for parent and child contexts
- fixed-width integers for IDs, enum values, and status
- explicit byte buffers for payloads
- POD request/result structs

## Required Behaviour

Any implementation of this ABI should preserve these behavioural contracts:

- `canopy_dll_init` creates the child-side context and returns the child root
  object descriptor
- `canopy_dll_destroy` is the terminal release for the child context
- `send` is request/reply and may return payload and back-channel data
- `post` is one-way
- `try_cast`, `add_ref`, and `release` preserve the same meaning as in the
  Canopy marshalling model
- `object_released` and `transport_down` are notifications
- `get_new_zone_id` requests a new zone from the parent side

## Ownership Rules

- Input buffers are borrowed for the duration of the call only.
- Output buffers are allocated by the callee using the allocator in the ABI
  table and must be released by the caller with the matching free callback.
- `parent_ctx` is owned by the parent implementation.
- `child_ctx` is owned by the child implementation and released by
  `canopy_dll_destroy`.

More explicitly:

- fields of type `canopy_const_byte_buffer` are borrowed inputs unless a
  function explicitly documents otherwise
- result buffers are owned by the callee until returned
- after return, the caller owns returned buffers and must release them using
  the allocator/free policy associated with the call
- implementations must not retain borrowed pointers after the call returns
  unless they first copy the data
- variable-sized outputs such as `output_obj`, `zone_id`, `out_buf`,
  back-channel entry arrays, and back-channel payload buffers are expected to
  be allocated using the caller-supplied allocator vtable
- if a call partially allocates a nested result and then fails, the callee must
  clean up any allocations it already produced before returning the error

For avoidance of doubt:

- if a field comment or function comment says the caller must keep referenced
  buffers valid for the duration of the call, that rule applies to manual
  implementations, generated bindings, and agent-authored glue code equally
- when in doubt, copy borrowed input data into language-native ownership before
  retaining it

## Allocator Model

The allocator vtable exists so languages with different allocation models can
exchange buffers safely.

The intended rule is:

- the side producing an output buffer uses the allocator supplied by the caller
- the side receiving the output buffer uses the paired `free` callback when the
  buffer is no longer needed
- nested result structures must follow the same rule recursively

For this transport specifically:

- `canopy_dll_init` may allocate storage for the returned `output_obj`
- `canopy_dll_send` may allocate `out_buf`, the back-channel entry array, and
  payload buffers for each returned back-channel entry
- `canopy_dll_try_cast`, `canopy_dll_add_ref`, and `canopy_dll_release` may
  allocate returned back-channel storage
- `canopy_dll_get_new_zone_id` may allocate storage for the returned `zone_id`
  and returned back-channel data

This avoids requiring compatible global allocators across languages.

## Error Model

- function return values are `int32_t` Canopy error codes
- result structs also carry an `error_code` field for consistency with the
  existing transport model
- implementations should keep those values aligned
- invalid ABI usage should still resolve to a Canopy-style error where practical

## Guidance For New Language Implementations

If you are implementing this ABI in a new language:

1. Treat every incoming pointer as borrowed unless ownership is explicitly
   transferred.
2. Convert ABI structs into native runtime structs at the boundary.
3. Keep native runtime objects behind opaque context handles.
4. Do not let language-native exceptions or panics cross the C boundary.
5. Convert native failures into Canopy error codes before returning.
6. Copy borrowed data if the native runtime needs to retain it after the call.
7. Keep the ABI layer small and dumb; most logic should live in the language's
   own runtime implementation.

## Non-Goals

- This ABI does not define coroutine or async interactions.
- This ABI does not standardize generator output.
- This ABI does not require a language implementation to share the C++ runtime.
- This ABI does not replace the Canopy protocol model defined elsewhere.

## Open Points

- The wire-independent C ABI enums may later be split into separate shared
  headers if multiple transports need them.
- An optional async ABI may be added later, but only as a separate ABI surface.
- Shared helper APIs for recursively freeing nested result buffers may still be
  useful so each language binding does not have to reimplement that logic.
