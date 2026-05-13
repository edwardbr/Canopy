<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# SGX Design Notes

This folder collects SGX-specific design notes that do not belong solely to one
transport implementation.

- [Connectivity](connectivity/README.md)

## Enclave TLS

Canopy has two secure stream backends. The current compatibility default remains
`CANOPY_SECURE_STREAM_BACKEND=OPENSSL`; inside Intel SGX that backend depends on
SGXSSL rather than the host OpenSSL libraries. The bundled Mbed TLS backend is
available as an opt-in backend with `CANOPY_BUILD_MBEDTLS=ON` or by selecting
`CANOPY_SECURE_STREAM_BACKEND=MBEDTLS`. This keeps existing presets stable while
the Mbed TLS stream is proven in real SGX, fake SGX, and host-only websocket
demos.

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

## Enclave Coroutine Boundary

Host coroutine builds still use upstream libcoro. Enclave builds should not pull
the full libcoro header surface or its host-oriented dependencies into trusted
code. The enclave include path therefore resolves the small `coro::*` subset
needed by Canopy through RPC-owned SGX polyfill headers, currently including
`task`, `event`, `expected`, `mutex`, `net::io_status`, and the minimal
`concepts::awaitable` traits used by `sync_wait`.

`libcoro_enclave` is kept as a compatibility CMake target, but it must not expose
`c++/submodules/libcoro/include` to enclave targets. New enclave code should use
`rpc::coro::*` aliases where possible and treat direct `coro::*` names as public
compatibility shims for existing streaming APIs.
