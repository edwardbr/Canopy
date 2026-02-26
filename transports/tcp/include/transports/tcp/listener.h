/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <mutex>
#include <chrono>
#include <functional>

#include <rpc/rpc.h>
#include <coro/coro.hpp>
#include <coro/net/tcp/server.hpp>
#include <transports/tcp/transport.h>

namespace rpc::tcp
{
    /**
     * @brief TCP listener that accepts incoming connections and creates tcp_transport instances
     *
     * This class provides separation of concerns by handling the server-side TCP listening
     * logic separately from the transport implementation. It creates a TCP server, accepts
     * incoming connections, and for each connection creates a tcp_transport instance.
     */
    class listener
    {
        rpc::event stop_confirmation_evt_;
        bool stop_ = false;
        std::chrono::milliseconds timeout_;
        std::chrono::milliseconds poll_timeout_ = std::chrono::milliseconds(10);

        using connection_handler = std::function<CORO_TASK(int)(const rpc::connection_settings& input_descr,
            rpc::interface_descriptor& output_interface,
            std::shared_ptr<rpc::service> child_service_ptr,
            std::shared_ptr<tcp_transport> transport)>;

        connection_handler connection_handler_;
        std::shared_ptr<coro::net::tcp::server> server_;
        std::shared_ptr<rpc::service> service_;

    public:
        /**
         * @brief Construct a new listener
         *
         * @param handler Connection handler callback invoked when a connection is accepted
         * @param timeout Timeout for TCP operations
         * @param server_options Options for configuring the TCP server (address, port, SSL, etc.)
         */
        listener(connection_handler handler, std::chrono::milliseconds timeout)
            : timeout_(timeout)
            , connection_handler_(handler)
        {
        }

        listener(const listener&) = delete;
        listener& operator=(const listener&) = delete;

        /**
         * @brief Start listening for incoming TCP connections
         *
         * This schedules the listening coroutine on the service's scheduler.
         *
         * @param service The RPC service to attach incoming connections to
         * @return true if listening started successfully, false otherwise
         */
        bool start_listening(std::shared_ptr<rpc::service> service,
            const coro::net::socket_address& endpoint,
            coro::net::tcp::server::options opts = {})
        {
            service_ = service;
            server_ = std::make_shared<coro::net::tcp::server>(service->get_scheduler(), endpoint, opts);
            return service->spawn(run_listener(service));
        }

        /**
         * @brief Stop listening for incoming connections
         *
         * This gracefully stops the listening loop and waits for confirmation.
         */
        CORO_TASK(void) stop_listening()
        {
            stop_ = true;
            CO_AWAIT stop_confirmation_evt_.wait();
        }

    private:
        /**
         * @brief Handle an incoming client connection
         *
         * Creates a tcp_transport for the connection and invokes the connection handler.
         * The transport's pump_send_and_receive is scheduled to handle ongoing communication.
         *
         * @param service The RPC service
         * @param client The accepted TCP client connection
         */
        CORO_TASK(void) run_client(std::shared_ptr<rpc::service> service, coro::net::tcp::client client)
        {
            assert(client.socket().is_ok());

            // Create a transport for this incoming connection
            // We don't know the remote zone ID yet - it will be provided in the handshake
            auto transport = tcp_transport::create("server_transport",
                service,
                rpc::zone{0}, // Will be updated during handshake
                timeout_,
                std::move(client),
                connection_handler_);

            // Start the pump to handle send/receive for this connection
            // This will also handle the initial handshake via create_stub
            service->spawn(transport->pump_send_and_receive());

            CO_RETURN;
        }

        /**
         * @brief Main listening loop
         *
         * Accepts incoming connections and spawns run_client for each one.
         *
         * @param service The RPC service
         */
        CORO_TASK(void) run_listener(std::shared_ptr<rpc::service> service)
        {
            auto scheduler = service->get_scheduler();
            CO_AWAIT scheduler->schedule();

            if (!server_)
            {
                RPC_ERROR("server_ is null in run_listener");
                CO_RETURN;
            }

            while (!stop_)
            {
                // Accept the incoming client connection
                auto client = co_await server_->accept(poll_timeout_);
                if (client)
                {
                    RPC_DEBUG("Accepted TCP connection, scheduling run_client");
                    service->spawn(run_client(service, std::move(*client)));
                }
                else if (client.error().is_timeout())
                {
                    // Poll window elapsed with no new connection — keep waiting
                    continue;
                }
                else
                {
                    // Actual socket error (closed, etc.)
                    if (!stop_)
                    {
                        RPC_ERROR("run_listener: accept error");
                    }
                    break;
                }
            }

            stop_confirmation_evt_.set();
            CO_RETURN;
        }
    };
}
