<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Attestation Backends

## Purpose

The protected-RPC design targets SGX first, but it must not bake SGX-only
concepts into the protocol or service boundary. The same architecture should
port to Intel TDX, AMD SEV-SNP, Arm TrustZone/PSA attestation, and future TEE
backends.

The attestation service therefore exposes a backend-neutral interface with
explicit backend identity and policy checks. Development machines that do not
provide SGX remote attestation use fake evidence behind the same interface.

## Backend Matrix

| Backend | Purpose | Production trust |
|---|---|---|
| Null | Explicit no-attestation policy for demos/browser clients | No |
| Fake | Host development and tests | No |
| SGX simulation | SDK/runtime development | No |
| SGX local report | Same-platform enclave pairs | Yes, for local SGX policy |
| SGX DCAP/ECDSA | Modern SGX remote attestation | Yes |
| SGX EPID/IAS | Legacy SGX1 remote attestation | Compatibility only |
| Intel TDX | Future VM TEE backend | Future |
| AMD SEV-SNP | Future VM TEE backend | Future |
| Arm TrustZone/PSA | Future device TEE backend | Future |
| Non-enclave verify | Browser/host verification | Verify-only |

Fake and SGX simulation backends are useful on machines without SGX hardware,
including non-Intel development systems. They must never silently satisfy a
production hardware-attestation policy.

## Null Backend

The null backend is the explicit no-attestation option. It exists so demos,
browser clients, diagnostics, or other non-enclave clients can connect only
when service policy deliberately permits an unattested peer.

Null is not evidence. It has `backend_id == "null"`, `security_level == none`,
does not produce accepted local Evidence, and rejects any Evidence verification
attempt. Accepting a peer with no Evidence therefore must flow through policy:
`require_peer_evidence == false` plus `allow_unattested_peer == true`.
Disabling `require_peer_evidence` alone is not sufficient.

## Fake Attestation Backend

The fake backend is a first-class development backend. Its job is to let the
same service, transport, envelope, key-exchange, counter, and failure-policy
code run without renting or owning a DCAP-capable SGX machine.

The fake backend should:

- implement the same attestation service interface as hardware backends;
- produce explicit fake evidence that names the backend as `fake`;
- create deterministic or configured fake enclave identities for tests;
- bind fake evidence to the key-exchange transcript;
- establish the same session id, key, epoch, and counter state used by protected
  envelopes;
- support negative tests for stale evidence, wrong identity, replay, and
  unsupported policy;
- fail closed when production policy requires hardware-backed evidence.

The fake backend may sign fake evidence with a development key so the transcript
exercises real cryptographic verification flow. That key is not a production
root of trust and must not be accepted by production enclave policy.

## SGX Simulation Backend

SGX simulation is useful for Intel SDK/runtime integration, but it is not remote
attestation evidence. Treat simulation evidence as development evidence with a
distinct backend id.

Simulation success can prove that the Canopy code path and enclave ABI are
functioning. It does not prove hardware isolation, platform TCB, or production
quote verification.

## SGX DCAP/ECDSA Backend

DCAP/ECDSA is the forward target for modern SGX remote attestation. This backend
is responsible for quote generation or verification, collateral handling, TCB
status evaluation, and binding the quote/report data to the session key
exchange.

The protected-RPC layer should not depend directly on DCAP data structures. It
should consume a backend-neutral verdict from the attestation service.

## Future TEE Backends

TDX, SEV-SNP, and TrustZone/PSA should be backend additions, not protocol
forks. Each backend maps its native evidence into CMW and returns the same
backend-neutral verdict shape used by SGX:

```text
backend_id
security_level
attested_platform_identity
attested_workload_identity
measurement fields
debug / production state
TCB or firmware/security-version status
bound key or transcript hash
raw evidence media type
policy verdict
```

The field names and policy checks will differ by backend. SGX uses
`MRENCLAVE`, `MRSIGNER`, `ISVPRODID`, `ISVSVN`, and SGX TCB collateral. TDX,
SEV-SNP, and TrustZone/PSA use different measurement and endorsement models.
Those differences belong inside the backend and policy layer, not in the RPC
envelope or transport.

The SGX implementation should avoid hard-coding SGX names into shared types
unless the type is explicitly SGX-specific. Shared records should use neutral
terms such as workload measurement, signer or author identity, product id,
security version, debug state, and TCB status.

## SGX EPID/IAS Backend

Some SGX1 machines support EPID remote attestation but not DCAP. That hardware
can still be useful for compatibility testing and for exercising a real SGX1
attestation path.

EPID should be treated as a legacy backend behind the same attestation service
interface. It is not the default forward production target for new deployments,
but the protocol should not preclude an EPID verifier where an application
policy explicitly accepts it.

An EPID backend is expected to have different operational requirements from
DCAP, such as dependence on an IAS-style verifier flow and different collateral
or privacy properties. Those details should stay inside the backend.

## Backend Capabilities

Each backend should expose capabilities that policy can inspect before accepting
an attestation result:

```text
backend_id
security_level              = development | simulation | hardware_legacy | hardware
can_produce_evidence
can_verify_evidence
supports_local_attestation
supports_remote_attestation
requires_online_verifier
supports_production_policy
```

Protected RPC should bind the selected backend id and security level into the
attestation transcript and session context. A downgrade from hardware policy to
fake, simulation, or legacy evidence is a policy failure.

## Development Policy

Development builds may deliberately allow fake or simulation attestation so the
RPC security layer can be built and tested on ordinary hardware.

Production builds should make this impossible by policy, preferably through
compile-time constants inside the enclave or verifier. A runtime flag should
not be enough to make fake evidence acceptable in a production policy.
