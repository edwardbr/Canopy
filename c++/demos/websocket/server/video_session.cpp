// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "video_session.h"

namespace websocket_demo
{
    namespace v1
    {
        int video_session::set_sink(const rpc::shared_ptr<i_context_event>& sink)
        {
            sink_ = sink;
            return rpc::error::OK();
        }

        CORO_TASK(int)
        video_session::forward_frame(
            uint64_t seq,
            uint64_t pts_us,
            uint32_t flags,
            const std::vector<uint8_t>& payload)
        {
            if (!sink_)
            {
                CO_RETURN rpc::error::OK();
            }
            co_await sink_->push_frame(seq, pts_us, flags, payload);
            CO_RETURN rpc::error::OK();
        }
    }
}
