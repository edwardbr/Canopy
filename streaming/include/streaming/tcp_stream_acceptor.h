/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <chrono>
#include <memory>
#include <optional>

#include <coro/coro.hpp>
#include <coro/net/tcp/server.hpp>

#include <streaming/stream_acceptor.h>
#include <streaming/tcp_stream.h>

namespace streaming
{
    // Accepts incoming TCP connections and produces tcp_stream instances.
    // Endpoint and server options are supplied at construction; the scheduler
    // is injected by the listener via init() before the first accept().
    class tcp_stream_acceptor : public stream_acceptor
    {
    public:
        tcp_stream_acceptor(const coro::net::socket_address& endpoint, coro::net::tcp::server::options opts = {})
            : endpoint_(endpoint)
            , opts_(std::move(opts))
        {
        }

        bool init(std::shared_ptr<coro::scheduler> scheduler) override
        {
            scheduler_ = scheduler;
            server_ = std::make_shared<coro::net::tcp::server>(scheduler, endpoint_, opts_);
            return true;
        }

        CORO_TASK(std::optional<std::shared_ptr<stream>>) accept() override
        {
            while (!stop_)
            {
                auto client = co_await server_->accept(poll_timeout_);
                if (client)
                {
                    CO_RETURN std::make_shared<tcp_stream>(std::move(*client), scheduler_);
                }
                else if (client.error().is_timeout())
                {
                    continue;
                }
                else
                {
                    if (!stop_)
                        RPC_ERROR("tcp_stream_acceptor: accept error");
                    break;
                }
            }
            CO_RETURN std::nullopt;
        }

        void stop() override { stop_ = true; }

    private:
        coro::net::socket_address endpoint_;
        coro::net::tcp::server::options opts_;
        std::shared_ptr<coro::net::tcp::server> server_;
        std::shared_ptr<coro::scheduler> scheduler_;
        bool stop_ = false;
        std::chrono::milliseconds poll_timeout_ = std::chrono::milliseconds(10);
    };

} // namespace streaming
