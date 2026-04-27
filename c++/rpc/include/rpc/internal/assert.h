/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <assert.h>

#if defined(CANOPY_HANG_ON_FAILED_ASSERT)
#  ifdef FOR_SGX
#    include <sgx_error.h>
extern "C"
{
    sgx_status_t hang();
}
#  else
extern "C"
{
    void hang();
}
#  endif

#  define RPC_ASSERT(x)                                                                                                \
      if (!(x))                                                                                                        \
      (hang())
#else
#  ifdef _DEBUG
#    define RPC_ASSERT(x)                                                                                              \
        if (!(x))                                                                                                      \
            assert(!*"error failed " #x);
#  else
#    define RPC_ASSERT(x)                                                                                              \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::abort();                                                                                              \
        }
#  endif
#endif
