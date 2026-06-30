// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <cstdlib>
#include <limits>

#include <zlib.h>

namespace streaming::detail
{
    inline voidpf zlib_alloc(voidpf opaque, uInt items, uInt size) noexcept
    {
        (void)opaque;
        if (size != 0 && items > std::numeric_limits<uInt>::max() / size)
            return Z_NULL;

        return std::calloc(items, size);
    }

    inline void zlib_free(voidpf opaque, voidpf address) noexcept
    {
        (void)opaque;
        std::free(address);
    }

    inline void initialise_zlib_allocator(z_stream& stream) noexcept
    {
        stream.zalloc = zlib_alloc;
        stream.zfree = zlib_free;
        stream.opaque = Z_NULL;
    }
} // namespace streaming::detail
