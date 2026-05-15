// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "video_session.h"

#include "genie_sprite.h"

#include <utility>

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
            {
                std::lock_guard<std::mutex> guard(mailbox_mutex_);
                stopping_ = true;
            }
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

        void video_session::set_scheduler(const std::shared_ptr<rpc::coro::scheduler>& scheduler)
        {
            // The scheduler, never the service — see the header note on the
            // service -> demo -> video_session reference cycle.
            scheduler_ = scheduler;
        }

        void video_session::set_effects(uint32_t effects)
        {
            effects_.store(effects, std::memory_order_relaxed);
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
            // pretty.
            vpx_codec_control(&encoder_, VP8E_SET_CPUUSED, 16);
            vpx_codec_control(&encoder_, VP8E_SET_STATIC_THRESHOLD, 1000);

            encoder_ready_ = true;
            return true;
        }

        // The in-enclave processing seam. Phase 2 proved the codec loop with a
        // luma invert; Phase 3 composites a fixed-position genie sprite over
        // the otherwise-unmodified frame. Phase 4 will anchor the sprite to a
        // detected face; that change stays entirely inside this seam.
        void video_session::invert_luma(vpx_image_t* img)
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

        void video_session::transform_frame(vpx_image_t* img)
        {
            const uint32_t fx = effects_.load(std::memory_order_relaxed);
            // Invert first, then composite the genie on top so the sprite
            // always renders with correct colours regardless of the invert
            // toggle.
            if (fx & effect_invert)
                invert_luma(img);
            if (fx & effect_genie)
                composite_genie_sprite(img);
        }

        CORO_TASK(void)
        video_session::process_one(
            uint64_t seq,
            uint64_t pts_us,
            uint32_t flags,
            std::vector<uint8_t> payload)
        {
            std::ignore = flags;

            if (!sink_ || codec_failed_)
                CO_RETURN;
            if (!ensure_decoder())
            {
                if (!codec_failed_)
                    RPC_WARNING("vp8 decoder init failed - video disabled for this session");
                codec_failed_ = true;
                CO_RETURN;
            }

            if (vpx_codec_decode(&decoder_, payload.data(), static_cast<unsigned int>(payload.size()), nullptr, 0)
                != VPX_CODEC_OK)
            {
                // Likely a delta arriving before a keyframe; skip silently.
                CO_RETURN;
            }

            vpx_codec_iter_t dec_iter = nullptr;
            vpx_image_t* img = vpx_codec_get_frame(&decoder_, &dec_iter);
            if (!img)
                CO_RETURN;

            transform_frame(img);

            if (!ensure_encoder(img->d_w, img->d_h))
            {
                if (!codec_failed_)
                    RPC_WARNING("vp8 encoder init failed - video disabled for this session");
                codec_failed_ = true;
                CO_RETURN;
            }

            vpx_enc_frame_flags_t enc_flags = VPX_EFLAG_FORCE_KF;
            if (vpx_codec_encode(&encoder_, img, encode_pts_++, 1, enc_flags, VPX_DL_REALTIME) != VPX_CODEC_OK)
            {
                RPC_WARNING("vp8 encode failed");
                CO_RETURN;
            }

            std::vector<uint8_t> out_payload;
            uint32_t out_flags = 0;
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

            if (out_payload.empty())
                CO_RETURN;

            co_await sink_->push_frame(seq, pts_us, out_flags, out_payload);
            CO_RETURN;
        }

        CORO_TASK(void)
        video_session::worker_loop()
        {
            for (;;)
            {
                uint64_t seq = 0;
                uint64_t pts = 0;
                uint32_t flags = 0;
                std::vector<uint8_t> payload;
                {
                    std::lock_guard<std::mutex> guard(mailbox_mutex_);
                    if (stopping_ || !has_pending_)
                    {
                        worker_running_ = false;
                        CO_RETURN;
                    }
                    seq = pending_seq_;
                    pts = pending_pts_;
                    flags = pending_flags_;
                    payload = std::move(pending_payload_);
                    pending_payload_.clear();
                    has_pending_ = false;
                }
                co_await process_one(seq, pts, flags, std::move(payload));
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

            // No scheduler (CALCULATOR_ONLY enclave compile-fit has no
            // rpc::service): fall back to synchronous inline processing —
            // correctness over latency there.
            if (!scheduler_)
            {
                co_await process_one(seq, pts_us, flags, payload);
                CO_RETURN rpc::error::OK();
            }

            bool start_worker = false;
            {
                std::lock_guard<std::mutex> guard(mailbox_mutex_);
                if (stopping_)
                    CO_RETURN rpc::error::OK();
                // Overwrite: only the freshest frame survives. Intermediate
                // frames the worker hasn't picked up are deliberately dropped.
                pending_seq_ = seq;
                pending_pts_ = pts_us;
                pending_flags_ = flags;
                pending_payload_ = payload;
                has_pending_ = true;
                if (!worker_running_)
                {
                    worker_running_ = true;
                    start_worker = true;
                }
            }

            if (start_worker)
                scheduler_->spawn_detached(worker_loop());

            // Return immediately: the transport receive loop is never blocked
            // by codec work, so no kernel-socket backlog forms.
            CO_RETURN rpc::error::OK();
        }
    }
}
