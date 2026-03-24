/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Minimal rpc_log for the libcoro test DLL.
// RTLD_LOCAL hides the host's rpc_log, so the DLL must supply its own.

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
            const char* prefix = (level >= 4) ? "[libcoro dll ERROR] " : "[libcoro dll WARN] ";
            fputs(prefix, stderr);
            fwrite(str, 1, sz, stderr);
            fputc('\n', stderr);
        }
    }
}
