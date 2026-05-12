<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# SGX Design Notes

This folder collects SGX-specific design notes that do not belong solely to one
transport implementation.

- [Connectivity](connectivity/README.md)

## Enclave TLS

Canopy has two secure stream backends. The default direction is the bundled
Mbed TLS backend selected by `CANOPY_SECURE_STREAM_BACKEND=MBEDTLS`. The older
OpenSSL-compatible backend remains available as
`CANOPY_SECURE_STREAM_BACKEND=OPENSSL`; inside Intel SGX that backend depends on
SGXSSL rather than the host OpenSSL libraries.

The OpenSSL/SGXSSL backend uses the TLS-enabled SGXSSL layout produced by
`submodules/confidential-computing.sgx/external/sgxssl/prepare_sgxssl.sh`:

- `libsgx_tsgxssl*.a`
- `libsgx_tsgxssl_ssl*.a`
- `libsgx_tsgxssl_crypto*.a`
- SGX SDK trusted support libraries such as `libsgx_ttls.a`,
  `libsgx_tcrypto.a`, and `libsgx_dcap_tvl.a`

For Intel SGX builds, `CANOPY_BOOTSTRAP_SGXSSL=ON` lets CMake prepare SGXSSL
from `submodules/confidential-computing.sgx/external/sgxssl`. CMake treats that
directory as a recipe source only: it copies `prepare_sgxssl.sh` into the active
build directory and runs it there. The preparation script downloads
checksum-verified Intel SGXSSL and OpenSSL source archives on the first build,
then compiles the trusted static libraries locally. Set
`CANOPY_BOOTSTRAP_SGXSSL=OFF` and point
`CANOPY_SGXSSL_LIB_DIR`/`CANOPY_SGXSSL_INCLUDE_DIR` at an existing SGXSSL
installation when network bootstrap is not acceptable.

The attestation certificate APIs shown in Intel's sample are hardware SGX-FLC
only. Simulation builds are useful for compiling and exercising plain TLS, but
not for validating the full attested-TLS quote flow.

Server peer-certificate verification is selected when constructing the secure
stream context, not by the websocket server itself. Browser-facing websocket
listeners normally use `peer_verification::none`; peer-to-peer and future RA-TLS
listeners should use `peer_verification::required` with an explicit
`trust_anchor`. `peer_verification::optional` accepts clients without a
certificate but rejects clients that present an untrusted certificate.
