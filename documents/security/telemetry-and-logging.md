<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Telemetry And Logging Security

Logging and telemetry are part of the security boundary when a transport crosses
an enclave, process, or network boundary.

Logging records are generated from `interfaces/rpc/logging.idl` as
`rpc::log_record`. Richer telemetry payloads live in
`interfaces/rpc/telemetry_types.idl`, and both are carried across internal
boundaries in the `rpc::telemetry_event` envelope from `interfaces/rpc/rpc_types.idl`.
Receivers identify the payload by its generated fingerprint ID rather than by a
separate telemetry enum.

## Attack Vectors

### Log Exfiltration

Attack:

- sensitive object data is written to logs
- serialized payloads are logged during error handling
- enclave logs are sent through host-controlled OCALLs

Mitigation:

- never log secrets, raw payloads, keys, or object contents
- cap untrusted string lengths before logging
- prefer structured reason codes over raw input dumps
- route SGX coroutine telemetry through the stream path when available
- keep OCALL logging as a minimal fallback only

### Log Injection

Attack:

- hostile input includes control characters or misleading text
- logs appear to show a different error or source

Mitigation:

- avoid logging untrusted strings directly
- escape or encode untrusted text
- include structured zone and transport identifiers where safe

### Diagnostic State Exposure

Attack:

- debug health checks expose object ids, route maps, reference counts, or
  scheduler state
- diagnostic code remains enabled in production paths

Mitigation:

- keep high-detail diagnostics out of default builds
- remove temporary investigation logs after fixes
- use narrow, opt-in telemetry for security review
- do not expose enclave object topology through host-visible logs

## SGX-Specific Rule

The enclave should assume OCALL logging is observable and controllable by the
host. For normal operation, SGX coroutine logs should be telemetry events over
the stream transport. OCALL logging should only cover startup-before-stream and
stream-submission failure cases, and those messages must be low detail.

## Ordered Diagnostics

OCALL logging is not ordered with SPSC protocol traffic. During SGX coroutine
debugging, OCALL-backed logs made enclave-side add-ref progress appear to happen
after the host had sent a later protocol message, even though the OCALL log could
overtake the SPSC message it described.

When investigating stream protocol ordering, prefer diagnostics carried on the
same ordered path as the protocol message being observed. Do not add high-volume
`RPC_INFO` calls inside enclave stream or add-ref hot paths: when enclave logs
share the normal SPSC stream, logging itself can fill the enclave-to-host queue
and perturb the protocol.

For stalled pollers, use a narrow diagnostic path outside the hot protocol lane.
A useful watchdog record includes:

- enclave zone id
- runtime loop iteration or progress counter
- worker count and per-worker progress counters
- scheduler ready/timer/thread-pool sizes
- transport adjacent zone
- last receive/send sequence observed by each stream transport

This watchdog path should be opt-in diagnostic infrastructure, not normal
logging.
