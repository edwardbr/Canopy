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

#include "websocket_demo/websocket_demo.h"

namespace coro
{
    class scheduler;
}

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
        // This frame-skip is a policy for this video stream only; it does not
        // and must not generalise into the transport.
        //
        // When no scheduler is available (the CALCULATOR_ONLY enclave
        // compile-fit build has no rpc::service), forward_frame falls back to
        // synchronous inline processing — correctness over latency there.
        class video_session
        {
            rpc::shared_ptr<i_context_event> sink_;
            // Hold the scheduler, not the service: the service owns demo which
            // owns this video_session, so a shared_ptr back to the service
            // would form a reference cycle. The scheduler does not own demo,
            // so a shared_ptr to it is cycle-free and outlives the connection.
            std::shared_ptr<coro::scheduler> scheduler_;

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
            vpx_codec_ctx_t decoder_{};
            vpx_codec_ctx_t encoder_{};
            vpx_codec_enc_cfg_t encoder_cfg_{};
            int64_t encode_pts_ = 0;

            // Effect bitmask, set live from the browser via set_effects().
            // atomic: written on the RPC-dispatch path, read on the worker.
            std::atomic<uint32_t> effects_{effect_genie};

            bool ensure_decoder();
            bool ensure_encoder(unsigned int width, unsigned int height);
            void transform_frame(vpx_image_t* img);
            static void invert_luma(vpx_image_t* img);

            // Heavy path: decode -> transform -> encode -> co_await push.
            CORO_TASK(void)
            process_one(uint64_t seq, uint64_t pts_us, uint32_t flags, std::vector<uint8_t> payload);

            // Detached drain loop; processes only the most recent slot value.
            CORO_TASK(void) worker_loop();

        public:
            video_session() = default;
            ~video_session();

            video_session(const video_session&) = delete;
            video_session& operator=(const video_session&) = delete;

            int set_sink(const rpc::shared_ptr<i_context_event>& sink);
            void set_scheduler(const std::shared_ptr<coro::scheduler>& scheduler);
            void set_effects(uint32_t effects);

            CORO_TASK(int)
            forward_frame(
                uint64_t seq,
                uint64_t pts_us,
                uint32_t flags,
                const std::vector<uint8_t>& payload);
        };
    }
}
