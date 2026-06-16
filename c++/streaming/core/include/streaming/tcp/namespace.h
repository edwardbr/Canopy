/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

namespace streaming::blocking::tcp
{
}

namespace streaming::coroutine::tcp
{
}

namespace streaming
{
#ifdef CANOPY_BUILD_COROUTINE
    namespace tcp = ::streaming::coroutine::tcp;
#else
    namespace tcp = ::streaming::blocking::tcp;
#endif
}
