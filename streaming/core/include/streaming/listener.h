/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>

#include <coro/coro.hpp>
#include <rpc/rpc.h>

#include <streaming/stream.h>
#include <streaming/stream_acceptor.h>

namespace streaming
{
    namespace debug
    {
        struct listener_diagnostics
        {
            std::atomic<uint64_t> start_calls{0};
            std::atomic<uint64_t> run_entries{0};
            std::atomic<uint64_t> accept_results{0};
            std::atomic<uint64_t> handle_connection_calls{0};
            std::atomic<uint64_t> stop_calls{0};
        };

        inline auto listener_diag() -> listener_diagnostics&
        {
            static listener_diagnostics diag;
            return diag;
        }

        inline void reset_listener_diagnostics()
        {
            auto& d = listener_diag();
            d.start_calls.store(0, std::memory_order_relaxed);
            d.run_entries.store(0, std::memory_order_relaxed);
            d.accept_results.store(0, std::memory_order_relaxed);
            d.handle_connection_calls.store(0, std::memory_order_relaxed);
            d.stop_calls.store(0, std::memory_order_relaxed);
        }

        inline void dump_listener_diagnostics(std::ostream& out)
        {
            auto& d = listener_diag();
            out << "listener diagnostics:"
                << " start_calls=" << d.start_calls.load(std::memory_order_relaxed)
                << " run_entries=" << d.run_entries.load(std::memory_order_relaxed)
                << " accept_results=" << d.accept_results.load(std::memory_order_relaxed)
                << " handle_connection_calls=" << d.handle_connection_calls.load(std::memory_order_relaxed)
                << " stop_calls=" << d.stop_calls.load(std::memory_order_relaxed)
                << '\n';
        }
    } // namespace debug

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
    //   CO_AWAIT listener->start_listening_async(service);

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
            debug::listener_diag().start_calls.fetch_add(1, std::memory_order_relaxed);
            ready_evt_.reset();
            stop_evt_.reset();
            service_ = service;
            if (!acceptor_->init(service->get_scheduler()))
                return false;
            return service->spawn(run(service));
        }

        CORO_TASK(bool) start_listening_async(std::shared_ptr<rpc::service> service)
        {
            if (!start_listening(service))
                CO_RETURN false;
            CO_AWAIT ready_evt_.wait();
            CO_RETURN true;
        }

        CORO_TASK(void) stop_listening()
        {
            debug::listener_diag().stop_calls.fetch_add(1, std::memory_order_relaxed);
            acceptor_->stop();
            CO_AWAIT stop_evt_.wait();
            service_.reset();
            CO_RETURN;
        }

    private:
        CORO_TASK(void) run(std::shared_ptr<rpc::service> service)
        {
            debug::listener_diag().run_entries.fetch_add(1, std::memory_order_relaxed);
            CO_AWAIT service->get_scheduler()->schedule();
            ready_evt_.set();

            while (true)
            {
                auto maybe = CO_AWAIT acceptor_->accept();
                if (!maybe)
                    break;
                debug::listener_diag().accept_results.fetch_add(1, std::memory_order_relaxed);
                service->spawn(handle_connection(service, *maybe));
            }

            stop_evt_.set();
            CO_RETURN;
        }

        CORO_TASK(void) handle_connection(std::shared_ptr<rpc::service> service, std::shared_ptr<stream> stm)
        {
            debug::listener_diag().handle_connection_calls.fetch_add(1, std::memory_order_relaxed);
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
        rpc::event ready_evt_;
        rpc::event stop_evt_;
    };

} // namespace streaming
