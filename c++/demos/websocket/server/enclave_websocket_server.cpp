// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"
#include "enclave_websocket_server_support.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <file_system/file_system_manager.h>
#include <io_uring/tcp.h>
#include <json/convert.h>
#include <json/schema_validator.h>
#include <rpc/rpc.h>
#include <streaming/tcp_coroutine/stream.h>
#include <streaming/secure_stream.h>
#include <transports/sgx_coroutine/enclave/runtime.h>
#include <transports/sgx_coroutine/enclave/service.h>
#include <network_config/network_config_schema.h>
#include <websocket_demo/websocket_demo.h>
#include <websocket_demo/websocket_demo_schema.h>

namespace websocket_demo::v1
{
    // This factory is implemented in demo.cpp. The enclave build compiles that
    // file without llama.cpp, but still passes the service into the demo so the
    // video relay can use the service scheduler.
    //
    // Change this if you want to expose your own application service:
    //   1. replace the IDL interface in websocket_demo.idl,
    //   2. return your service object from this factory,
    //   3. update websocket_handler.cpp so the untrusted_web transport binds the
    //      matching inbound/outbound interfaces.
    rpc::shared_ptr<i_calculator> create_websocket_demo_instance(const std::shared_ptr<rpc::service>& service);

    namespace
    {
        using enclave_support::make_std_shared_result;

        constexpr const char* websocket_demo_app_name = "websocket_demo_enclave";

        // Reference enclave websocket server
        // ----------------------------------
        //
        // Lifetime overview:
        //   - The host executable creates the enclave and connects to this file's
        //     registered i_enclave_websocket_server factory.
        //   - listen() runs inside the enclave. It builds enclave-owned listener
        //     state, starts an io_uring loopback acceptor, and spawns accept_loop().
        //   - Each accepted TCP stream is optionally wrapped in TLS, parsed as
        //     HTTP, upgraded to WebSocket, and then handed to the Canopy websocket
        //     RPC transport.
        //   - stop() only asks the listener to stop and closes the acceptor. Active
        //     client coroutines retain the stream state they need and finish
        //     independently.
        //
        // Security notes for people copying this file:
        //   - The host currently passes certificate/key paths into the enclave.
        //     The enclave reads those files through its file-system manager so
        //     later implementations can seal or replace those credentials inside
        //     the enclave boundary.
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
            bool listen_ipv6{false};
            std::array<uint8_t, 4> listen_ipv4_address{127, 0, 0, 1};
            std::array<uint8_t, 16> listen_ipv6_address{};
            bool listen_endpoint_valid{true};
            uint16_t listen_port{8080};
            std::string static_root_path;
            std::string certificate_path;
            std::string private_key_path;

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
            const std::string& private_key_pem) -> enclave_support::shared_create_result<streaming::secure::context>
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
            return make_std_shared_result<streaming::secure::context>(
                "enclave websocket TLS context",
                credentials,
                streaming::secure::server_context_options{.verify_peer = streaming::secure::peer_verification::none});
        }

        auto bytes_to_string(const std::vector<uint8_t>& data) -> std::string
        {
            return std::string(reinterpret_cast<const char*>(data.data()), data.size());
        }

        struct tls_credentials_result
        {
            int error_code{rpc::error::OK()};
            std::string certificate_pem;
            std::string private_key_pem;
        };

        CORO_TASK(tls_credentials_result)
        load_tls_credentials(
            const rpc::shared_ptr<rpc::file_system::i_manager>& file_system_manager,
            const std::string& certificate_path,
            const std::string& private_key_path)
        {
            if (!file_system_manager || certificate_path.empty() || private_key_path.empty())
                CO_RETURN tls_credentials_result{rpc::error::INVALID_DATA(), {}, {}};

            std::vector<uint8_t> certificate;
            auto error_code = CO_AWAIT file_system_manager->read_file(certificate_path, certificate);
            if (error_code != rpc::error::OK())
            {
                RPC_ERROR("enclave failed to read TLS certificate {} error={}", certificate_path, error_code);
                CO_RETURN tls_credentials_result{error_code, {}, {}};
            }

            std::vector<uint8_t> private_key;
            error_code = CO_AWAIT file_system_manager->read_file(private_key_path, private_key);
            if (error_code != rpc::error::OK())
            {
                RPC_ERROR("enclave failed to read TLS private key {} error={}", private_key_path, error_code);
                CO_RETURN tls_credentials_result{error_code, {}, {}};
            }

            if (certificate.empty() || private_key.empty())
                CO_RETURN tls_credentials_result{rpc::error::INVALID_DATA(), {}, {}};

            CO_RETURN tls_credentials_result{rpc::error::OK(), bytes_to_string(certificate), bytes_to_string(private_key)};
        }

        auto decode_websocket_server_options(
            const json::v1::object& json_options,
            server_options& typed_options) -> int
        {
            try
            {
                static const json::v1::schema::schema_validator validator(json::v1::parse(server_options::get_schema()));

                const auto validation = validator.validate(json_options);
                if (!validation)
                {
                    const auto& errors = validation.errors();
                    if (!errors.empty())
                    {
                        RPC_WARNING(
                            "invalid enclave websocket startup options at {} schema {}: {}",
                            errors.front().path,
                            errors.front().schema_path,
                            errors.front().message);
                    }
                    return rpc::error::INVALID_DATA();
                }

                typed_options = json::v1::convert::from_json_object<server_options>(json_options);
                return rpc::error::OK();
            }
            catch (const std::exception& error)
            {
                RPC_WARNING("failed to decode enclave websocket startup options: {}", error.what());
                return rpc::error::INVALID_DATA();
            }
        }

        struct startup_listen_endpoint
        {
            bool valid{true};
            bool ipv6{false};
            std::array<uint8_t, 4> ipv4_address{127, 0, 0, 1};
            std::array<uint8_t, 16> ipv6_address{};
        };

        auto parse_startup_listen_endpoint(const canopy::network_config::tcp_endpoint& options) -> startup_listen_endpoint
        {
            startup_listen_endpoint endpoint;
            if (options.family == canopy::network_config::ip_address_family::ipv6)
                endpoint.ipv6 = true;
            else if (options.family != canopy::network_config::ip_address_family::ipv4)
            {
                endpoint.valid = false;
                return endpoint;
            }

            if (endpoint.ipv6)
                std::copy(options.addr.begin(), options.addr.end(), endpoint.ipv6_address.begin());
            else
                std::copy(
                    options.addr.begin(),
                    options.addr.begin() + endpoint.ipv4_address.size(),
                    endpoint.ipv4_address.begin());
            return endpoint;
        }

        auto listen_request_is_valid(const enclave_websocket_state& state) -> bool
        {
            // The generated JSON schema validates the config shape. This guard
            // also covers runtime dependencies and semantic constraints that are
            // not currently encoded in the IDL schema.
            const bool has_tls_paths = !state.certificate_path.empty() && !state.private_key_path.empty();
            const bool has_partial_tls_paths = !state.certificate_path.empty() || !state.private_key_path.empty();
            return state.service && state.controller && state.file_system_manager && state.listen_endpoint_valid
                   && state.listen_port != 0 && !state.static_root_path.empty()
                   && (has_tls_paths || !has_partial_tls_paths);
        }

        CORO_TASK(void)
        handle_client(
            std::shared_ptr<enclave_websocket_state> state,
            std::shared_ptr<streaming::secure::context> tls_context,
            std::shared_ptr<streaming::stream> io_stream)
        {
            // This coroutine owns the accepted connection from this point on.
            std::shared_ptr<streaming::stream> client_stream = std::move(io_stream);
            if (tls_context)
            {
                auto tls_stream_result = make_std_shared_result<streaming::secure::stream>(
                    "enclave websocket TLS stream", std::move(client_stream), std::move(tls_context));
                if (tls_stream_result.error_code != rpc::error::OK() || !tls_stream_result.value)
                {
                    RPC_ERROR("failed to create enclave websocket TLS stream error={}", tls_stream_result.error_code);
                    if (client_stream)
                        CO_AWAIT client_stream->set_closed();
                    CO_RETURN;
                }

                auto tls_stream = std::move(tls_stream_result.value);
                const bool handshake_ok = CO_AWAIT tls_stream->handshake();
                if (!handshake_ok)
                {
                    RPC_WARNING("enclave websocket TLS handshake did not complete; closing connection");
                    CO_RETURN;
                }

                client_stream = std::move(tls_stream);
            }

            // HTTP + WebSocket handling is shared with the host demo. The static
            // root string names the demo's www directory; the file_system_manager
            // decides how that path is resolved inside the enclave.
            http_client_connection connection(
                std::move(client_stream),
                state->service,
                [service = state->service] { return create_websocket_demo_instance(service); },
                state->file_system_manager,
                state->static_root_path);

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
                    = streaming::coroutine::tcp::make_stream_result(CO_AWAIT acceptor->accept_with_result(), port);
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

                if (!state->service->spawn(handle_client(state, tls_context, accept_result.connection)))
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
                std::shared_ptr<rpc::io_uring::controller> controller,
                server_options options,
                std::shared_ptr<enclave_websocket_state> state)
                : state_(std::move(state))
            {
                const auto listen_endpoint = parse_startup_listen_endpoint(options.listen);
                state_->service = std::move(service);
                state_->controller = std::move(controller);
                state_->file_system_manager = create_file_system_manager(state_->controller);
                state_->listen_ipv6 = listen_endpoint.ipv6;
                state_->listen_ipv4_address = listen_endpoint.ipv4_address;
                state_->listen_ipv6_address = listen_endpoint.ipv6_address;
                state_->listen_endpoint_valid = listen_endpoint.valid;
                state_->listen_port = options.listen.port;
                state_->static_root_path = std::move(options.path);
                if (options.tls)
                {
                    auto tls_options = std::move(*options.tls);
                    state_->certificate_path = std::move(tls_options.certificate_path);
                    state_->private_key_path = std::move(tls_options.private_key_path);
                }
            }

            // Called by the untrusted host after it has connected to the enclave
            // service. This method is intentionally small and ordered:
            //   validate input -> optionally create TLS -> create acceptor ->
            //   publish state -> spawn accept loop.
            //
            // Change this signature only rarely. Prefer adding a versioned IDL
            // request struct if the listener needs more parameters.
            CORO_TASK(int) listen() override
            {
                if (!listen_request_is_valid(*state_))
                    CO_RETURN rpc::error::INVALID_DATA();

                if (state_->acceptor && state_->acceptor->is_listening())
                    CO_RETURN rpc::error::INVALID_DATA();

                // A websocket listener spends most of its life inside accept.
                // Cooperative polling has a finite guard timeout, which is
                // useful for tests but noisy for an idle server. Proactor mode
                // lets the shared controller pump wake this coroutine only when
                // the accept completion arrives.
                state_->controller->set_wait_strategy(rpc::io_uring::wait_strategy::proactor);

                std::shared_ptr<streaming::secure::context> tls_context;
                if (!state_->certificate_path.empty() && !state_->private_key_path.empty())
                {
                    auto credentials = CO_AWAIT load_tls_credentials(
                        state_->file_system_manager, state_->certificate_path, state_->private_key_path);
                    if (credentials.error_code != rpc::error::OK())
                        CO_RETURN credentials.error_code;

                    auto tls_context_result = create_tls_context(credentials.certificate_pem, credentials.private_key_pem);
                    if (tls_context_result.error_code != rpc::error::OK() || !tls_context_result.value)
                    {
                        CO_RETURN tls_context_result.error_code != rpc::error::OK() ? tls_context_result.error_code
                                                                                    : rpc::error::OUT_OF_MEMORY();
                    }

                    tls_context = std::move(tls_context_result.value);
                    if (!tls_context->is_valid())
                        CO_RETURN rpc::error::INVALID_DATA();
                }

                auto acceptor_result = make_std_shared_result<rpc::io_uring::acceptor>(
                    "enclave websocket io_uring acceptor", state_->controller);
                if (acceptor_result.error_code != rpc::error::OK() || !acceptor_result.value)
                    CO_RETURN acceptor_result.error_code != rpc::error::OK() ? acceptor_result.error_code
                                                                             : rpc::error::OUT_OF_MEMORY();

                auto acceptor = std::move(acceptor_result.value);
                auto listen_error
                    = state_->listen_ipv6
                          ? CO_AWAIT acceptor->listen_ipv6(state_->listen_ipv6_address, state_->listen_port)
                          : CO_AWAIT acceptor->listen_ipv4(state_->listen_ipv4_address, state_->listen_port);
                if (listen_error != rpc::error::OK())
                    CO_RETURN listen_error;

                state_->stopping.store(false, std::memory_order_release);
                const bool tls_enabled = static_cast<bool>(tls_context);
                state_->tls_context = tls_context;
                state_->acceptor = std::move(acceptor);

                if (!state_->service->spawn(
                        accept_loop(state_, state_->acceptor, std::move(tls_context), state_->listen_port)))
                {
                    CO_AWAIT state_->acceptor->close();
                    state_->acceptor.reset();
                    state_->tls_context.reset();
                    CO_RETURN rpc::error::TRANSPORT_ERROR();
                }

                if (tls_enabled)
                    RPC_INFO("enclave websocket server listening on io_uring port {} (TLS enabled)", state_->listen_port);
                else
                    RPC_INFO("enclave websocket server listening on io_uring port {}", state_->listen_port);
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(int) stop() override
            {
                // stop() is deliberately cooperative. It closes the acceptor so
                // accept_loop() wakes, and then drops the listener's references.
                // Existing client coroutines keep their own stream state snapshot.
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

        auto create_enclave_websocket_server(
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<rpc::io_uring::controller> controller,
            server_options options) -> rpc::service_connect_result<i_enclave_websocket_server>
        {
            try
            {
                auto state_result = make_std_shared_result<enclave_websocket_state>("enclave websocket listener state");
                if (state_result.error_code != rpc::error::OK() || !state_result.value)
                {
                    return {state_result.error_code != rpc::error::OK() ? state_result.error_code
                                                                        : rpc::error::OUT_OF_MEMORY(),
                        {}};
                }

                auto server = rpc::make_shared<enclave_websocket_server>(
                    std::move(service), std::move(controller), std::move(options), std::move(state_result.value));
                if (!server)
                    return {rpc::error::OUT_OF_MEMORY(), {}};

                return {rpc::error::OK(), std::move(server)};
            }
            catch (const std::bad_alloc&)
            {
                enclave_support::log_bad_alloc("enclave websocket server");
                return {rpc::error::OUT_OF_MEMORY(), {}};
            }
            catch (const std::exception& error)
            {
                RPC_ERROR("exception while creating enclave websocket server: {}", error.what());
                return {rpc::error::EXCEPTION(), {}};
            }
            catch (...)
            {
                RPC_ERROR("unknown exception while creating enclave websocket server");
                return {rpc::error::EXCEPTION(), {}};
            }
        }

        struct enclave_entry_point
        {
            enclave_entry_point()
            {
                // The host connects by name. Change this name if you package
                // several enclave services in one image.
                rpc::sgx_coroutine_transport::enclave::register_connection_factory<rpc::i_noop, i_enclave_websocket_server>(
                    websocket_demo_app_name,
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

                        const auto& startup_applications
                            = rpc::sgx_coroutine_transport::enclave::runtime_startup_applications();
                        const auto startup_options = startup_applications.find(websocket_demo_app_name);
                        if (startup_options == startup_applications.end())
                        {
                            RPC_WARNING("missing enclave websocket startup options");
                            CO_RETURN rpc::service_connect_result<i_enclave_websocket_server>{
                                rpc::error::INVALID_DATA(), {}};
                        }

                        server_options parsed_options;
                        if (auto error_code = decode_websocket_server_options(startup_options->second, parsed_options);
                            error_code != rpc::error::OK())
                        {
                            CO_RETURN rpc::service_connect_result<i_enclave_websocket_server>{error_code, {}};
                        }

                        CO_RETURN create_enclave_websocket_server(
                            std::move(service), std::move(controller), std::move(parsed_options));
                    });
            }
        };

        enclave_entry_point g_enclave_entry_point;
    } // namespace
} // namespace websocket_demo::v1
