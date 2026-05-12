// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

/*
 * llhttp keeps a debug logging hook in its C API implementation. SGX enclave
 * builds do not provide stdio, and this path is not used by Canopy, so compile
 * it out when building the vendored parser for an enclave.
 */
#if defined(FOR_SGX) && !defined(CANOPY_FAKE_SGX)
/*
 * Include stdio before defining the local stubs so the macros do not rewrite
 * stdio's own declarations when the enclave SDK provides them.
 */
#  if defined(__has_include)
#    if __has_include(<stdio.h>)
#      include <stdio.h>
#    endif
#  else
#    include <stdio.h>
#  endif
#  ifndef fprintf
#    define fprintf(...) ((int)0)
#  endif
#  ifndef stderr
#    define stderr ((FILE*)0)
#  endif
#endif
