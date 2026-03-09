/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/rpc.h>
#include <coro/coro.hpp>
#include <coro/net/tcp/server.hpp>
#include <streaming/io_uring_tcp_stream.h>
#include <transports/streaming/transport.h>

namespace rpc::io_uring
{
    /**
     * @brief io_uring listener that accepts incoming connections and creates streaming_transport
     * instances backed by io_uring_tcp_stream
     *
     * Mirrors rpc::tcp::listener but wraps each accepted connection in an io_uring_tcp_stream
     * instead of tcp_stream, giving fully asynchronous kernel-bypass I/O per connection.
     */
    class listener
    {
        rpc::event stop_confirmation_evt_;
        bool stop_ = false;
        std::chrono::milliseconds poll_timeout_ = std::chrono::milliseconds(10);

        using connection_handler = std::function<CORO_TASK(int)(const rpc::connection_settings& input_descr,
            rpc::interface_descriptor& output_interface,
            std::shared_ptr<rpc::service> child_service_ptr,
            std::shared_ptr<rpc::stream_transport::streaming_transport> transport)>;

        connection_handler connection_handler_;
        std::shared_ptr<coro::net::tcp::server> server_;
        std::shared_ptr<rpc::service> service_;

    public:
        listener(connection_handler handler)
            : connection_handler_(handler)
        {
        }

        listener(const listener&) = delete;
        listener& operator=(const listener&) = delete;

        /**
         * @brief Start listening for incoming connections
         *
         * Schedules the listening coroutine on the service's scheduler.
         */
        bool start_listening(std::shared_ptr<rpc::service> service,
            const coro::net::socket_address& endpoint,
            coro::net::tcp::server::options opts = {})
        {
            service_ = service;
            auto scheduler = service->get_scheduler();
            server_ = std::make_shared<coro::net::tcp::server>(scheduler, endpoint, opts);
            return service->spawn(run_listener(service));
        }

        /**
         * @brief Stop listening for incoming connections
         *
         * Gracefully stops the listening loop and waits for confirmation.
         */
        CORO_TASK(void) stop_listening()
        {
            stop_ = true;
            CO_AWAIT stop_confirmation_evt_.wait();
            // Release our reference to the service so the service reference
            // count can reach zero and trigger its shutdown event. Mirrors
            // the effect of `listener.reset()` in the TCP server task.
            service_.reset();
        }

    private:
        CORO_TASK(void) run_client(std::shared_ptr<rpc::service> service, coro::net::tcp::client client)
        {
            assert(client.socket().is_ok());

            auto scheduler = service->get_scheduler();
            auto stm = std::make_shared<streaming::io_uring_tcp_stream>(std::move(client), scheduler);
            auto transport = rpc::stream_transport::streaming_transport::create(
                "io_uring_server_transport", service, std::move(stm), connection_handler_);

            service->spawn(transport->pump_send_and_receive());
            CO_RETURN;
        }

        CORO_TASK(void) run_listener(std::shared_ptr<rpc::service> service)
        {
            auto scheduler = service->get_scheduler();
            CO_AWAIT scheduler->schedule();

            if (!server_)
            {
                RPC_ERROR("server_ is null in io_uring::listener::run_listener");
                CO_RETURN;
            }

            while (!stop_)
            {
                auto client = co_await server_->accept(poll_timeout_);
                if (client)
                {
                    RPC_DEBUG("Accepted io_uring connection, scheduling run_client");
                    service->spawn(run_client(service, std::move(*client)));
                }
                else if (client.error().is_timeout())
                {
                    continue;
                }
                else
                {
                    if (!stop_)
                    {
                        RPC_ERROR("io_uring::listener: accept error");
                    }
                    break;
                }
            }

            stop_confirmation_evt_.set();
            CO_RETURN;
        }
    };
}
