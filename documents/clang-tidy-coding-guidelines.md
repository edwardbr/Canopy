<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Clang-Tidy Coding Guidelines

This document captures the coding style and cleanup approach used while enabling
the broader `bugprone-*`, `cppcoreguidelines-*`, and `performance-*`
clang-tidy checks for Canopy.

## Intent

Treat clang-tidy as a design review tool, not as a reason to mechanically
reshape APIs. Prefer real fixes when the finding points at ownership, lifetime,
initialization, or performance behavior. Use suppressions only when the current
shape is intentional and safer than the tidy-suggested rewrite.

The tidy presets use `WarningsAsErrors: '*'`, so every enabled diagnostic must
either be fixed, narrowly suppressed, or deliberately excluded at the right
build boundary.

## Check Selection

The main check families are:

- `bugprone-*`
- `cppcoreguidelines-*`
- `performance-*`

Some checks are intentionally disabled because they conflict with existing
Canopy idioms, generated interfaces, ABI boundaries, or review signal quality.
Do not re-enable a disabled check without first testing both ordinary and SGX /
coroutine tidy presets.

Use current LLVM/clang-tidy tooling that supports the configured checks in
`.clang-tidy`. If a developer machine reports an unknown check, update the local
toolchain or discuss the check selection before weakening the project config.

## Generated Code

Canopy-generated wrappers are project code and should remain visible to
clang-tidy. This includes generated proxy, stub, YAS, protobuf-wrapper, and
nanopb-wrapper C++ files.

Raw third-party generator output is not project code. Keep raw `protoc` and
nanopb generated sources excluded with per-file `SKIP_LINTING TRUE`, and keep
raw generated nanopb headers excluded through `.clang-tidy` header filters.

Prefer fixing generator templates over suppressing generated output. If a
generated proxy or stub emits a warning, change the generator so all generated
interfaces receive the fix consistently.

Do not use target-level `CXX_CLANG_TIDY ""` on generated IDL targets or the
hand-written `generator` executable just to avoid raw serializer warnings. That
disables useful checks on Canopy-owned code. Use per-source lint exclusion for
raw serializer files instead.

## Third-Party And SDK Headers

Do not tidy external SDKs or vendored dependencies. Exclude them at the CMake
target boundary where possible and in `.clang-tidy` header filters where headers
are staged into the build tree.

Current examples include:

- `submodules/`
- `_deps/`
- staged SGX SDK paths such as `sgx-sdk/` and `sgxsdk/`
- staged SGXSSL paths such as `sgxssl/`
- raw nanopb generated headers under `generated/src/.../nanopb/...`

Avoid globally disabling a check because of third-party headers. Narrow the
exclusion to the external path or raw generated source that caused the noise.

## Coroutine References

Be careful with `cppcoreguidelines-avoid-reference-coroutine-parameters`.
References can be correct in Canopy when the caller-owned object is guaranteed
to outlive the coroutine frame.

Common safe cases are:

- immediately awaited calls joined by `sync_wait`, `when_all`, or equivalent
  structured waiting;
- per-iteration state declared outside the coroutine call and joined before the
  state leaves scope;
- generated IDL input and output references passed from named locals in the
  awaiting coroutine frame;
- required output parameters such as results, stats, reason strings, or parsed
  payloads.

Do not replace a required reference with a raw pointer unless `nullptr` is a
valid, documented state and the implementation handles it. Do not wrap a
parameter in `std::shared_ptr` or `rpc::shared_ptr` just to silence tidy. Passing
a shared pointer by value is appropriate when the coroutine must extend the
object lifetime independently of the caller.

When a reference is intentionally kept, use a narrow `NOLINT` or `NOLINTBEGIN`
with a short lifetime explanation.

## Ownership And Moves

Use `std::move` only when ownership or expensive state is actually consumed.
`performance-move-const-arg` findings are usually real; remove no-op moves from
const objects, trivially copyable values, and APIs that take `const&`.

For `std::shared_ptr` upcasts where ownership transfer is intended, prefer the
converting move constructor:

```cpp
std::shared_ptr<base> value(std::move(derived));
```

Avoid `std::static_pointer_cast<T>(std::move(value))` when the active standard
library implementation accepts the source pointer by `const&`; the move is then
only decorative.

Use `rpc::shared_ptr` and `std::shared_ptr` according to their ownership
domains. Do not bridge them with raw pointer conversions or casts.

## Special Members

Use rule-of-zero for value types. For resource, transport, stream, and identity
objects, make copy and move behavior explicit.

If an object owns sockets, streams, transport state, registration identity,
threads, callbacks, or parent/child lifetime links, either implement the full
required special-member behavior or explicitly delete copy and move operations.
Do not rely on an implicit compiler decision to communicate ownership semantics.

## C APIs, ABI, And Low-Level Code

Preserve C ABI and foreign-library shapes. Raw pointers, macros, C arrays,
casts, and globals may be the correct contract at module boundaries, SGX hooks,
callback contexts, generated C serializers, and foreign library adapters.

When a low-level construct is required, suppress locally with the exact check
name and a reason tied to the ABI or storage requirement. Do not perform a
large semantic rewrite only to satisfy a stylistic check.

## Suppression Style

Keep suppressions narrow:

- prefer line-level `NOLINTNEXTLINE` for a single expression;
- use `NOLINTBEGIN` / `NOLINTEND` only for a coherent block;
- name the exact check;
- include a short reason unless the suppression is emitted into generated code
  where the generator already documents the pattern.

Avoid broad file-level suppressions except for raw generated output or external
code.

## CMake Integration

Enable clang-tidy once for project targets and explicitly opt out external
subdirectories. For Canopy-generated IDL targets, keep target-level tidy enabled
and use source-level exclusions for raw serializer output.

When adding a new generated source family, decide which side it belongs to:

- Canopy-owned generated C++ wrapper: tidy it.
- Third-party/raw generated serializer source: compile it, but skip linting it.
- External SDK or dependency header: exclude it through include classification
  or header filters.

Run `cmake-format` after changing CMake files.

## Verification

For clang-tidy changes, verify at least one ordinary tidy build and the most
relevant special build affected by the change. Useful presets include:

```bash
cmake --preset Debug_Coroutine_Tidy
cmake --build build_debug_coroutine_tidy

cmake --preset Debug_SGX_Sim_Tidy
cmake --build build_debug_sgx_sim_tidy
```

If the change touches shared coroutine/non-coroutine behavior, also consider the
matching non-tidy build or target-specific tests. Run `clang-format` and
`cmake-format` on touched files before handoff.

## Review Checklist

Before accepting a tidy cleanup, check:

- Did the change preserve API ownership and lifetime semantics?
- Did coroutine reference parameters remain references where lifetime is
  structured and known?
- Are required out parameters still references?
- Are raw pointers only used where null, C ABI, buffer, or foreign-library
  semantics require them?
- Are generated-code warnings fixed in the generator when possible?
- Are raw generated and external paths excluded narrowly rather than globally?
- Are `NOLINT` comments local, named, and justified?
- Were both coroutine and blocking paths considered?
- Were SGX / enclave paths considered when include filters changed?
