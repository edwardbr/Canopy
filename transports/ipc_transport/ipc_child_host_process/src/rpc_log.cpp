/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <cstdio>
#include <string>

extern "C"
{
    void rpc_log(int level, const char* str, size_t sz)
    {
        std::string message(str, sz);
        if (level >= 4)
            std::fprintf(stderr, "[libcoro ipc dll ERROR] %s\n", message.c_str());
    }
}
