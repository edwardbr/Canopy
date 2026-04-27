<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Security Notes

This folder records attack surfaces, security invariants, and mitigation work
for Canopy. It is intentionally separate from the general architecture pages:
the architecture pages describe how the system should work, while these pages
describe how it can be abused.

The live C++ repository remains the source of truth. These documents are a
security review aid and a checklist for hardening work.

## Current Documents

- [Untrusted Transport Input](untrusted-transport-input.md)
- [Reference Protocol Security](reference-protocol-security.md)
- [Telemetry And Logging Security](telemetry-and-logging.md)

## Review Principles

- Treat host-controlled memory as hostile, even when it is required for normal
  operation.
- Treat malformed protocol input as an attack, not merely a transport error.
- Distinguish confidentiality and integrity failures from denial of service.
- Fail closed when protocol state is impossible.
- Do not continue executing with sensitive state after fraudulent input.
- Prefer explicit state machines over inferred lifecycle behaviour.
- Keep shutdown and release paths secure, because object destruction can send
  security-relevant messages back to another zone.

## Categories To Expand

- remote attestation and key establishment
- authenticated stream framing
- replay and reordering protection
- object capability boundaries
- generated binding validation
- denial-of-service boundaries and timeouts
- fuzzing and negative protocol tests
