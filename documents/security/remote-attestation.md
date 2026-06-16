<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# Remote And Local Attestation

This document has moved to the attestation design section:

- [Overview](attestation/overview.md)
- [Attestation Backends](attestation/attestation-backends.md)
- [Route Security Design Patterns](attestation/route-security-design-patterns.md)
- [Protected RPC Envelope](attestation/protected-rpc-envelope.md)
- [Zone Address Validation](attestation/zone-address-validation.md)
- [Back-Channel Context](attestation/back-channel-context.md)
- [Failure Policy](attestation/failure-policy.md)

The attestation design is intentionally split across multiple files because it
now covers direct enclave-to-enclave attestation, routed RPC-level attestation,
end-to-end protected marshaller payloads, public back-channel metadata, route
validation, and fraud handling.
