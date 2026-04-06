# Canopy C ABI

This directory contains language-neutral C ABI specifications shared across
language implementations.

The intended audience is broader than the current C++ and Rust work. These
documents and headers should help future implementations in other languages as
well, including languages with garbage collection, manual memory management,
different async models, or thin C FFI layers.

Rules:

- C ABI specifications live here, not inside language-specific trees.
- C++ implementation lives under `c++/`.
- Rust implementation lives under `rust/`.
- Shared ABI contracts should be written so either side can implement them
  without depending on the other language's runtime types.
- ABI documentation should explain structure, intent, ownership, and expected
  behaviour rather than only listing C structs and function names.
- Shared headers should carry inline field- and function-level precondition
  comments wherever pointer validity, borrowing, or ownership is subtle.
- Ownership and lifetime rules must be explicit for handles, buffers, and
  callback tables.
- Fixed-width integer types should be used for all protocol-relevant numeric
  values.
- Async or coroutine behaviour must not leak into a base blocking ABI.

Initial focus:

- non-coroutine dynamic-library interop ABI for in-process C++ <-> Rust calls

Longer-term candidates:

- shared ABI enums/constants reusable across transports
- optional async ABI surfaces as separate transport-specific layers
- common plugin/host loading conventions

Planned contents:

- ABI notes/specification documents
- shared C headers for FFI boundaries
- transport-specific ABI definitions

## Documentation Expectations

Each ABI subtree should answer these questions clearly:

1. What problem does this ABI solve?
2. Which side owns each handle and each buffer?
3. Which inputs are borrowed and which results transfer ownership?
4. Which numeric values must match Canopy semantics exactly?
5. Which parts are transport-specific and which parts are reusable?
6. Which behaviour is intentionally out of scope?
7. How should a new language implementation approach the ABI safely?

In addition:

- Treat the inline comments in shared headers as part of the ABI contract.
- If a header comment says a caller must keep a pointer or borrowed buffer
  valid for the duration of a call, language implementations and code-generating
  agents should treat that as a required precondition, not advisory prose.
- Subtree READMEs should point implementers at the relevant shared header so
  they do not miss those local safety comments.

## Versioning Guidance

These ABI surfaces are still drafts. Until they stabilize:

- draft headers should say they are drafts
- breaking changes should be documented in the relevant subtree README
- language implementations should treat early drafts as unstable

## Portability Guidance

To keep the ABI broadly implementable:

- avoid compiler-specific language features in shared headers
- prefer POD structs, callback tables, and opaque handles
- avoid assumptions about exceptions, destructors, coroutines, or thread-local
  behaviour at the ABI boundary
- document alignment and packing expectations whenever raw binary layouts matter

## Relationship To The Rest Of The Repository

- `interfaces/` remains the source of truth for protocol concepts and object
  identity semantics
- `generator/` remains the source of truth for fingerprints and generated schema
- `c_abi/` is the source of truth only for cross-language ABI boundaries
- language-specific runtime behaviour belongs under each language tree
