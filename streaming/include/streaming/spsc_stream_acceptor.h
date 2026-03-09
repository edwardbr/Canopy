/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <memory>
#include <optional>

#include <coro/coro.hpp>

#include <streaming/stream_acceptor.h>
#include <streaming/spsc_queue_stream.h>

namespace streaming
{
    // Single-use acceptor over a pre-created SPSC queue pair.
    // Returns the stream on the first accept() call; returns nullopt on all subsequent calls.
    // The scheduler is provided at construction time since no socket server is involved.
    class spsc_stream_acceptor : public stream_acceptor
    {
    public:
        spsc_stream_acceptor(spsc_raw_queue* send_q, spsc_raw_queue* recv_q, std::shared_ptr<coro::scheduler> scheduler)
            : send_q_(send_q)
            , recv_q_(recv_q)
            , scheduler_(std::move(scheduler))
        {
        }

        CORO_TASK(std::optional<std::shared_ptr<stream>>) accept() override
        {
            if (accepted_ || stop_)
            {
                CO_RETURN std::nullopt;
            }
            accepted_ = true;
            CO_RETURN std::make_shared<spsc_queue_stream>(send_q_, recv_q_, scheduler_);
        }

        void stop() override { stop_ = true; }

    private:
        spsc_raw_queue* send_q_;
        spsc_raw_queue* recv_q_;
        std::shared_ptr<coro::scheduler> scheduler_;
        bool accepted_ = false;
        bool stop_ = false;
    };

} // namespace streaming
