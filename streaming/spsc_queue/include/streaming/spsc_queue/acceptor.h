/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <memory>
#include <optional>

#include <coro/coro.hpp>

#include <streaming/spsc_queue/stream.h>
#include <streaming/stream_acceptor.h>

namespace streaming::spsc_queue
{
    class acceptor : public ::streaming::stream_acceptor
    {
    public:
        acceptor(
            queue_type* send_q,
            queue_type* recv_q,
            std::shared_ptr<coro::scheduler> scheduler)
            : send_q_(send_q)
            , recv_q_(recv_q)
            , scheduler_(std::move(scheduler))
        {
        }

        CORO_TASK(std::optional<std::shared_ptr<::streaming::stream>>) accept() override
        {
            if (accepted_ || stop_)
            {
                CO_RETURN std::nullopt;
            }
            accepted_ = true;
            CO_RETURN std::make_shared<stream>(send_q_, recv_q_, scheduler_);
        }

        void stop() override { stop_ = true; }

    private:
        queue_type* send_q_;
        queue_type* recv_q_;
        std::shared_ptr<coro::scheduler> scheduler_;
        bool accepted_ = false;
        bool stop_ = false;
    };
} // namespace streaming::spsc_queue
