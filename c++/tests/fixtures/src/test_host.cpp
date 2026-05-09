/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "test_host.h"
#include "common/foo_impl.h"
#include <cstdint>
#if defined(CANOPY_BUILD_ENCLAVE) && !defined(CANOPY_BUILD_COROUTINE)
#  include <transports/sgx/transport.h>
#elif defined(CANOPY_BUILD_ENCLAVE) && defined(CANOPY_BUILD_COROUTINE)
#  include <transports/sgx_coroutine/host/transport.h>
#endif
