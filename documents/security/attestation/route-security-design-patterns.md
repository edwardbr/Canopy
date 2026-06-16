<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Route Security Design Patterns

## Purpose

Canopy should support more than one way to establish trust between zones.
Hardware attestation is one family of route-security mechanisms, but it is not
the whole design. The same route may also be protected by certificate-authenticated
key agreement, pinned public keys, pre-shared trust anchors, or a cloud verifier
token. These mechanisms make different claims, but they all need the same
runtime plumbing:

- select a route-security backend;
- bind the handshake to the exact Canopy route and transcript;
- derive or import session keys;
- record normalized route-security state;
- apply destination-zone policy;
- protect later RPC envelopes with the accepted session keys.

The design goal is to keep SGX, TDX, SEV-SNP, Arm CCA/TrustZone, Nitro,
TPM/DICE, GPU attestation, and TLS-style route key transfer behind backend
boundaries. Common Canopy code should speak in terms of route security,
evidence, identity, keys, claims, and policy. It should not require every new
backend to look like SGX.

## Vocabulary

Use backend-neutral names in common code and IDL:

- **Route security**: the umbrella term for attestation-backed and
  non-attestation-backed protected routes.
- **Attester**: a party that produces evidence about itself.
- **Evidence**: backend-specific proof material, such as an SGX DCAP quote,
  SEV-SNP report, TDX quote, Arm CCA token, Nitro attestation document, TPM
  quote, or a certificate-chain proof.
- **Verifier**: code or service that validates evidence against endorsements,
  reference values, and policy.
- **Relying party**: the peer or service that consumes the result and decides
  whether to allow the route.
- **Attestation result**: normalized claims produced by a verifier after
  evidence appraisal.
- **Route identity**: the Canopy-level identity for a zone, service, enclave,
  guest, realm, process, or certificate subject.
- **Session binding**: the cryptographic link between evidence, identity, the
  handshake transcript, and the keys used by protected RPC.

Reserve backend-specific words such as `enclave`, `realm`, `guest`, `host`,
`quote`, and `report` for backend implementation code and backend-specific IDL
files. Do not make them required vocabulary in the common route-security API.

## Families To Consider

The following technologies are relevant when designing the common abstraction,
even if Canopy does not implement all of them immediately.

| Family | Native evidence | Useful design lesson |
|---|---|---|
| Intel SGX DCAP | SGX ECDSA quote plus collateral | Process/enclave evidence with quote collateral and explicit report-data binding. |
| Intel SGX EPID | Legacy EPID quote plus IAS report | Legacy online-verifier model. Keep compatibility isolated from modern SGX paths. |
| Intel TDX | TDX quote | VM-level confidential computing. Similar relying-party shape to DCAP, but not an enclave process model. |
| AMD SEV-SNP | SNP attestation report plus VCEK/cert chain | VM-level measurement and firmware/TCB policy. Useful for whole-guest route identity. |
| Arm CCA/RME | CCA token, usually EAT-profiled | Realm model and standards-oriented claims. Prefer CCA for server confidential computing over classic TrustZone. |
| Arm TrustZone/PSA | PSA attestation token | Device and embedded model. Useful for constrained or appliance deployments. |
| AWS Nitro Enclaves | Nitro attestation document with PCRs | Provider-specific enclave identity and KMS-integrated policy. |
| TPM 2.0 | TPM quote over PCRs | Platform and measured-boot signal. Useful as a companion proof, not a TEE by itself. |
| DICE | Compound device identity and measurements | Layered device identity and measured software chain. Useful for embedded roots. |
| Cloud verifier tokens | JWT/OIDC/EAT-like token | Canopy may consume a verifier result without owning the raw quote-verifier code. |
| GPU confidential computing | GPU attestation report | Separate accelerator identity for workloads that move secrets or models onto GPU memory. |
| Certificate route security | Certificate chain or pinned key | Authenticated key transfer without claiming TEE isolation. |

The table is not a roadmap. It is a checklist for keeping the route-security
model wide enough.

## Common Backend Shape

Every route-security backend should fit these interfaces conceptually:

```text
produce_handshake_material(binding) -> route_security_material
verify_handshake_material(material, binding, policy) -> route_security_result
derive_or_import_session_keys(transcript, key_agreement, result) -> session
```

For attestation backends, `route_security_material` contains evidence. For a
certificate-only backend, it contains a certificate chain, signature, and key
share. For a pre-shared-key backend, it may contain only a key id, nonce, and
MAC over the transcript.

The result should not be a raw backend structure. It should be normalized into
a Canopy-owned result with fields such as:

- backend id and evidence format;
- verification mode: local, delegated verifier, or unauthenticated development;
- route identity and asserted subject;
- platform family and product id when available;
- workload measurement;
- signer, author, or certificate subject;
- debug or production state;
- security version, TCB status, or firmware level;
- verifier identity;
- freshness result;
- session-binding result;
- policy result;
- security level used by Canopy policy.

Backend-specific details can remain available as opaque evidence or extension
claims, but ordinary route policy should not need to parse raw SGX, SNP, TDX,
CCA, Nitro, or TPM bytes.

## Evidence Versus Key Transfer

Attestation and key transfer are related but distinct.

Attestation answers questions like:

- What environment produced this evidence?
- What code, image, firmware, or signer was measured?
- Is debug enabled?
- Is the platform TCB acceptable?
- Did the evidence bind to this handshake?

Key transfer or key agreement answers different questions:

- Which peer controls the private key used in this handshake?
- Which route and transcript produced this shared secret?
- Which session id, epoch, and counters should protect future RPC records?
- Which intermediates can read or modify which fields?

Do not assume hardware evidence creates transport keys by itself. A hardware
backend must still bind evidence to a key exchange or to a key imported through
a trusted channel. A certificate-only backend can establish strong channel
confidentiality without making any claim about enclave or VM isolation.

## Passport And Background-Check Flows

Canopy should support both common remote-attestation topologies:

- **Passport**: the attester sends evidence to a verifier first, receives a
  signed result token, then presents that result to the relying party.
- **Background check**: the attester sends raw evidence to the relying party,
  and the relying party asks a verifier to appraise it.

The route-security protocol should carry either raw evidence or a verifier
result. Policy decides which is acceptable. This is important for cloud
deployments where Azure Attestation, Intel Trust Authority, Google Cloud
Attestation, AWS KMS, or another verifier service may be the party that appraises
the hardware evidence.

## Layered Evidence

Some deployments need more than one proof. Examples:

- SGX DCAP quote plus application manifest hash;
- SEV-SNP report plus container image digest;
- CCA token plus TPM measured boot;
- Nitro attestation document plus a cloud identity token;
- certificate-authenticated route plus a deployment-signed workload manifest.

The common protocol should eventually allow a vector of evidence entries and a
vector of normalized claim sets. The policy engine can then express rules such
as "accept this route only if the TEE measurement matches and the container
image digest is in this release set".

## IDL Direction

The current IDL layout is mostly the right starting point:

- `interfaces/attestation/route_attestation_protocol.idl` carries the
  route-level handshake request and response.
- `interfaces/attestation/protected_rpc_protocol.idl` defines the protected
  envelope metadata and encrypted RPC payload shapes.
- `interfaces/attestation/sgx_dcap_protocol.idl`,
  `interfaces/attestation/sgx_epid_protocol.idl`, and
  `interfaces/attestation/sgx_sim_protocol.idl` keep SGX-specific payloads
  outside the common route protocol.

Expected future IDL changes:

- Rename or supersede SGX-shaped common fields such as `enclave_id` with
  backend-neutral names such as `security_domain_id`, `subject_id`, or
  `route_identity`.
- Add a neutral route-security envelope that can carry either attestation
  evidence or certificate/key-agreement material.
- Add a neutral normalized result or claims structure.
- Add extension claims, for example `{ namespace, name, value }` or
  `{ claim_uri, value }`, so new backend claims do not force a common-IDL
  revision for every platform.
- Add multi-evidence support for layered proofs.
- Keep backend-specific binding structures in backend-specific IDL files.

One possible future shape:

```text
route_security_material {
    backend_id
    material_kind              # evidence, verifier_result, certificate_chain,
                               # key_share, psk_mac, or extension
    media_type
    content_format
    payload
    extensions[]
}

route_security_claims {
    backend_id
    verifier_id
    subject_id
    platform_family
    measurement
    signer
    product_id
    security_version
    debug_state
    freshness_status
    session_binding_status
    policy_status
    extensions[]
}
```

This should not be implemented until the current SGX/DCAP path has proven the
minimum common fields. The point is to avoid painting the protocol into an
SGX-only corner.

## Certificate And TLS-Style Route Key Transfer

Canopy also needs a non-attestation route-security backend for TLS-style key
transfer between two zones across a chain of zones. This is not remote
attestation. It proves control of keys and accepted identities, not hardware
isolation.

For a route `A -> B -> C -> D`, the design should support:

- A and D establishing an end-to-end shared secret while B and C forward the
  handshake.
- B and C seeing only public routing AAD needed to route traffic.
- A and D binding the key schedule to source zone, destination zone, route
  hops, service/interface target, cipher suite, nonces, and transcript id.
- Optional policy where an intermediate is deliberately a terminating
  participant, for example a gateway that decrypts and re-encrypts under an
  explicit delegation rule.
- Rekey using session epochs.
- Failure modes that distinguish unauthenticated, authenticated-but-untrusted,
  expired, replayed, and policy-rejected routes.

This backend could be named `CERT_ROUTE`, `TLS_ROUTE`, or
`KEY_AGREEMENT_ROUTE`. It should share the protected RPC envelope and route
policy machinery, but it should not report `security_level::hardware` or claim
TEE evidence.

The common state should be able to represent:

```text
route_security_kind = key_agreement
backend_id = cert-route
peer_identity = certificate subject or pinned key id
session_id
session_epoch
public_route_aad_policy
endpoint_confidentiality = true
hardware_attested = false
```

That distinction matters for policy. A treasury zone may accept
`sgx-dcap + key agreement` for high-value operations, while a gateway zone may
accept `cert-route + key agreement` for ordinary authenticated clients.

## Intermediates And Visibility

Route security must preserve Canopy's passthrough model. Intermediates may need
to know:

- next hop;
- source route zone;
- destination route zone;
- protected carrier interface and method;
- session id or key id;
- counters needed for replay protection;
- route failure and liveness state.

Intermediates should not see endpoint-only details unless policy explicitly
makes them participants:

- application object ids;
- application interface ids;
- application method ids;
- application request and response payloads;
- endpoint-only attestation policy decisions;
- private key material;
- raw secrets imported into the route.

The existing protected-RPC split between public AAD and encrypted plaintext is
the right pattern for this.

## Build-System Pattern

Backend selection should happen at build and policy boundaries:

- Common route-security code is always available when the feature is enabled.
- Backend-specific source files are compiled only when the selected backend or
  an explicit backend-test build needs them.
- Hardware production builds should not drag in development backends.
- A DCAP build should not compile EPID backend components or SGX-simulation
  attestation tests.
- Future SEV-SNP, TDX, CCA/TrustZone, Nitro, TPM, or certificate-route
  backends should be new source blocks behind the same backend selection
  pattern, not branches inside SGX files.

This keeps the binary contents aligned with the selected security claim and
makes it easier to audit production builds.

## Policy Pattern

Route policy should evaluate normalized state, not backend internals. Useful
policy inputs include:

- allowed backend ids;
- minimum security level;
- allowed route-security kind: none, key-agreement, one-way attested, mutual
  attested, delegated verifier, or hardware attested;
- accepted measurements or release manifests;
- accepted signer, certificate subject, or author identity;
- debug-state allowance;
- minimum security version or TCB status;
- accepted verifier identities;
- allowed delegation through intermediates;
- whether endpoint confidentiality is required;
- whether route handshakes may be visible to intermediates.

The same endpoint can then express different policy for different zones. A
public gateway can accept certificate-authenticated clients. A private enclave
zone can require mutual hardware attestation and reject certificate-only routes.

## References

- IETF RFC 9334, Remote ATtestation procedureS (RATS) Architecture:
  <https://www.ietf.org/rfc/rfc9334.html>
- IETF RFC 9711, Entity Attestation Token (EAT):
  <https://www.ietf.org/rfc/rfc9711.html>
- Arm CCA attestation with Veraison:
  <https://learn.arm.com/learning-paths/servers-and-cloud-computing/cca-veraison/cca-attestation/>
- AWS Nitro Enclaves cryptographic attestation:
  <https://docs.aws.amazon.com/enclaves/latest/user/set-up-attestation.html>
- Azure Attestation overview:
  <https://learn.microsoft.com/en-us/azure/attestation/overview>
- Google Cloud Confidential Space:
  <https://docs.cloud.google.com/docs/security/confidential-space>
- Intel Trust Authority attestation overview:
  <https://docs.trustauthority.intel.com/main/articles/articles/ita/concept-attestation-overview.html>
