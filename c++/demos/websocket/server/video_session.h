// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <rpc/rpc.h>

#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>

#include "face_track.h"
#include "websocket_demo/websocket_demo.h"

// Use the build-agnostic rpc::coro::scheduler spelling (libcoro on host, the
// SGX coro runtime in-enclave) provided by <rpc/rpc.h>, as demo_zone does.
// Do NOT forward-declare coro or pull coroutine_support.h here: re-entering
// rpc internals after rpc.h in consumers ODR-diverges the shared_ptr control
// block.

namespace websocket_demo
{
    namespace v1
    {
        // In-enclave video effect bitmask (matches i_calculator::
        // set_video_effects). Browser toggles these live.
        constexpr uint32_t effect_genie = 1u;  // genie sprite overlay
        constexpr uint32_t effect_invert = 2u; // invert luma (photo-negative)

        // Per-connection video processing with a single-slot "latest frame
        // wins" mailbox.
        //
        // The transport delivers every [post] frame in order and losslessly —
        // that contract is untouched. forward_frame() (the [post] handler)
        // only stashes the newest frame and returns immediately, so the
        // transport receive loop is never blocked by codec work. A single
        // detached worker coroutine drains the slot: decode -> transform ->
        // encode -> push back. Frames that arrive while the worker is busy
        // overwrite the slot and are deliberately dropped *by this
        // application*, bounding end-to-end latency to ~one processing cycle.
        //
        // LIFETIME: the worker is detached and can be suspended in
        // co_await sink_->push_frame(...) when the connection tears down (a
        // resolution switch's stop+start, or any disconnect). video_session
        // is a by-value member of demo, which the rpc service frees on
        // teardown. If the worker's state (codecs, sink_) lived directly in
        // video_session it would be freed out from under the suspended
        // worker -> use-after-free deep in the transport/TLS send path. So
        // all worker state lives in `pump`, a shared_ptr-owned object the
        // detached worker holds a strong reference to: the codecs and sink_
        // outlive any in-flight push and are destroyed only once BOTH the
        // session and the worker have let go. No core-transport changes
        // needed; the fix is entirely here.
        //
        // When no scheduler is available, forward_frame falls back to
        // synchronous inline processing — correctness over latency there.
        class video_session
        {
            struct pump : std::enable_shared_from_this<pump>
            {
                rpc::shared_ptr<i_context_event> sink_;
                // Hold the scheduler, not the service: the service owns demo
                // which owns video_session which owns this pump, so a
                // shared_ptr back to the service would form a reference
                // cycle. The scheduler does not own demo.
                std::shared_ptr<rpc::coro::scheduler> scheduler_;

                std::mutex mailbox_mutex_;
                bool has_pending_ = false;
                bool worker_running_ = false;
                bool stopping_ = false;
                uint64_t pending_seq_ = 0;
                uint64_t pending_pts_ = 0;
                uint32_t pending_flags_ = 0;
                std::vector<uint8_t> pending_payload_;

                bool decoder_ready_ = false;
                bool encoder_ready_ = false;
                // Latches if codec init persistently fails (e.g. enclave heap
                // exhausted). Without this, every inbound frame retries init,
                // pegging the CPU and flooding the log.
                bool codec_failed_ = false;
                vpx_codec_ctx_t decoder_{};
                vpx_codec_ctx_t encoder_{};
                vpx_codec_enc_cfg_t encoder_cfg_{};
                int64_t encode_pts_ = 0;
                // Frame size + params generation the encoder was built with;
                // a mismatch (browser resolution switch, or set_video_params)
                // forces a re-init.
                unsigned int enc_w_ = 0;
                unsigned int enc_h_ = 0;
                uint32_t enc_params_gen_ = 0;

                // Effect bitmask, set live from the browser via set_effects().
                // atomic: written on the RPC-dispatch path, read on the
                // worker.
                std::atomic<uint32_t> effects_{effect_genie};

                // Live video params (browser -> set_video_params). atomics:
                // written on RPC dispatch, read on the worker. params_gen_
                // bumps on any change so the worker re-inits the encoder.
                std::atomic<int32_t> brightness_{0};
                std::atomic<uint32_t> bitrate_kbps_{1200};
                std::atomic<uint32_t> cpu_used_{16};
                std::atomic<uint32_t> params_gen_{0};

                // Per-connection skin-region tracker; only touched by the
                // worker.
                face_tracker face_tracker_;

                ~pump();

                bool ensure_decoder();
                bool ensure_encoder(
                    unsigned int width,
                    unsigned int height);
                void transform_frame(vpx_image_t* img);
                static void invert_luma(vpx_image_t* img);
                static void apply_brightness(
                    vpx_image_t* img,
                    int delta);

                // Heavy path: decode -> transform -> encode -> co_await push.
                CORO_TASK(void)
                process_one(
                    uint64_t seq,
                    uint64_t pts_us,
                    uint32_t flags,
                    std::vector<uint8_t> payload);

                // Detached drain loop. Holds a self shared_ptr for its whole
                // lifetime so the pump (codecs, sink_) cannot be freed while
                // it is suspended in push_frame.
                CORO_TASK(void) worker_loop();
            };

            std::shared_ptr<pump> pump_ = std::make_shared<pump>();

        public:
            video_session() = default;
            ~video_session();

            video_session(const video_session&) = delete;
            video_session& operator=(const video_session&) = delete;

            websocket_error set_sink(const rpc::shared_ptr<i_context_event>& sink);
            void set_scheduler(const std::shared_ptr<rpc::coro::scheduler>& scheduler);
            void set_effects(uint32_t effects);
            void set_params(
                int32_t brightness,
                uint32_t bitrate_kbps,
                uint32_t cpu_used);

            CORO_TASK(websocket_error)
            forward_frame(
                uint64_t seq,
                uint64_t pts_us,
                uint32_t flags,
                const std::vector<uint8_t>& payload);
        };
    }
}
