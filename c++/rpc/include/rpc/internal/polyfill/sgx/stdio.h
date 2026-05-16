// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.
//
// SGX enclave polyfill: <stdio.h>
//
// Intel tlibc ships a restricted <stdio.h> that intentionally omits `FILE`
// (an SGX enclave has no filesystem / no host stdio). Some third-party
// headers nonetheless declare prototypes in terms of `FILE*` that are never
// called or linked inside the enclave — e.g. libvpx's vpx_tpl.h
// (vpx_write/read_tpl_gop_stats, used only for VP9 two-pass TPL stats, dead
// in our VP8 single-pass build). Those translation units only need the
// *type* to exist so the prototype parses.
//
// This header is found ahead of tlibc on the enclave include path; it chains
// to the real tlibc <stdio.h> via #include_next and then supplies an opaque
// FILE if (and only if) tlibc did not. No file I/O is provided or implied —
// any actual call would be a link error, which is the correct outcome.

#ifndef CANOPY_SGX_POLYFILL_STDIO_H
#define CANOPY_SGX_POLYFILL_STDIO_H

#include_next <stdio.h>

// tlibc's <stdio.h> omits FILE (no host stdio in an enclave). libvpx's
// vpx_tpl.h declares prototypes in terms of FILE* and is pulled into BOTH
// the pure-C libvpx build AND C++ enclave TUs (via <vpx/vpx_encoder.h> in
// video_session.h), so the *type* must exist in both languages.
//
// The catch: the SGX libc++ side defines `FILE` as `unsigned long` for the
// C++ TUs where it is in scope (libcxx <cstdio> -> <stdio.h>). A typedef may
// be repeated with the SAME type, but `struct _canopy_sgx_opaque_FILE` vs
// `unsigned long` is a hard redefinition error. So mirror the SGX spelling
// in C++ (identical-typedef-safe even when both definitions are visible) and
// keep the self-documenting opaque struct for C, where we are the only
// definer.
#ifndef CANOPY_SGX_FILE_DEFINED
#define CANOPY_SGX_FILE_DEFINED
#ifdef __cplusplus
typedef unsigned long FILE; // must match the SGX libc++ FILE spelling
#else
typedef struct _canopy_sgx_opaque_FILE FILE;
#endif
#endif

// Declarations only — NO enclave implementation. libvpx's vpx_tpl.c (VP9
// two-pass TPL stats, dead in our VP8/single-pass build) references these.
// Providing prototypes lets the file parse; because nothing in the active
// code path calls vpx_write/read_tpl_gop_stats, static-archive linking never
// extracts vpx_tpl.o, so fprintf/fscanf stay unreferenced. An actual call
// would be a link error — the correct outcome for file I/O in an enclave.
// libvpx is C; only that build references them.
#ifndef __cplusplus
int fprintf(FILE *, const char *, ...);
int fscanf(FILE *, const char *, ...);
#endif

#endif // CANOPY_SGX_POLYFILL_STDIO_H
