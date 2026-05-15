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

#ifndef CANOPY_SGX_FILE_DEFINED
#define CANOPY_SGX_FILE_DEFINED
typedef struct _canopy_sgx_opaque_FILE FILE;
#endif

// Declarations only — NO enclave implementation. libvpx's vpx_tpl.c (VP9
// two-pass TPL stats, dead in our VP8/single-pass build) references these.
// Providing prototypes lets the file parse; because nothing in the active
// code path calls vpx_write/read_tpl_gop_stats, static-archive linking never
// extracts vpx_tpl.o, so fprintf/fscanf stay unreferenced. An actual call
// would be a link error — the correct outcome for file I/O in an enclave.
#ifdef __cplusplus
extern "C" {
#endif
int fprintf(FILE *, const char *, ...);
int fscanf(FILE *, const char *, ...);
#ifdef __cplusplus
}
#endif

#endif // CANOPY_SGX_POLYFILL_STDIO_H
