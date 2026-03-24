/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <cstdio>

extern "C"
{
    void rpc_log(
        int level,
        const char* str,
        size_t sz)
    {
        if (level >= 3)
        {
            const char* prefix = (level >= 4) ? "[benchmark ERROR] " : "[benchmark WARN] ";
            fputs(prefix, stderr);
            fwrite(str, 1, sz, stderr);
            fputc('\n', stderr);
        }
    }
}
