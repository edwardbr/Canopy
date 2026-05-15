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
        void composite_genie_sprite(vpx_image_t* img);
    }
}
