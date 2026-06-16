<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Untrusted Transport Input

This document covers attacks through transport bytes, shared queues, and stream
frames. It applies to SGX most strongly, but the same categories are useful for
network transports and IPC transports.

## Core Rule

Transport input is untrusted until it has passed framing, authentication,
deserialisation, and semantic protocol validation.

For SGX coroutine transport, the shared SPSC queue lives outside the enclave.
The host can corrupt or replace queue contents. The enclave must therefore treat
queue bytes as hostile even after the ECALL setup pointers were validated.

For enclave zones, the host may be required for routing but must not be trusted
with application plaintext or capability secrets. Host-visible headers should be
limited to the routing data needed to deliver a frame, and those headers should
be authenticated as associated data.

## Attack Vectors

### Malformed Frames

Attack:

- invalid prefix length
- payload length larger than policy allows
- unknown direction or message kind
- truncated payload
- deserialisation failure

Mitigation:

- impose hard maximum prefix and payload sizes
- reject unknown enum values
- deserialize into bounded temporary buffers
- mutate service state only after the full frame is valid

### Replay And Reordering

Attack:

- replay an old add-ref, release, call, or response
- reorder messages to produce impossible object lifetimes
- replay close handshakes

Mitigation:

- authenticated monotonically increasing sequence numbers per direction
- track outstanding request ids
- reject duplicate responses
- reject stale or unexpected one-way messages
- include message kind and direction in authenticated data

### Cross-Zone Message Injection

Attack:

- forge source or destination zone ids
- use a valid object id in the wrong zone
- claim a route that was never established

Mitigation:

- validate source and destination zone against the active transport adjacency
- validate route and passthrough state before dispatch
- reject calls to objects not owned by the addressed service
- bind source, destination, route epoch, direction, and message kind into the
  authenticated frame data

### Reference Protocol Manipulation

Attack:

- send release without a matching add-ref
- send add-ref for an object not exposed to the caller
- use optimistic references to bypass ownership checks

Mitigation:

- validate object existence and caller authority
- keep reference-count transitions monotonic and auditable
- fail closed on underflow or impossible ownership state
- reject lifecycle messages that are validly encoded but not authorised for the
  authenticated peer identity

### Downgrade And Negotiation Abuse

Attack:

- negotiate a weaker serialisation, no authentication, no encryption, stale
  attestation policy, or a debug enclave when the peer requires production
  enclave identity
- replay a previous handshake to reuse old keys or route epochs

Mitigation:

- authenticate the complete negotiated feature set
- include nonces, protocol versions, encoding set, compression mode, security
  policy, and attestation requirements in the handshake transcript
- derive session keys only after negotiation and attestation checks pass
- fail closed when a required security feature is missing

### Queue Corruption

Attack:

- corrupt queue head/tail fields
- make queue appear empty forever
- make queue point to stale entries
- interleave payloads from different logical streams

Mitigation:

- treat queue corruption as transport failure or fraud
- authenticate every logical message above the queue
- do not trust queue metadata for security decisions
- keep parsing bounded even if queue metadata is hostile

Additional SGX queue hardening:

- wrap host-owned boundary queues with guard bytes before and after the queue
  object where practical
- expose only the validated inner queue pointer to the enclave
- have host-side diagnostic drainers check guard bytes while tests run, so queue
  object overruns are reported before timeout kills the process
- reject blob length headers larger than the stream `max_payload`, preventing a
  corrupted queue entry from reading past a fixed-size blob
- measure queue pressure without adding high-volume SPSC log traffic from inside
  the enclave hot path

The current SPSC stream chunks outgoing data into queue blobs, so one oversized
RPC payload should become multiple bounded blobs rather than one overlarge queue
slot. Back pressure can still happen if the consumer side does not drain those
blobs fast enough.

## Authenticated Stream Framing

SGX needs an authenticated stream layer before the normal RPC transport acts on
messages. A recommended frame layout is:

```text
frame {
    magic/version
    direction
    message_kind
    source_zone
    destination_zone
    route_epoch_or_session
    sequence
    request_id
    payload_length
    ciphertext
    auth_tag
}
```

The authenticated associated data should include all header fields. Payloads
should be serialised first, optionally compressed, and then encrypted and
authenticated. Inbound payloads should not be decompressed or deserialised until
the authentication tag has passed.

Payload encryption should be mandatory when an untrusted intermediary, host, or
passthrough can observe the transport bytes. Routing metadata may remain
visible, but it must be authenticated so a host cannot redirect ciphertext
without detection.

Without authenticated framing, pointer validation and worker admission only
protect the ECALL control plane. They do not protect against hostile queue
messages.
