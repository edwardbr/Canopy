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

        // ---- pump: shared-state worker core ---------------------------------

        video_session::pump::~pump()
        {
            // Runs only when BOTH video_session and any detached worker have
            // released their references, so the codecs are never destroyed
            // under an in-flight worker.
            if (decoder_ready_)
                vpx_codec_destroy(&decoder_);
            if (encoder_ready_)
                vpx_codec_destroy(&encoder_);
        }

        bool video_session::pump::ensure_decoder()
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

        bool video_session::pump::ensure_encoder(unsigned int width, unsigned int height)
        {
            const uint32_t gen = params_gen_.load(std::memory_order_relaxed);
            // Reuse the encoder unless the frame size changed (browser
            // resolution switch) or params changed (set_video_params).
            if (encoder_ready_ && enc_w_ == width && enc_h_ == height && enc_params_gen_ == gen)
                return true;
            if (encoder_ready_)
            {
                vpx_codec_destroy(&encoder_);
                encoder_ready_ = false;
            }

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
            encoder_cfg_.rc_target_bitrate = bitrate_kbps_.load(std::memory_order_relaxed);

            if (vpx_codec_enc_init(&encoder_, vpx_codec_vp8_cx(), &encoder_cfg_, 0) != VPX_CODEC_OK)
                return false;

            // cpu_used is the dominant realtime speed/quality lever for VP8.
            // Range [-16, 16]; higher = faster encode, lower quality.
            vpx_codec_control(
                &encoder_, VP8E_SET_CPUUSED, static_cast<int>(cpu_used_.load(std::memory_order_relaxed)));
            vpx_codec_control(&encoder_, VP8E_SET_STATIC_THRESHOLD, 1000);

            enc_w_ = width;
            enc_h_ = height;
            enc_params_gen_ = gen;
            encoder_ready_ = true;
            return true;
        }

        void video_session::pump::invert_luma(vpx_image_t* img)
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

        void video_session::pump::apply_brightness(vpx_image_t* img, int delta)
        {
            if (delta == 0)
                return;
            unsigned char* y = img->planes[VPX_PLANE_Y];
            const int stride = img->stride[VPX_PLANE_Y];
            const unsigned int w = img->d_w;
            const unsigned int h = img->d_h;
            for (unsigned int row = 0; row < h; ++row)
            {
                unsigned char* line = y + static_cast<size_t>(row) * stride;
                for (unsigned int col = 0; col < w; ++col)
                {
                    const int v = line[col] + delta;
                    line[col] = static_cast<unsigned char>(v < 0 ? 0 : (v > 255 ? 255 : v));
                }
            }
        }

        // The in-enclave processing seam: brightness -> invert -> face-anchored
        // genie composite. All effects are toggled live from the browser.
        void video_session::pump::transform_frame(vpx_image_t* img)
        {
            const uint32_t fx = effects_.load(std::memory_order_relaxed);
            apply_brightness(img, brightness_.load(std::memory_order_relaxed));
            if (fx & effect_invert)
                invert_luma(img);
            if (!(fx & effect_genie))
                return;

            const int fw = static_cast<int>(img->d_w);
            const int fh = static_cast<int>(img->d_h);
            int nat_w = 0;
            int nat_h = 0;
            genie_sprite_native_size(nat_w, nat_h);

            const face_box fb = face_tracker_.track(img);
            int draw_h = 0;
            int draw_w = 0;
            int ox = 0;
            int oy = 0;
            if (fb.valid)
            {
                // Crown scales with the head: a bit wider than the detected
                // skin region, aspect preserved.
                draw_w = fb.w * 6 / 5; // ~1.2x head width
                if (draw_w < nat_w / 2)
                    draw_w = nat_w / 2;
                if (draw_w > fw)
                    draw_w = fw;
                draw_h = draw_w * nat_h / nat_w;
                // Centre on the head; rest the band on top of the head with a
                // slight overlap so it sits on the head rather than floating.
                ox = fb.cx - draw_w / 2;
                oy = (fb.cy - fb.h / 2) - draw_h * 85 / 100;
            }
            else
            {
                // No skin detected: fixed top-centre at native size so the
                // crown is still visible.
                draw_w = nat_w;
                draw_h = nat_h;
                ox = (fw - nat_w) / 2;
                oy = 10;
            }
            if (ox < 0)
                ox = 0;
            if (oy < 0)
                oy = 0;
            composite_genie_sprite(img, ox, oy, draw_w, draw_h);
        }

        CORO_TASK(void)
        video_session::pump::process_one(
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
        video_session::pump::worker_loop()
        {
            // Strong self-ref for the worker's entire lifetime: even if
            // video_session (and demo) are destroyed mid-flush, the pump —
            // and therefore sink_ and the codecs — stays alive until this
            // coroutine returns, so the suspended push_frame can't UAF.
            auto self = shared_from_this();
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

        // ---- video_session: thin facade over the shared pump ----------------

        video_session::~video_session()
        {
            // Signal the worker to stop; the pump (and its codecs/sink_) is
            // freed by ~pump only once the worker has also released it.
            std::lock_guard<std::mutex> guard(pump_->mailbox_mutex_);
            pump_->stopping_ = true;
        }

        int video_session::set_sink(const rpc::shared_ptr<i_context_event>& sink)
        {
            pump_->sink_ = sink;
            return rpc::error::OK();
        }

        void video_session::set_scheduler(const std::shared_ptr<rpc::coro::scheduler>& scheduler)
        {
            pump_->scheduler_ = scheduler;
        }

        void video_session::set_effects(uint32_t effects)
        {
            pump_->effects_.store(effects, std::memory_order_relaxed);
        }

        void video_session::set_params(int32_t brightness, uint32_t bitrate_kbps, uint32_t cpu_used)
        {
            if (brightness < -128)
                brightness = -128;
            if (brightness > 127)
                brightness = 127;
            if (bitrate_kbps < 50)
                bitrate_kbps = 50;
            if (bitrate_kbps > 20000)
                bitrate_kbps = 20000;
            if (cpu_used > 16)
                cpu_used = 16;
            pump_->brightness_.store(brightness, std::memory_order_relaxed);
            pump_->bitrate_kbps_.store(bitrate_kbps, std::memory_order_relaxed);
            pump_->cpu_used_.store(cpu_used, std::memory_order_relaxed);
            // Bump so the worker re-inits the encoder with the new rate/speed.
            pump_->params_gen_.fetch_add(1, std::memory_order_relaxed);
        }

        CORO_TASK(int)
        video_session::forward_frame(
            uint64_t seq,
            uint64_t pts_us,
            uint32_t flags,
            const std::vector<uint8_t>& payload)
        {
            auto p = pump_;
            if (!p->sink_)
                CO_RETURN rpc::error::OK();

            // No scheduler (CALCULATOR_ONLY enclave compile-fit has no
            // rpc::service): fall back to synchronous inline processing —
            // correctness over latency there.
            if (!p->scheduler_)
            {
                co_await p->process_one(seq, pts_us, flags, payload);
                CO_RETURN rpc::error::OK();
            }

            bool start_worker = false;
            {
                std::lock_guard<std::mutex> guard(p->mailbox_mutex_);
                if (p->stopping_)
                    CO_RETURN rpc::error::OK();
                // Overwrite: only the freshest frame survives. Intermediate
                // frames the worker hasn't picked up are deliberately dropped.
                p->pending_seq_ = seq;
                p->pending_pts_ = pts_us;
                p->pending_flags_ = flags;
                p->pending_payload_ = payload;
                p->has_pending_ = true;
                if (!p->worker_running_)
                {
                    p->worker_running_ = true;
                    start_worker = true;
                }
            }

            if (start_worker)
                p->scheduler_->spawn_detached(p->worker_loop());

            // Return immediately: the transport receive loop is never blocked
            // by codec work, so no kernel-socket backlog forms.
            CO_RETURN rpc::error::OK();
        }
    }
}
