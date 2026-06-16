# Config Topology Neutral Curation Plan

## Purpose

`config-topology-schema` is the active development line and `main_rebase` is being prepared as the future mainline. The final merge from `config-topology-schema` into `main_rebase` should be a real merge, not a squash, so future work can track ancestry between the two branches.

This plan captures the changes from the SGX-stripping curation that are not inherently SGX removal. Those changes can be applied to `config-topology-schema` first as a small feature branch, leaving the later `main_rebase` merge to carry only the curated SGX removal and integration state.

## Current Branch Relationship

- `main` and the current `main_rebase` base are at `6ab2bc937930`.
- `config-topology-schema` is at `958d4764564b`.
- `config-topology-schema` is 164 commits ahead of `main`.

## Patch Scope

The neutral patch should include:

- Rename attestation identity terminology from `enclave_id` to `security_domain_id` where it describes the logical security relationship rather than an SGX runtime enclave handle.
- Keep SGX backend code present, but update SGX attestation backend references to the renamed attestation identity field.
- Rename local-process pointer test capability from enclave terminology to transport capability terminology:
  - `get_has_enclave()` becomes `supports_process_local_reference_tests()`.
  - the `standard_tests` boolean parameter becomes `supports_process_local_reference_tests`.
  - direct in-memory transports return `true`; remote/SGX-style transports return `false`.
- Preserve the fix that prevents remote transports from round-tripping process-local raw pointers and references.
- Add release-build fake attestation backend linkage where tests use the fake backend while development backends are disabled.
- Update affected test strings and golden vectors only when the input identity strings change from enclave wording to domain wording.

The neutral patch should exclude:

- Deleting SGX transports, SGX CMake modules, SGX attestation backends, enclave tests, or SGX submodules.
- Removing `_enclave` targets.
- Removing `CANOPY_BUILD_ENCLAVE`, `FOR_SGX`, SGX EDL, SGX host library paths, or SGX sidecar/peer connector wiring.
- Making the shared coroutine runtime API more specific to a particular coroutine backend.
- Deleting dot-tool directories or other branch hygiene unrelated to the shared code behavior.

## Proposed Flow

1. Create a feature branch from `config-topology-schema`.
2. Apply the neutral patch produced next to this plan.
3. Build and test at least `Debug`, `Debug_Coroutine`, `Release`, and `Release_Coroutine`.
4. If SGX presets are expected to remain viable on `config-topology-schema`, also build the available SGX presets on a machine with the required SGX dependencies.
5. Merge the neutral feature branch back into `config-topology-schema`.
6. Refresh or re-run the curated `main_rebase` merge from the updated `config-topology-schema`.
7. Commit the final `main_rebase` merge with an explicit ancestry-linking message that records:
   - source branch and head commit,
   - main/base commit,
   - SGX removal as intentional curation,
   - verification performed.

## Patch Artifact

Patch file:

- `documents/reviews/2026-06-15-config-topology-schema-neutral-curation.patch`

Generation and validation notes:

- Generated from a temporary export of `config-topology-schema`.
- Does not touch `c++/rpc/include/rpc/internal/coro_runtime`.
- Keeps SGX backend files and SGX test files present.
- Updates SGX attestation backend code only where it uses the shared attestation identity field.
- `git diff --check` passed in the temporary patch worktree.
- The patch is generated with zero context so this repository's whitespace checks do not treat patch context lines as trailing whitespace.
- `git apply --unidiff-zero --check documents/reviews/2026-06-15-config-topology-schema-neutral-curation.patch` passed against a fresh export of `config-topology-schema`.

## Merge Message Direction

The final merge message should not remain the default `Merge branch 'config-topology-schema' into main_rebase`. It should describe the merge as a curated ancestry link from the development branch to the future mainline, with SGX intentionally stripped pending the later SGX refactor.

Draft message:

```text
Curate config-topology-schema onto main_rebase without SGX

Merge config-topology-schema at 958d4764564b into main_rebase from the
main/base commit 6ab2bc937930. This is an ancestry-preserving merge so the
future mainline can continue to track the development branch instead of losing
the relationship in a squash or replay.

Bring forward the config topology, schema introspection, connection factory,
stream composition, protected RPC, route attestation, executor, and coroutine
integration work from config-topology-schema.

Intentionally strip SGX/enclave implementation material for this curated
mainline pass while keeping the non-SGX attestation backends. The SGX-specific
backends, transports, enclave tests, CMake modules, and SGX-only submodule are
left for a later dedicated SGX refactor.

Keep the curation fixes needed for the stripped branch:
- guard process-local pointer/reference tests by transport capability rather
  than enclave terminology
- fix release test linkage for fake attestation backends
- update the attestation KDF golden vector for the backend-neutral identity
  terminology
- leave an explicit source comment documenting that SGX attestation backends
  were stripped during the refactor

Verified with Debug, Release, Debug_Coroutine, and Release_Coroutine builds and
CTest runs, plus targeted io_uring, file-system, and test_fixtures checks.
```
