// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"
#include "enclave_websocket_server_support.h"

#include <atomic>
#include <memory>
#include <tuple>
#include <utility>

#include <file_system/file_system_manager.h>
#include <io_uring/tcp.h>
#include <rpc/rpc.h>
#include <streaming/io_uring/stream.h>
#include <streaming/secure_stream.h>
#include <transports/sgx_coroutine/enclave/runtime.h>
#include <transports/sgx_coroutine/enclave/service.h>
#include <websocket_demo/websocket_demo.h>

namespace websocket_demo::v1
{
    // This factory is implemented in demo.cpp. The enclave build compiles that
    // file with CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY, so this demo exposes the
    // calculator and leaves llama.cpp out of the enclave for now.
    //
    // Change this if you want to expose your own application service:
    //   1. replace the IDL interface in websocket_demo.idl,
    //   2. return your service object from this factory,
    //   3. update websocket_handler.cpp so the websocket transport binds the
    //      matching inbound/outbound interfaces.
    rpc::shared_ptr<i_calculator> create_websocket_demo_instance();

    namespace
    {
        using enclave_support::make_rpc_shared_or_terminate;
        using enclave_support::make_std_shared_or_terminate;

        // Reference enclave websocket server
        // ----------------------------------
        //
        // Lifetime overview:
        //   - The host executable creates the enclave and connects to this file's
        //     registered i_enclave_websocket_server factory.
        //   - listen() runs inside the enclave. It builds enclave-owned listener
        //     state, starts an io_uring loopback acceptor, and spawns accept_loop().
        //   - Each accepted TCP stream is wrapped in TLS, parsed as HTTP, upgraded
        //     to WebSocket, and then handed to the Canopy websocket RPC transport.
        //   - stop() only asks the listener to stop and closes the acceptor. Active
        //     client coroutines retain their own shared TLS context and finish
        //     independently.
        //
        // Security notes for people copying this file:
        //   - The host currently passes certificate_pem/private_key_pem into the
        //     enclave. That is acceptable for this demo, but not for a production
        //     SGX threat model where the host is untrusted. Replace this with
        //     enclave-generated keys, sealed keys, or RA-TLS/TA-TLS credentials.
        //   - This demo serves files via file_system_manager. The current manager
        //     can proxy reads to the host io_uring backend. Change the manager
        //     construction if static pages should come from sealed storage or
        //     statically linked enclave data.
        //   - Browser clients normally do not present client certificates. Peer
        //     verification is therefore disabled below. For peer-to-peer enclave
        //     services, require client certificates or an attestation-aware verifier.
        struct enclave_websocket_state
        {
            // Immutable after construction. The client coroutines use these to
            // create service objects and read static assets.
            std::shared_ptr<rpc::service> service;
            std::shared_ptr<rpc::io_uring::controller> controller;
            rpc::shared_ptr<rpc::file_system::i_manager> file_system_manager;

            // Listener-owned objects. accept_loop() receives local shared_ptr
            // snapshots so stop() can reset these fields without racing a worker
            // thread reading the same shared_ptr object.
            std::shared_ptr<rpc::io_uring::acceptor> acceptor;
            std::shared_ptr<streaming::secure::context> tls_context;

            // Cross-coroutine stop flag. Use this instead of destroying shared
            // objects under active coroutines.
            std::atomic_bool stopping{false};
        };

        auto create_file_system_manager(const std::shared_ptr<rpc::io_uring::controller>& controller)
            -> rpc::shared_ptr<rpc::file_system::i_manager>
        {
            // Change this if you want a different asset source. For example, a
            // production enclave may serve files from sealed storage or compiled
            // byte arrays instead of asking the untrusted host filesystem.
            return rpc::file_system::create_factory(controller);
        }

        auto create_tls_context(
            const std::string& certificate_pem,
            const std::string& private_key_pem) -> std::shared_ptr<streaming::secure::context>
        {
            streaming::secure::pem_credentials credentials;
            credentials.certificate = certificate_pem;
            credentials.private_key = private_key_pem;

            // Browser demo mode: do not require client certificates.
            //
            // Change this for mTLS, RA-TLS, or TA-TLS:
            //   - provide a trust anchor or attestation verifier in the secure
            //     stream backend,
            //   - set peer verification to the policy required by your protocol,
            //   - decide whether failed peer verification should close the HTTP
            //     connection before any application data is processed.
            return make_std_shared_or_terminate<streaming::secure::context>(
                "enclave websocket TLS context",
                credentials,
                streaming::secure::server_context_options{.verify_peer = streaming::secure::peer_verification::none});
        }

        auto listen_request_is_valid(
            const enclave_websocket_state& state,
            uint16_t port,
            const std::string& certificate_pem,
            const std::string& private_key_pem) -> bool
        {
            return state.service && state.controller && state.file_system_manager && port != 0
                   && !certificate_pem.empty() && !private_key_pem.empty();
        }

        CORO_TASK(void)
        handle_tls_client(
            std::shared_ptr<enclave_websocket_state> state,
            std::shared_ptr<streaming::secure::context> tls_context,
            std::shared_ptr<streaming::stream> io_stream)
        {
            // This coroutine owns the accepted connection from this point on.
            // If you want to support plaintext development mode, branch here
            // before constructing streaming::secure::stream.
            auto tls_stream = make_std_shared_or_terminate<streaming::secure::stream>(
                "enclave websocket TLS stream", std::move(io_stream), std::move(tls_context));

            const bool handshake_ok = CO_AWAIT tls_stream->handshake();
            if (!handshake_ok)
            {
                RPC_WARNING("enclave websocket TLS handshake did not complete; closing connection");
                CO_RETURN;
            }

            // HTTP + WebSocket handling is shared with the host demo. The static
            // root string names the demo's www directory; the file_system_manager
            // decides how that path is resolved inside the enclave.
            http_client_connection connection(
                tls_stream,
                state->service,
                [] { return create_websocket_demo_instance(); },
                state->file_system_manager,
                CANOPY_WEBSOCKET_DEMO_STATIC_ROOT);

            auto transport = CO_AWAIT connection.handle();
            if (transport)
                CO_AWAIT transport->inner_accept();
            CO_RETURN;
        }

        CORO_TASK(void)
        accept_loop(
            std::shared_ptr<enclave_websocket_state> state,
            std::shared_ptr<rpc::io_uring::acceptor> acceptor,
            std::shared_ptr<streaming::secure::context> tls_context,
            uint16_t port)
        {
            while (!state->stopping.load(std::memory_order_acquire))
            {
                auto accept_result
                    = streaming::io_uring::make_stream_result(CO_AWAIT acceptor->accept_with_result(), port);
                if (state->stopping.load(std::memory_order_acquire))
                    break;

                if (accept_result.error_code != rpc::error::OK() || !accept_result.connection)
                {
                    // CALL_TIMEOUT is used as an accept guard timeout. It lets
                    // stop() be observed without treating an idle listener as an
                    // error. Change the acceptor strategy if your production
                    // listener has a different idle/wakeup policy.
                    if (accept_result.error_code == rpc::error::CALL_TIMEOUT())
                        continue;

                    RPC_ERROR(
                        "enclave websocket io_uring accept failed error={} native={}",
                        accept_result.error_code,
                        accept_result.native_result);
                    break;
                }

                if (!state->service->spawn(handle_tls_client(state, tls_context, accept_result.connection)))
                {
                    RPC_ERROR("failed to spawn enclave websocket client handler");
                    CO_AWAIT accept_result.connection->set_closed();
                }
            }

            CO_RETURN;
        }

        class enclave_websocket_server : public rpc::base<enclave_websocket_server, i_enclave_websocket_server>
        {
        public:
            enclave_websocket_server(
                std::shared_ptr<rpc::service> service,
                std::shared_ptr<rpc::io_uring::controller> controller)
                : state_(make_std_shared_or_terminate<enclave_websocket_state>("enclave websocket listener state"))
            {
                state_->service = std::move(service);
                state_->controller = std::move(controller);
                state_->file_system_manager = create_file_system_manager(state_->controller);
            }

            // Called by the untrusted host after it has connected to the enclave
            // service. This method is intentionally small and ordered:
            //   validate input -> create TLS -> create acceptor -> publish state
            //   -> spawn accept loop.
            //
            // Change this signature only rarely. Prefer adding a versioned IDL
            // request struct if the listener needs more parameters.
            CORO_TASK(int)
            listen(
                uint16_t port,
                const std::string& certificate_pem,
                const std::string& private_key_pem) override
            {
                if (!listen_request_is_valid(*state_, port, certificate_pem, private_key_pem))
                    CO_RETURN rpc::error::INVALID_DATA();

                if (state_->acceptor && state_->acceptor->is_listening())
                    CO_RETURN rpc::error::INVALID_DATA();

                // A websocket listener spends most of its life inside accept.
                // Cooperative polling has a finite guard timeout, which is
                // useful for tests but noisy for an idle server. Proactor mode
                // lets the shared controller pump wake this coroutine only when
                // the accept completion arrives.
                state_->controller->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

                auto tls_context = create_tls_context(certificate_pem, private_key_pem);
                if (!tls_context->is_valid())
                    CO_RETURN rpc::error::INVALID_DATA();

                auto acceptor = make_std_shared_or_terminate<rpc::io_uring::acceptor>(
                    "enclave websocket io_uring acceptor", state_->controller);

                // This demo listens on loopback because the host side owns the
                // external socket plumbing. Change this if your embedding uses a
                // different host/enclave boundary or forwards accepted sockets
                // into the enclave in another way.
                auto listen_error = CO_AWAIT acceptor->listen_loopback(port);
                if (listen_error != rpc::error::OK())
                    CO_RETURN listen_error;

                state_->stopping.store(false, std::memory_order_release);
                state_->tls_context = tls_context;
                state_->acceptor = std::move(acceptor);

                if (!state_->service->spawn(accept_loop(state_, state_->acceptor, std::move(tls_context), port)))
                {
                    CO_AWAIT state_->acceptor->close();
                    state_->acceptor.reset();
                    state_->tls_context.reset();
                    CO_RETURN rpc::error::TRANSPORT_ERROR();
                }

                RPC_INFO("enclave websocket server listening on io_uring loopback port {}", port);
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(int) stop() override
            {
                // stop() is deliberately cooperative. It closes the acceptor so
                // accept_loop() wakes, and then drops the listener's references.
                // Existing client coroutines keep their own TLS context snapshot.
                state_->stopping.store(true, std::memory_order_release);
                auto acceptor = state_->acceptor;
                if (acceptor)
                    CO_AWAIT acceptor->close();
                state_->acceptor.reset();
                state_->tls_context.reset();
                CO_RETURN rpc::error::OK();
            }

        private:
            std::shared_ptr<enclave_websocket_state> state_;
        };

        struct connection_factory_registrar
        {
            connection_factory_registrar()
            {
                // The host connects by name ("websocket_demo_enclave"). Change
                // this name if you package several enclave services in one image.
                rpc::sgx::coro::enclave::register_connection_factory<rpc::i_noop, i_enclave_websocket_server>(
                    "websocket_demo_enclave",
                    [](rpc::shared_ptr<rpc::i_noop> host, std::shared_ptr<rpc::service> service)
                        -> CORO_TASK(rpc::service_connect_result<i_enclave_websocket_server>)
                    {
                        std::ignore = host;
                        auto enclave_service = std::dynamic_pointer_cast<rpc::enclave_service>(service);
                        if (!enclave_service)
                        {
                            CO_RETURN rpc::service_connect_result<i_enclave_websocket_server>{
                                rpc::error::INCOMPATIBLE_SERVICE(), {}};
                        }

                        auto controller = enclave_service->get_io_uring_controller();
                        if (!controller)
                        {
                            CO_RETURN rpc::service_connect_result<i_enclave_websocket_server>{
                                rpc::error::INCOMPATIBLE_SERVICE(), {}};
                        }

                        auto server = make_rpc_shared_or_terminate<enclave_websocket_server>(
                            "enclave websocket server", std::move(service), std::move(controller));
                        CO_RETURN rpc::service_connect_result<i_enclave_websocket_server>{rpc::error::OK(), server};
                    });
            }
        };

        connection_factory_registrar g_connection_factory_registrar;
    } // namespace
} // namespace websocket_demo::v1
