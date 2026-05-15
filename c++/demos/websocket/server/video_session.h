// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <cstdint>
#include <vector>

#include <rpc/rpc.h>

#include "websocket_demo/websocket_demo.h"

namespace websocket_demo
{
    namespace v1
    {
        // Per-connection video echo state. Owns a reference to the
        // browser-facing i_context_event sink (shared with the LLM chat
        // path) and uses its push_frame method to bounce inbound frames
        // straight back. Kept separate from the calculator/chat code so
        // future work (decode, face detection, sprite compositing, encode)
        // lives entirely here.
        class video_session
        {
            rpc::shared_ptr<i_context_event> sink_;

        public:
            int set_sink(const rpc::shared_ptr<i_context_event>& sink);

            CORO_TASK(int)
            forward_frame(
                uint64_t seq,
                uint64_t pts_us,
                uint32_t flags,
                const std::vector<uint8_t>& payload);
        };
    }
}
