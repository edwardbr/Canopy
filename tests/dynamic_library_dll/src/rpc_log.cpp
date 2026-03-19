/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Minimal rpc_log implementation for the test DLL.
// The DLL has its own statically linked copy of librpc.a which requires
// a user-provided rpc_log.  We route to stderr so DLL-side log messages
// are visible during test runs.

#include <cstdio>
#include <cstring>

extern "C"
{
    void rpc_log(int level, const char* str, size_t sz)
    {
        // Only print warnings and above to avoid flooding test output
        if (level >= 3)
        {
            const char* prefix = (level >= 4) ? "[dll ERROR] " : "[dll WARN] ";
            fputs(prefix, stderr);
            fwrite(str, 1, sz, stderr);
            fputc('\n', stderr);
        }
    }
}
