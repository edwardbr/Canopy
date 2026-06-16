<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Attestation And Protected Envelope Failure Policy

## Principle

Attestation failures, protected-envelope authentication failures, replay
detections, and inner/outer identity mismatches are security events. Treat
authenticated tamper, replay, downgrade, and impossible protocol sequencing as
fraudulent or hostile until proven otherwise.

Do not classify every rejected message as fraud. In particular, an unknown IDL
fingerprint or unsupported protected-control payload can simply mean the peer
was built from a newer schema. That should fail closed as a version or
compatibility error, not as `FRAUDULANT_REQUEST`, because fraud handling may
feed temporary or permanent blacklists.

A backend downgrade is also a security event. For example, fake or simulation
evidence presented to a policy that requires hardware attestation must fail
closed, even if the fake transcript is internally well formed.

Application policy decides whether a specific failure causes:

- rejection of only the message;
- suspicious-route accounting;
- adjacent transport close;
- route revocation;
- temporary or permanent blacklist;
- enclave-local fatal handling.

## Caller Response

The remote caller should receive only the minimal result needed for protocol
progress. `ZONE_NOT_SUPPORTED` is the current compatibility error for peers
that do not support required protected envelopes or attestation features.
`INVALID_VERSION` is appropriate for unsupported protocol versions, message
types, or generated IDL fingerprints.

Use `FRAUDULANT_REQUEST` only for failures that indicate a security violation
or impossible protocol sequence, such as:

- normal-mode RPC/control traffic before the route has been admitted by
  `add_ref` or an attestation handshake;
- plaintext downgrade attempts on a route that already has a protected
  context;
- protected-envelope authentication failure;
- protected-envelope replay or out-of-order receive counters;
- authenticated inner/outer binding mismatch;
- invalid request-scoped capability handoff.

More specific public error codes are deferred until a full security audit
defines which details can be disclosed safely.

## Local Logging And Telemetry

Local logging and telemetry may contain more detail than the remote response,
including:

- remote caller zone id;
- remote enclave id;
- local zone id;
- failing validation stage;
- public attestation identity fields;
- policy rule that rejected the message;
- source file and line number if local logging policy allows it.

Never log:

- encryption keys;
- private keys;
- decrypted application payloads;
- raw nonce derivation state if it includes secret salt or key material;
- sealed state material.

The current RPC logs include line numbers and internal error codes in some
paths. Those paths need review before production attestation support is enabled.

## Route Liveness

In routed topologies, an intermediate may be the only channel for reporting
that a downstream zone crashed or disconnected. Such reports can be trusted
for route liveness according to policy, but should not automatically be treated
as authoritative evidence about the downstream enclave's internal state.

For example, if `A -> B -> C` and C disappears, B may tell A "my route to C is
down." B should not be assumed to prove that C's enclave crashed unless a
stronger attested control protocol is added.

## Epochs And Replay

Each enclave restart or session rekey creates a fresh epoch. End-to-end message
counters are valid only within their key epoch.

Replay detection failure defaults to message rejection and suspicious-route
accounting. Escalation is policy-driven.
