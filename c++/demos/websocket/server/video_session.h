// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include <rpc/rpc.h>

#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>

#include "websocket_demo/websocket_demo.h"

namespace websocket_demo
{
    namespace v1
    {
        // Per-connection video processing. Phase 2: decode the inbound VP8
        // chunk, apply a visible in-enclave transform (luma invert), re-encode
        // to VP8, and push it back out via the i_context_event sink. The
        // codec contexts are lazily initialised on the first decodable frame
        // (the encoder needs the stream dimensions, which come from the
        // decoder). Future phases (sprite overlay, face detection, genie
        // animation) operate on the decoded raw frame in transform_frame().
        class video_session
        {
            rpc::shared_ptr<i_context_event> sink_;

            std::mutex codec_mutex_;
            bool decoder_ready_ = false;
            bool encoder_ready_ = false;
            vpx_codec_ctx_t decoder_{};
            vpx_codec_ctx_t encoder_{};
            vpx_codec_enc_cfg_t encoder_cfg_{};
            unsigned int frame_width_ = 0;
            unsigned int frame_height_ = 0;
            int64_t encode_pts_ = 0;

            bool ensure_decoder();
            bool ensure_encoder(unsigned int width, unsigned int height);
            static void transform_frame(vpx_image_t* img);

        public:
            video_session() = default;
            ~video_session();

            video_session(const video_session&) = delete;
            video_session& operator=(const video_session&) = delete;

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
