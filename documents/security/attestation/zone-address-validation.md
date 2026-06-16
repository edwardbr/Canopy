<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Zone Address Validation

## Purpose

`rpc::zone_address` carries an optional `validation_bits` block. The block is
reserved for compact public validation material associated with the address.

The block is not the encrypted RPC payload and should not grow into the full
attestation transcript. Large or evolving metadata belongs in back-channel
entries or protected payloads.

## Current Direction

The validation block may eventually contain a MAC, HMAC, signature, or compact
token proving that a zone address is valid in a particular routing/security
context. The exact mechanism is undecided.

Important constraints:

- the token must be computed over canonical address bytes with
  `validation_bits` cleared;
- the token must include a purpose/version field;
- the token must be scoped to a session, route, audience, or epoch once those
  scopes are specified;
- the token must not contain encryption keys;
- `address_type::local` currently rejects validation bits in the live
  implementation, so using validation bits with local addresses would require a
  deliberate format change, such as a new address type or a version/capability
  extension.

## Token Inputs

A candidate validation token may cover:

```text
canonical zone_address without validation_bits
issuer enclave id
issuer zone id
subject enclave id
subject zone id
audience zone or next hop
session id
attestation epoch
expiry or generation
token purpose
key id
```

The exact field set is open.

## Object Id Reservation

The service-level control object should use the maximum object id representable
by the active `zone_address` object-id bit width, not hard-coded
`UINT64_MAX`.

This matters because `zone_address` supports variable object-id widths. The
implementation must derive the reserved value from
`zone_address::get_object_id_size_bits()`:

```text
object_id_bits == 0  -> no object-id namespace; no reserved service object
object_id_bits == 64 -> UINT64_MAX
0 < object_id_bits < 64 -> (1 << object_id_bits) - 1
```

The reserved object id is intended for service/control interfaces such as
remote attestation. It should not be treated as a normal application object.

## Zone To Enclave Binding

Attestation is initially cached per enclave pair. That does not automatically
prove that an arbitrary `caller_zone_id` belongs to the attested remote
enclave. The attestation service must bind zone ids to enclave identity.

The current assumption is that once enclave identity is established, trust is
shared between all zones maintained by that enclave. This may later be refined
to per-subnet or per-zone authorization.

## Host Allocation Risk

`get_new_zone_id` often involves host/root allocation. A hostile host can
assign misleading route or subnet data unless the enclave binds the allocation
to its attested identity.

The likely model is:

- the host/root suggests route or subnet data;
- the enclave attestation service verifies policy;
- the enclave stamps or records the binding between route identity and enclave
  identity;
- protected messages rely on the binding rather than on host allocation alone.

Whether `get_new_zone_id` itself is protected is still under review.
