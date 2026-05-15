// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.
//
// SGX enclave polyfill: <stdlib.h>
//
// tlibc provides stdlib.h (malloc/atoi/strtol/...) but only declares
// `getenv` as a deprecated stub — an SGX enclave has no process
// environment. Third-party code nonetheless probes the environment for
// optional/debug overrides (e.g. libvpx's vpx_encoder.c reading
// VPX_SIMD_CAPS_MASK). The correct enclave behaviour is "the variable is
// unset": getenv returns NULL and the library uses its compiled-in default
// (here: the pinned SSE2 SIMD path).
//
// This header is found ahead of tlibc on the enclave include path; it chains
// to the real tlibc <stdlib.h> via #include_next for everything it provides,
// then redirects all `getenv` callers to a benign NULL-returning inline.
// Header-only (no link symbol), deterministic, no OCALL.

#ifndef CANOPY_SGX_POLYFILL_STDLIB_H
#define CANOPY_SGX_POLYFILL_STDLIB_H

#include_next <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline char* canopy_sgx_getenv(const char* name)
{
    (void)name;
    return 0;
}

#ifdef __cplusplus
}
#endif

#ifdef getenv
#  undef getenv
#endif
#define getenv canopy_sgx_getenv

#endif // CANOPY_SGX_POLYFILL_STDLIB_H
