// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.
//
// SGX enclave polyfill: <sys/time.h>
//
// An enclave has no OS wall clock and tlibc ships no <sys/time.h>. libvpx's
// vpx_ports/vpx_timer.h needs `struct timeval` and `gettimeofday` purely for
// an *optional performance-instrumentation* timer (vpx_usec_timer_*); the
// codec's correctness never depends on it. There is nothing to chain to
// (#include_next would fail), so this is a standalone, deterministic shim:
// gettimeofday is a header-only `static inline` that zero-fills and returns
// 0 — no OCALL, no link symbol, and reproducible-build friendly (the timer
// simply measures zero elapsed time, which is harmless for instrumentation).

#ifndef CANOPY_SGX_POLYFILL_SYS_TIME_H
#define CANOPY_SGX_POLYFILL_SYS_TIME_H

struct timeval
{
    long tv_sec;
    long tv_usec;
};

#ifdef __cplusplus
extern "C" {
#endif

static inline int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv)
    {
        tv->tv_sec = 0;
        tv->tv_usec = 0;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif // CANOPY_SGX_POLYFILL_SYS_TIME_H
