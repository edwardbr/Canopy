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

### Reference Protocol Manipulation

Attack:

- send release without a matching add-ref
- send add-ref for an object not exposed to the caller
- use optimistic references to bypass ownership checks

Mitigation:

- validate object existence and caller authority
- keep reference-count transitions monotonic and auditable
- fail closed on underflow or impossible ownership state

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
    sequence
    request_id
    payload_length
    ciphertext
    auth_tag
}
```

The authenticated associated data should include all header fields. Payloads
should not be deserialised until the authentication tag has passed.

Without authenticated framing, pointer validation and worker admission only
protect the ECALL control plane. They do not protect against hostile queue
messages.

