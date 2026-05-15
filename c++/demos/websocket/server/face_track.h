// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <vpx/vpx_image.h>

namespace websocket_demo
{
    namespace v1
    {
        struct face_box
        {
            int cx = 0; // centre, full-frame luma pixels
            int cy = 0;
            int w = 0; // detected skin-region extent
            int h = 0;
            bool valid = false; // false => too little skin this frame
        };

        // Phase 4b "face" tracking without ML: skin pixels form a tight,
        // well-known cluster in YCbCr chroma. We scan the I420 U/V planes for
        // that cluster, take the bounding box of the dominant skin region,
        // and exponentially smooth it across frames so the genie tracks the
        // person steadily. Pure integer math on planes we already touch — no
        // model, no new dependency, no extra enclave polyfill. It tracks the
        // dominant skin blob (face+neck/hands), not strictly a face; good
        // enough for the demo, ggml is the documented upgrade path.
        class face_tracker
        {
            float ema_cx_ = 0.f;
            float ema_cy_ = 0.f;
            float ema_w_ = 0.f;
            float ema_h_ = 0.f;
            bool initialised_ = false;

        public:
            face_box track(const vpx_image_t* img);
        };
    }
}
