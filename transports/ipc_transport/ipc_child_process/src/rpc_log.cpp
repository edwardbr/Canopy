/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <cstdio>
#include <string>

#ifdef CANOPY_BUILD_COROUTINE

extern "C"
{
    void rpc_log(int level, const char* str, size_t sz)
    {
        std::string message(str, sz);
        if (level >= 4)
            std::fprintf(stderr, "[ipc child process ERROR] %s\n", message.c_str());
    }
}

#endif // CANOPY_BUILD_COROUTINE
