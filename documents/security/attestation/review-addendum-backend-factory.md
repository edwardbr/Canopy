# Review Addendum: Null Backend + Backend Factory + Unattested-Peer Policy

Scope: staged changes only. Reviewed against live code, not prose. Blunt by request.

## Summary verdict

The core policy change is correct and is a genuine security hardening: accepting
a no-Evidence peer now requires *two* affirmative policy flags
(`require_peer_evidence == false` AND `allow_unattested_peer == true`) instead of
one. The gating is applied consistently at all three production handshake sites.
The null backend and `produce_evidence` guard are sound.

The backend factory, however, is half-wired scaffolding with a security-hostile
default and several silent fail-open fallbacks. None of it is exercised by
production code yet, which limits blast radius today but means the risky parts
will land unreviewed when a call site adopts it.

---

## Security

### S1 (High) — Build default is the dev backend, not fail-closed

`cmake/Canopy.cmake:62` defaults `CANOPY_ATTESTATION_BACKEND` to `"FAKE"`.
`implementation-plan.md` previously stated NULL was the production default; the
doc was edited to "production presets *should* use NULL" but the build default
was simultaneously flipped to FAKE. Net effect: a build that forgets to set the
variable compiles in the **fake backend, which satisfies development
attestation policy**. The fake backend is explicitly "No" production trust in
`attestation-backends.md`. A forgotten flag should fail closed (NULL or hard
error), never silently to a backend that produces accepted Evidence.
Recommendation: default to `NULL`, or require the variable to be set explicitly
with no default in non-dev presets.

### S2 (Medium) — Enum switches fail open to FAKE

`backend_factory.cpp`:
- `attestation_backend_kind_name` falls through to `return fake_backend_id;` (line ~158)
- `make_attestation_backend` falls through to `return std::make_shared<fake_backend>();` (line ~184)

Both enum values are handled, so the trailing `return` is dead today. But the
moment a third backend kind is added and a switch is missed, the system
silently produces the **dev backend that passes development policy**. Same
anti-pattern as S1. Fallback should be `null_backend` or
`std::terminate`/`unreachable` — fail closed, not to fake.

### S3 (Low) — `parse_attestation_backend_kind` is case-fragile and unused

`backend_factory.cpp:140` only matches exact `"FAKE"/"fake"/"NULL"/"null"`.
`"Fake"`, whitespace, etc. return `nullopt`. More importantly it has **zero
callers anywhere** (including tests). Backend selection is purely compile-time
via the `CANOPY_ATTESTATION_BACKEND_NULL` macro; this function implies a runtime
/env path that does not exist. Either wire it (and define the fail-closed
behaviour on parse failure) or delete it. Dead security-relevant parsing code
invites a future caller to assume it does something it doesn't.

### S4 (Low, by-design but flag it) — `make_default_attestation_service_options` is "default" in name only

For a NULL build this returns a service that accepts any unattested peer
(`require_peer_evidence=false`, `allow_unattested_peer=true`,
`minimum_security_level=none`). The name `make_default_...` reads as "the safe
default to reach for". A caller wiring production code who picks the
"default" factory on a NULL build gets a wide-open service. The behaviour is
intended for demos; the *name* is the hazard. Consider
`make_demo_attestation_service_options` / `make_configured_...` and reserve
"default" for something that fails closed.

### S5 (Informational) — `requires_peer_evidence() ||` term is not redundant

In all three sites the guard is
`requires_peer_evidence() || !allows_unattested_peer()`. The first term looks
redundant but is not: it ensures `require_peer_evidence=true` always rejects
even if someone misconfigures `allow_unattested_peer=true`. This is correct
defense-in-depth and should be kept. No action — noted so it is not
"simplified" away later.

---

## Backward compatibility

- The factory is purely additive; existing call sites construct
  `attestation_service` explicitly and are unaffected. OK.
- The `produce_evidence` guard at `service.cpp:175` (reject when
  `backend->level() == none`) is new behaviour for any existing service
  configured with `send_local_evidence=true` and a none-level backend. Such a
  config was already broken (null backend produces empty Evidence), so this
  turns a silent bad state into an explicit failure. Acceptable, but it *is* a
  behaviour change — call it out in release notes.
- Wire compat: `hello_payload` advertises `requires_peer_evidence` but there is
  no `allow_unattested_peer` on the wire. That is fine — the decision is
  local-policy only — but it means the new flag is invisible to the peer and
  cannot be negotiated. Document this as intentional so no one "fixes" it by
  adding a wire field (which would be an attacker-controlled input).

## Scalability / Performance

No concerns. All changes are handshake-time or build-time. No hot-path,
allocation, or locking impact. `make_shared<backend>()` per service is
negligible.

## Maintainability

- M1: The factory (`backend_factory.{h,cpp}`), `parse_attestation_backend_kind`,
  and the `CANOPY_ATTESTATION_BACKEND_FAKE` macro are scaffolding consumed only
  by `attestation_service_test.cpp`. There is no production consumer. Shipping
  unused selection logic ahead of its caller means S1/S2 land without a
  review forcing function. Prefer landing the factory with its first real
  consumer.
- M2: Backend identity is duplicated: `attestation_backend_kind_name()` returns
  `null_backend_id`/`fake_backend_id`, and policy `required_backend_id` is set
  from it, while `backend->backend_id()` returns the same strings
  independently. They agree today by convention only. A single source (e.g.
  derive `required_backend_id` from `backend->backend_id()`) removes the drift
  risk.
- M3: `CANOPY_ATTESTATION_BACKEND_FAKE` is defined by CMake but never
  `#ifdef`-tested (only `_NULL` is). Harmless but misleading — either test it
  or stop defining it.

## Readability

- Minor: in `backend_factory.cpp`, the post-switch `return fake_backend_id;` /
  `return std::make_shared<fake_backend>();` read as "default to fake on
  purpose". If kept for compiler-warning reasons, add
  `// unreachable: enum is exhaustive` or use an explicit
  unreachable/terminate so intent is unambiguous (and so it is not mistaken
  for an intentional fallback per S2).
- Test `attested_streaming_transport_poc_suite.cpp:1900` assertion changed from
  `peer_attested == requires_peer_evidence()` to
  `peer_attested == peer_sends_evidence()`. This is a correctness fix
  (`peer_attested` should mean "the peer actually attested", not "I required
  it"); the old assertion passed only by coincidence of the old test matrix.
  Good change — no action, noted because it is easy to misread as a regression.

## Top recommendations (priority order)

1. Flip the build default to fail closed (S1). This is the only change with
   real production consequence.
2. Make the enum-switch fallbacks fail closed, not to fake (S2).
3. Delete or fully wire `parse_attestation_backend_kind` (S3).
4. Rename `make_default_attestation_service_options` away from "default" (S4).
5. Land the factory with its first production consumer rather than ahead of it
   (M1).

## Verification performed

Read full staged diff and surrounding context in the main repo: factory,
null backend, service guard, all three handshake gates
(`streaming/attestation/src/stream.cpp:595`,
`transports/sgx_coroutine/enclave/src/service.cpp:552` and `:810`), and
caller search. Confirmed factory has no non-test consumer and
`parse_attestation_backend_kind` has no callers. Did not build or run tests.
Did not inspect any worktree.
