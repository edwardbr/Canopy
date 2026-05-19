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

Backend-neutral does not mean both peers run the same TEE. The handshake should
allow each side to produce Evidence in its native format and appraise the
peer's Evidence with a different verifier backend. A route between SGX and
SEV-SNP is therefore valid when:

- the SGX side can produce SGX Evidence accepted by the SEV-SNP side, or by a
  verifier service the SEV-SNP side trusts;
- the SEV-SNP side can produce SEV-SNP Evidence accepted by the SGX side, or by
  a verifier service the SGX side trusts;
- both Evidence blobs are bound to the same Canopy transcript or derived key
  exchange;
- each destination zone policy explicitly accepts the peer backend, security
  level, measurement, signer/author identity, debug state, and TCB or firmware
  status.

If only one side can appraise the other's native Evidence, the route may still
be useful as one-way attested or certificate-authenticated, but it must not be
recorded as bidirectionally attested.

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

The build system enforces that separation. `CANOPY_PRODUCTION_RELEASE` defaults
to `OFF`, and formal release presets opt in explicitly. When it is `ON`, it
rejects `CANOPY_ATTESTATION_BACKEND` values `FAKE` and `SGX_SIM`,
`CANOPY_SGX_BACKEND=Fake`, and SGX enclave simulation (`SGX_HW=OFF`). It also
forces `CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS=OFF`, so the fake and
SGX-sim attestation backend implementations are not compiled into the
production `security_attestation` library. Release-like simulation presets,
such as `Release_SGX_Sim`, are build tests rather than formal releases and must
set `CANOPY_PRODUCTION_RELEASE=OFF` explicitly.

Evidence and encrypted session keys are separate concerns. A backend can prove
or simulate identity without creating confidential transport key material. Fake
and SGX-simulation sessions may use development key derivation for tests, but
hardware-grade verdicts such as SGX EPID or DCAP must be paired with an
explicit shared secret from a key exchange bound to the same transcript before
protected RPC is established.

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

The current `SGX_SIM` backend is a Canopy development profile with three
evidence paths:

- host-only builds fall back to signed development evidence with
  `backend_id == "sgx-sim"`;
- Intel SGX simulation enclave self-tests can produce an IDL-defined
  `sgx_sim_report_evidence` payload that binds the Canopy transcript into
  `sgx_report_data_t`, calls `sgx_self_target`, calls `sgx_create_report`, and
  requires the producer to verify that self-targeted report with
  `sgx_verify_report` before sending it;
- route handshakes can carry an IDL-defined
  `sgx_sim_local_attestation_challenge` in the backend-neutral
  `verifier_challenge` CMW. The peer responds with
  `sgx_sim_local_attestation_report`, targeted to the verifier enclave's
  `sgx_target_info_t` and verified by the verifier with `sgx_verify_report`.

This lets an SGX-sim build exercise stricter simulation policy defaults and SGX
SDK report mechanics without confusing that result with production hardware
evidence.

The runtime proof for these SGX-sim paths is test-only and uses the dedicated
`sgx_attestation_test_enclave` plus `sgx_attestation_test_host` targets rather
than piggybacking on unrelated transport tests.

SGX simulation should exercise as much Intel SGX SDK simulation machinery as
the installed SDK exposes on non-SGX hardware, including AMD developer
machines:

- enclave creation, ECALL/OCALL, and EDL ABI flow already used by the SGX
  coroutine transport;
- enclave-side report-data binding for the Canopy transcript;
- `sgx_create_report` / `sgx_verify_report` style local-report flow where the
  SGX simulation libraries support it;
- SGX quote or `sgx_ttls` simulation helpers only if they are available without
  hardware services.

This should still report `security_level == simulation`. It tests the Canopy
enclave code path, SGX SDK integration, evidence binding, and verifier plumbing.
It does not prove hardware isolation, platform TCB, or a production quote.

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

Canopy now has the first DCAP backend slice:

- `interfaces/attestation/sgx_dcap_protocol.idl` defines a typed
  `sgx_dcap_report_binding`, `sgx_dcap_quote_evidence`, and optional
  `sgx_dcap_verification_result` carrier outside `rpc_types.idl`;
- `sgx_dcap_backend` is selectable as `CANOPY_ATTESTATION_BACKEND=DCAP`;
- the default backend has no quote provider or verifier and therefore fails
  closed with typed unavailable Evidence;
- injected quote-provider and quote-verifier interfaces define the seams where
  `sgx_qe_get_target_info`, `sgx_create_report`, `sgx_qe_get_quote`,
  `sgx_qv_verify_quote` or `tee_verify_quote`, and QvE/TVL appraisal must be
  wired on a DCAP-capable machine.

This current DCAP backend is a protocol, policy, and build-selection skeleton.
It does not yet call Intel DCAP quote-generation or quote-verification
libraries. Production confidentiality and peer authenticity still require the
real DCAP provider/verifier adapter plus the transcript-bound key exchange
described in the implementation plan.

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

## Cross-TEE Attestation

Cross-TEE attestation is a first-class design goal. The route handshake carries
typed CMW Evidence, not a hard-coded SGX quote field, so the producer backend
and verifier backend can differ. The attestation service should select the
verifier from the peer Evidence type and local policy, not from the local
enclave's own TEE type.

Examples:

- SGX DCAP service to SEV-SNP service: request Evidence may be SGX DCAP and
  response Evidence may be SEV-SNP.
- TDX service to SGX EPID compatibility service: policy may accept EPID only
  for specific legacy peers and require TDX or DCAP for all others.
- Host or browser verifier to enclave service: the host/browser side is
  verify-only and cannot produce enclave Evidence, but it can still verify the
  server and participate in protected key exchange under explicit policy.

This requires a backend registry with separate production and verification
capabilities:

```text
local_evidence_backend       = backend used to describe this enclave or zone
peer_evidence_content_format = CMW content format received from the peer
peer_verifier_backend        = backend selected to appraise that content format
accepted_peer_backend_ids    = policy allow-list for the destination zone
accepted_peer_security_level = minimum required peer security level
```

The route `security_context` must preserve the peer's appraised backend id,
security level, workload identity, platform identity, and verifier path. Policy
can then distinguish "this peer is SGX DCAP production", "this peer is SEV-SNP
production", "this peer is EPID legacy compatibility", and "this peer is only
certificate-authenticated".

Verifier delegation is allowed but must be explicit. If an enclave cannot
embed a SEV-SNP, TDX, or PSA verifier directly, it may use the RATS passport
model or a background-check verifier service. In that case the trusted
Attestation Result, not the raw verifier's word alone, must be bound to the
Canopy transcript and accepted by destination-zone policy.

## SGX EPID/IAS Backend

Some SGX1 machines support EPID remote attestation but not DCAP. That hardware
can still be useful for compatibility testing and for exercising a real SGX1
attestation path.

EPID should be treated as a legacy backend behind the same attestation service
interface. It is not the default forward production target for new deployments,
but the protocol should not preclude an EPID verifier where an application
policy explicitly accepts it.

Canopy now has the initial EPID backend shape:

- `interfaces/attestation/sgx_epid_protocol.idl` defines
  `sgx_epid_report_binding`, `sgx_epid_ias_report`, and
  `sgx_epid_quote_evidence`;
- `sgx_epid_report_binding` is canonical_crypto encoded and hashed with
  SHA-256; the digest is the value that the quote provider must bind into
  SGX `report_data`;
- `sgx_epid_backend` is selectable as `CANOPY_ATTESTATION_BACKEND=SGX_EPID`;
- without an injected quote provider and quote verifier it returns a typed
  unavailable CMW and verification fails closed;
- injected providers and verifiers are the only code that should call Intel
  PSW/AESM, IAS, or quote parsing APIs;
- `backend_factory_overrides` can supply a prebuilt `attestation_backend`, so
  SGX-specific provider/verifier objects do not leak into the neutral factory
  header;
- the backend applies defensive payload and per-field size caps before handing
  decoded quote material to a verifier.

The current EPID backend is therefore a protocol and policy skeleton, not a
complete IAS implementation. The next runtime slice on SGX1 hardware should
wire a provider that obtains an EPID quote from the SGX PSW and a verifier that
checks the IAS report signature, quote status, revocation/collateral state, and
that the quote report_data contains the Canopy transcript hash.

That verifier contract is load-bearing. A production or demo verifier must
reject unless it has checked the IAS signature, IAS signing certificate chain
and trust anchor, quote status, advisory ids, revocation material, quote
freshness, `sgx_report_data_t == report_data_sha256 || zero(32)`,
`MRENCLAVE`, `MRSIGNER`, `ISVPRODID`, `ISVSVN`, debug state, and local policy.
The backend normalizes accepted EPID verdicts to `security_level ==
hardware_legacy`; it must not be promoted to the DCAP/TDX class of hardware
evidence.

Intel IAS/EPID is a fading trust anchor. New production deployments should
prefer DCAP/ECDSA or a newer TEE backend. EPID should ship only for explicit
legacy interop, demonstrations on SGX1 hardware, or environments whose policy
owner has deliberately accepted IAS dependence.

EPID quote verification does not create the protected-RPC session key. The
route handshake must also carry, or be bound to, a real key exchange whose
shared secret is passed into `attestation_service::establish_session`. Until
that exists, an EPID verdict can prove the peer identity for policy decisions
but must not be used to derive AEAD keys from public transcript data.

EPID has different operational requirements from DCAP, such as dependence on an
IAS-style verifier flow and different collateral or privacy properties. Those
details should stay inside the backend and verifier adapter.

The expected hardware split is:

- SGX simulation on development machines, including machines without SGX
  hardware, for SDK and enclave-path coverage only;
- SGX1 EPID on legacy SGX hardware where DCAP is unavailable;
- SGX DCAP and Intel TDX on a separate machine with the required FLC/DCAP
  platform support and collateral services.

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
