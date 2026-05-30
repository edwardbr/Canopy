/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <streaming/tcp/namespace.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <streaming/tcp_coroutine/acceptor.h>
#else
#  include <streaming/tcp_blocking/acceptor.h>
#endif
