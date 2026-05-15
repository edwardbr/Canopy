// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.
//
// SGX enclave polyfill: <inttypes.h>
//
// Intel tlibc ships <inttypes.h> with the C99 PRI* (printf) format macros but
// omits the SCN* (scanf) set. That is a genuine libc gap, not specific to any
// one consumer (libvpx's vpx_tpl.c is just the first to hit it). This header
// is found ahead of tlibc on the enclave include path, chains to tlibc via
// #include_next for everything it does provide, then fills in the standard
// SCN* macros for the LP64 model the SGX SDK targets (x86_64: int64_t = long,
// intptr_t = long, intmax_t via "j"). Each macro is guarded so a future SDK
// that adds them takes precedence with no conflict.

#ifndef CANOPY_SGX_POLYFILL_INTTYPES_H
#define CANOPY_SGX_POLYFILL_INTTYPES_H

#include_next <inttypes.h>

// scanf length modifiers (LP64): 8 -> "hh", 16 -> "h", 32 -> "", 64 -> "l".
#ifndef SCNd8
#  define SCNd8 "hhd"
#endif
#ifndef SCNi8
#  define SCNi8 "hhi"
#endif
#ifndef SCNu8
#  define SCNu8 "hhu"
#endif
#ifndef SCNo8
#  define SCNo8 "hho"
#endif
#ifndef SCNx8
#  define SCNx8 "hhx"
#endif

#ifndef SCNd16
#  define SCNd16 "hd"
#endif
#ifndef SCNi16
#  define SCNi16 "hi"
#endif
#ifndef SCNu16
#  define SCNu16 "hu"
#endif
#ifndef SCNo16
#  define SCNo16 "ho"
#endif
#ifndef SCNx16
#  define SCNx16 "hx"
#endif

#ifndef SCNd32
#  define SCNd32 "d"
#endif
#ifndef SCNi32
#  define SCNi32 "i"
#endif
#ifndef SCNu32
#  define SCNu32 "u"
#endif
#ifndef SCNo32
#  define SCNo32 "o"
#endif
#ifndef SCNx32
#  define SCNx32 "x"
#endif

#ifndef SCNd64
#  define SCNd64 "ld"
#endif
#ifndef SCNi64
#  define SCNi64 "li"
#endif
#ifndef SCNu64
#  define SCNu64 "lu"
#endif
#ifndef SCNo64
#  define SCNo64 "lo"
#endif
#ifndef SCNx64
#  define SCNx64 "lx"
#endif

// LEAST/FAST exact-width aliases (same underlying types on LP64).
#ifndef SCNdLEAST8
#  define SCNdLEAST8 SCNd8
#  define SCNiLEAST8 SCNi8
#  define SCNuLEAST8 SCNu8
#  define SCNoLEAST8 SCNo8
#  define SCNxLEAST8 SCNx8
#  define SCNdLEAST16 SCNd16
#  define SCNiLEAST16 SCNi16
#  define SCNuLEAST16 SCNu16
#  define SCNoLEAST16 SCNo16
#  define SCNxLEAST16 SCNx16
#  define SCNdLEAST32 SCNd32
#  define SCNiLEAST32 SCNi32
#  define SCNuLEAST32 SCNu32
#  define SCNoLEAST32 SCNo32
#  define SCNxLEAST32 SCNx32
#  define SCNdLEAST64 SCNd64
#  define SCNiLEAST64 SCNi64
#  define SCNuLEAST64 SCNu64
#  define SCNoLEAST64 SCNo64
#  define SCNxLEAST64 SCNx64
#endif
#ifndef SCNdFAST8
#  define SCNdFAST8 SCNd8
#  define SCNiFAST8 SCNi8
#  define SCNuFAST8 SCNu8
#  define SCNoFAST8 SCNo8
#  define SCNxFAST8 SCNx8
#  define SCNdFAST16 SCNd64
#  define SCNiFAST16 SCNi64
#  define SCNuFAST16 SCNu64
#  define SCNoFAST16 SCNo64
#  define SCNxFAST16 SCNx64
#  define SCNdFAST32 SCNd64
#  define SCNiFAST32 SCNi64
#  define SCNuFAST32 SCNu64
#  define SCNoFAST32 SCNo64
#  define SCNxFAST32 SCNx64
#  define SCNdFAST64 SCNd64
#  define SCNiFAST64 SCNi64
#  define SCNuFAST64 SCNu64
#  define SCNoFAST64 SCNo64
#  define SCNxFAST64 SCNx64
#endif

// intmax_t / intptr_t (LP64: intptr_t = long -> "l").
#ifndef SCNdMAX
#  define SCNdMAX "jd"
#  define SCNiMAX "ji"
#  define SCNuMAX "ju"
#  define SCNoMAX "jo"
#  define SCNxMAX "jx"
#endif
#ifndef SCNdPTR
#  define SCNdPTR "ld"
#  define SCNiPTR "li"
#  define SCNuPTR "lu"
#  define SCNoPTR "lo"
#  define SCNxPTR "lx"
#endif

#endif // CANOPY_SGX_POLYFILL_INTTYPES_H
