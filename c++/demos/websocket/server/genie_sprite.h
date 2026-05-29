// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <vpx/vpx_image.h>

namespace websocket_demo
{
    namespace v1
    {
        // Alpha-composite a procedural crown onto the decoded I420 frame. The
        // sprite is generated in code (no asset file) so the build stays
        // deterministic for the SGX reproducible-build / MRENCLAVE
        // requirement; the artwork can later be swapped for a real sprite
        // without touching callers.
        // Scaled (nearest-neighbour) into the rect [ox,oy]..[ox+draw_w,
        // oy+draw_h]. The caller picks the rect: it positions/scales the
        // crown over the tracked head; with no detection it falls back to a
        // fixed position. The sprite's aspect ratio is the caller's
        // responsibility.
        void composite_genie_sprite(
            vpx_image_t* img,
            int ox,
            int oy,
            int draw_w,
            int draw_h);

        // Native sprite dimensions, so the caller can preserve aspect ratio
        // when choosing the draw rect.
        void genie_sprite_native_size(
            int& w,
            int& h);
    }
}
