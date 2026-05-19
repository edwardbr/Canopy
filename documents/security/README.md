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

- [Security Hardening Roadmap](security-hardening-roadmap.md)
- [SGX Enclave Threat Model](sgx-threat-model.md)
- [SGX Runtime Lifecycle Security](sgx-runtime-lifecycle.md)
- [Attestation And Protected RPC](attestation/overview.md)
- [Untrusted Transport Input](untrusted-transport-input.md)
- [Reference Protocol Security](reference-protocol-security.md)
- [Telemetry And Logging Security](telemetry-and-logging.md)

## Review Principles

- Treat host-controlled memory as hostile, even when it is required for normal
  operation.
- Treat malformed protocol input as an attack, not merely a transport error,
  while keeping unsupported versions or unknown generated fingerprints in the
  compatibility-failure category unless another security invariant is violated.
- Distinguish confidentiality and integrity failures from denial of service.
- Fail closed when protocol state is impossible.
- Do not continue executing with sensitive state after fraudulent input.
- Prefer explicit state machines over inferred lifecycle behaviour.
- Keep shutdown and release paths secure, because object destruction can send
  security-relevant messages back to another zone.

## Categories To Expand

- backend-neutral attestation and key establishment, including fake
  development, SGX local, EPID/IAS legacy, and DCAP/ECDSA paths
- authenticated stream framing and payload encryption
- TLS and RA-TLS inside enclave-safe stream layers
- replay and reordering protection
- object capability boundaries and `zone_address` validation tokens
- generated binding validation and method-level authorisation
- limited public-client gateway profiles
- passthrough route authority
- denial-of-service boundaries and timeouts
- fuzzing and negative protocol tests
- enclave zeroisation and fatal shutdown policy
