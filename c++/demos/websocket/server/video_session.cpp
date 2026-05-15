// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "video_session.h"

#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>

namespace websocket_demo
{
    namespace v1
    {
        namespace
        {
            constexpr uint32_t VIDEO_FLAG_KEYFRAME = 1u;
        }

        video_session::~video_session()
        {
            if (decoder_ready_)
                vpx_codec_destroy(&decoder_);
            if (encoder_ready_)
                vpx_codec_destroy(&encoder_);
        }

        int video_session::set_sink(const rpc::shared_ptr<i_context_event>& sink)
        {
            sink_ = sink;
            return rpc::error::OK();
        }

        bool video_session::ensure_decoder()
        {
            if (decoder_ready_)
                return true;
            vpx_codec_dec_cfg_t cfg{};
            cfg.threads = 1;
            if (vpx_codec_dec_init(&decoder_, vpx_codec_vp8_dx(), &cfg, 0) != VPX_CODEC_OK)
                return false;
            decoder_ready_ = true;
            return true;
        }

        bool video_session::ensure_encoder(unsigned int width, unsigned int height)
        {
            if (encoder_ready_)
                return true;
            if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &encoder_cfg_, 0) != VPX_CODEC_OK)
                return false;

            encoder_cfg_.g_w = width;
            encoder_cfg_.g_h = height;
            encoder_cfg_.g_timebase.num = 1;
            encoder_cfg_.g_timebase.den = 30;
            encoder_cfg_.g_error_resilient = 1;
            encoder_cfg_.g_pass = VPX_RC_ONE_PASS;
            encoder_cfg_.g_lag_in_frames = 0; // realtime, no lookahead
            encoder_cfg_.rc_end_usage = VPX_CBR;
            // Every frame is a keyframe: with [post] there is no delivery
            // guarantee, and VP8 delta frames reference the previous frame, so
            // a single dropped/reordered frame corrupts everything until the
            // next keyframe. All-intra makes each frame independent — a lost
            // frame costs exactly one frame, no block-artifact cascade.
            encoder_cfg_.kf_mode = VPX_KF_AUTO;
            encoder_cfg_.kf_max_dist = 1;
            encoder_cfg_.rc_target_bitrate = 1200; // kbps; smaller = faster encode + push

            if (vpx_codec_enc_init(&encoder_, vpx_codec_vp8_cx(), &encoder_cfg_, 0) != VPX_CODEC_OK)
                return false;

            // cpu_used is the dominant realtime speed/quality lever for VP8.
            // Range [-16, 16]; higher = much faster encode, lower quality.
            // Max it: the demo needs to keep up with the camera, not look
            // pretty. Without this the synchronous per-frame encode falls
            // behind 15 fps and latency grows unbounded.
            vpx_codec_control(&encoder_, VP8E_SET_CPUUSED, 16);
            vpx_codec_control(&encoder_, VP8E_SET_STATIC_THRESHOLD, 1000);

            frame_width_ = width;
            frame_height_ = height;
            encoder_ready_ = true;
            return true;
        }

        // Phase 2 transform: invert the luma plane. Produces an unmistakable
        // photo-negative so the round-trip through the enclave codec is
        // visually obvious. Later phases replace this with sprite compositing
        // and face-anchored genie rendering on the same decoded frame.
        void video_session::transform_frame(vpx_image_t* img)
        {
            unsigned char* y = img->planes[VPX_PLANE_Y];
            const int stride = img->stride[VPX_PLANE_Y];
            const unsigned int w = img->d_w;
            const unsigned int h = img->d_h;
            for (unsigned int row = 0; row < h; ++row)
            {
                unsigned char* line = y + static_cast<size_t>(row) * stride;
                for (unsigned int col = 0; col < w; ++col)
                    line[col] = static_cast<unsigned char>(255 - line[col]);
            }
        }

        CORO_TASK(int)
        video_session::forward_frame(
            uint64_t seq,
            uint64_t pts_us,
            uint32_t flags,
            const std::vector<uint8_t>& payload)
        {
            if (!sink_)
                CO_RETURN rpc::error::OK();

            std::vector<uint8_t> out_payload;
            uint32_t out_flags = 0;

            {
                std::lock_guard<std::mutex> guard(codec_mutex_);

                if (!ensure_decoder())
                {
                    RPC_WARNING("vp8 decoder init failed");
                    CO_RETURN rpc::error::OK();
                }

                if (vpx_codec_decode(&decoder_, payload.data(), static_cast<unsigned int>(payload.size()), nullptr, 0)
                    != VPX_CODEC_OK)
                {
                    // Likely a delta arriving before a keyframe; skip silently.
                    CO_RETURN rpc::error::OK();
                }

                vpx_codec_iter_t dec_iter = nullptr;
                vpx_image_t* img = vpx_codec_get_frame(&decoder_, &dec_iter);
                if (!img)
                    CO_RETURN rpc::error::OK();

                transform_frame(img);

                if (!ensure_encoder(img->d_w, img->d_h))
                {
                    RPC_WARNING("vp8 encoder init failed");
                    CO_RETURN rpc::error::OK();
                }

                vpx_enc_frame_flags_t enc_flags = VPX_EFLAG_FORCE_KF;
                if (vpx_codec_encode(&encoder_, img, encode_pts_++, 1, enc_flags, VPX_DL_REALTIME)
                    != VPX_CODEC_OK)
                {
                    RPC_WARNING("vp8 encode failed");
                    CO_RETURN rpc::error::OK();
                }

                vpx_codec_iter_t enc_iter = nullptr;
                const vpx_codec_cx_pkt_t* pkt = nullptr;
                while ((pkt = vpx_codec_get_cx_data(&encoder_, &enc_iter)) != nullptr)
                {
                    if (pkt->kind != VPX_CODEC_CX_FRAME_PKT)
                        continue;
                    const auto* buf = static_cast<const uint8_t*>(pkt->data.frame.buf);
                    out_payload.assign(buf, buf + pkt->data.frame.sz);
                    if (pkt->data.frame.flags & VPX_FRAME_IS_KEY)
                        out_flags |= VIDEO_FLAG_KEYFRAME;
                    break; // one encoded frame per input frame in realtime mode
                }
            }

            std::ignore = flags;
            if (out_payload.empty())
                CO_RETURN rpc::error::OK();

            co_await sink_->push_frame(seq, pts_us, out_flags, out_payload);
            CO_RETURN rpc::error::OK();
        }
    }
}
