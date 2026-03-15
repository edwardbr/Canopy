/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <coro/coro.hpp>
#include <rpc/rpc.h>

#include <streaming/stream.h>
#include <streaming/stream_acceptor.h>

namespace streaming
{
    // Accepts streams from a stream_acceptor, optionally transforms each stream
    // (e.g. TLS handshake, HTTP→WebSocket upgrade), then creates an RPC transport
    // and registers the remote zone with the service — all without exposing
    // connection_settings, interface_descriptor, or attach_remote_zone to user code.
    //
    // Usage:
    //   auto listener = std::make_shared<streaming::listener>(
    //       "my_connection",
    //       std::make_shared<streaming::tcp::acceptor>(endpoint),
    //       rpc::stream_transport::make_connection_callback<i_remote, i_local>(
    //           [](const rpc::shared_ptr<i_remote>& remote,
    //               rpc::shared_ptr<i_local>& local,
    //               const std::shared_ptr<rpc::service>& svc) -> CORO_TASK(int)
    //           {
    //               local = rpc::shared_ptr<i_local>(new my_local_impl(svc));
    //               CO_RETURN rpc::error::OK();
    //           }));
    //   listener->start_listening(service);

    class listener
    {
    public:
        // Optional: transforms the raw accepted stream before handing it to the transport.
        // Return nullopt to reject the connection (e.g. failed TLS handshake,
        // non-WebSocket HTTP request handled inline).
        using stream_transformer
            = std::function<CORO_TASK(std::optional<std::shared_ptr<stream>>)(std::shared_ptr<stream>)>;

        // Called per accepted stream to create a transport.
        // Obtain via rpc::stream_transport::make_connection_callback<Remote, Local>(zone_factory) —
        // the zone factory and all connection protocol details are baked in.
        using connection_callback
            = std::function<CORO_TASK(void)(const std::string&, std::shared_ptr<rpc::service>, std::shared_ptr<stream>)>;

        listener(std::string name,
            std::shared_ptr<stream_acceptor> acceptor,
            connection_callback on_connection,
            stream_transformer transform_stream = {})
            : acceptor_(std::move(acceptor))
            , name_(std::move(name))
            , make_transport_(std::move(on_connection))
            , transformer_(std::move(transform_stream))
        {
        }

        listener(const listener&) = delete;
        auto operator=(const listener&) -> listener& = delete;

        bool start_listening(std::shared_ptr<rpc::service> service)
        {
            service_ = service;
            if (!acceptor_->init(service->get_scheduler()))
                return false;
            return service->spawn(run(service));
        }

        CORO_TASK(void) stop_listening()
        {
            acceptor_->stop();
            CO_AWAIT stop_evt_.wait();
            service_.reset();
            CO_RETURN;
        }

    private:
        CORO_TASK(void) run(std::shared_ptr<rpc::service> service)
        {
            CO_AWAIT service->get_scheduler()->schedule();

            while (true)
            {
                auto maybe = CO_AWAIT acceptor_->accept();
                if (!maybe)
                    break;
                service->spawn(handle_connection(service, *maybe));
            }

            stop_evt_.set();
            CO_RETURN;
        }

        CORO_TASK(void) handle_connection(std::shared_ptr<rpc::service> service, std::shared_ptr<stream> stm)
        {
            if (transformer_)
            {
                auto wrapped = CO_AWAIT transformer_(stm);
                if (!wrapped)
                    CO_RETURN;
                stm = *wrapped;
            }

            CO_AWAIT make_transport_(name_, service, std::move(stm));
            CO_RETURN;
        }

        std::shared_ptr<stream_acceptor> acceptor_;
        std::string name_;
        connection_callback make_transport_;
        stream_transformer transformer_;
        std::shared_ptr<rpc::service> service_;
        rpc::event stop_evt_;
    };

} // namespace streaming
