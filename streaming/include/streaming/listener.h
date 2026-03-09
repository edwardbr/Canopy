/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <functional>
#include <memory>

#include <coro/coro.hpp>
#include <rpc/rpc.h>

#include <streaming/stream_acceptor.h>

namespace streaming
{
    // Transport-agnostic accept loop.
    //
    // Calls the on_new_connection callable for each stream produced by the acceptor.
    // The callable is responsible for building any stream stack (TLS, WS, etc.) and
    // creating the transport — the listener has no knowledge of either.
    class listener
    {
    public:
        using on_new_connection = std::function<CORO_TASK(void)(std::shared_ptr<stream>)>;

        listener(std::shared_ptr<stream_acceptor> acceptor, on_new_connection handler)
            : acceptor_(std::move(acceptor))
            , on_new_connection_(std::move(handler))
        {
        }

        listener(const listener&) = delete;
        listener& operator=(const listener&) = delete;

        bool start_listening(std::shared_ptr<rpc::service> service)
        {
            service_ = service;
            if (!acceptor_->init(service->get_scheduler()))
            {
                RPC_ERROR("listener: acceptor init failed");
                return false;
            }
            return service->spawn(run_listener(service));
        }

        CORO_TASK(void) stop_listening()
        {
            acceptor_->stop();
            CO_AWAIT stop_confirmation_evt_.wait();
            service_.reset();
        }

    private:
        CORO_TASK(void) run_listener(std::shared_ptr<rpc::service> service)
        {
            CO_AWAIT service->get_scheduler()->schedule();

            while (true)
            {
                auto stream = CO_AWAIT acceptor_->accept();
                if (!stream)
                    break;
                service->spawn(on_new_connection_(*stream));
            }

            stop_confirmation_evt_.set();
            CO_RETURN;
        }

        std::shared_ptr<stream_acceptor> acceptor_;
        on_new_connection on_new_connection_;
        std::shared_ptr<rpc::service> service_;
        rpc::event stop_confirmation_evt_;
    };

} // namespace streaming
