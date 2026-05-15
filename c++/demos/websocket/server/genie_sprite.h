// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <vpx/vpx_image.h>

namespace websocket_demo
{
    namespace v1
    {
        // Phase 3: alpha-composite a fixed-position procedural "genie + lamp"
        // placeholder onto the decoded I420 frame. The sprite is generated in
        // code (no asset file) so the build stays deterministic for the later
        // SGX reproducible-build / MRENCLAVE requirement. Phase 4 replaces the
        // fixed position with a face-anchored one; the artwork can later be
        // swapped for a real sprite without touching callers.
        // Alpha-composite the genie over the I420 frame, scaled (nearest-
        // neighbour) into the rect [ox,oy]..[ox+draw_w, oy+draw_h]. The
        // caller picks the rect: Phase 4b positions/scales it over the
        // tracked face; with no detection it falls back to a fixed corner.
        // The sprite's aspect ratio is the caller's responsibility.
        void composite_genie_sprite(vpx_image_t* img, int ox, int oy, int draw_w, int draw_h);

        // Native sprite dimensions, so the caller can preserve aspect ratio
        // when choosing the draw rect.
        void genie_sprite_native_size(int& w, int& h);
    }
}
