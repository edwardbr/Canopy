/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <rpc/rpc.h>
#include <streaming/listener.h>
#include <stream_transport/stream_transport_config.h>
#include <transports/streaming/transport.h>

namespace streaming
{
    class listener;
    class stream;
    class stream_acceptor;
} // namespace streaming

namespace rpc::stream_transport
{
    struct service_settings
    {
        rpc::optional<std::string> name;
    };

    struct connection_settings
    {
        service_settings service;
        transport_settings transport;
        listener_settings listener;
    };

    template<class Remote, class Local>
    using rpc_factory
        = std::function<CORO_TASK(rpc::service_connect_result<Local>)(rpc::shared_ptr<Remote>, std::shared_ptr<rpc::service>)>;

    using rpc_transport_observer = std::function<void(std::shared_ptr<rpc::stream_transport::transport>)>;

    std::shared_ptr<::streaming::stream> keep_owner(
        std::shared_ptr<::streaming::stream> stream,
        std::shared_ptr<void> owner);

    rpc::executor_ptr make_default_executor();

    std::optional<rpc::encoding> encoding_option(const transport_settings& settings);

    std::string configured_name(
        const rpc::optional<std::string>& configured,
        std::string fallback);

    std::string service_name(
        const service_settings& settings,
        std::string fallback);

    std::string transport_name(
        const transport_settings& settings,
        std::string fallback);

    std::string service_proxy_name(
        const transport_settings& settings,
        std::string fallback);

    std::string listener_name(
        const listener_settings& settings,
        std::string fallback);

    stream_transport_options transport_options(const transport_settings& settings);

    int configure_service(
        const std::shared_ptr<rpc::service>& service,
        const transport_settings& settings);

    std::shared_ptr<rpc::service> ensure_service(
        const service_settings& service_settings,
        const transport_settings& transport_settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name);

    std::shared_ptr<rpc::service> ensure_service(
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name);

    std::shared_ptr<rpc::service> ensure_service(
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name);

    connection_settings make_connection_settings(
        transport_settings transport = {},
        service_settings service = {},
        listener_settings listener = {});

    struct stream_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<::streaming::stream> stream;
        int32_t native_result{0};
    };

    struct stream_acceptor_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<::streaming::stream_acceptor> acceptor;
        std::shared_ptr<void> owner;
        uint16_t port{0};
    };

    using stream_callback = std::function<CORO_TASK(void)(std::shared_ptr<::streaming::stream>)>;

    class stream_accept_handle : public std::enable_shared_from_this<stream_accept_handle>
    {
    public:
        stream_accept_handle(
            std::shared_ptr<::streaming::stream_acceptor> acceptor,
            std::shared_ptr<rpc::service> service,
            stream_callback callback,
            std::shared_ptr<void> owner = {},
            uint16_t port = 0);

        [[nodiscard]] bool start();

        CORO_TASK(void) stop();

        [[nodiscard]] uint16_t port() const noexcept;

    private:
        CORO_TASK(void) run(std::shared_ptr<stream_accept_handle> self);

        std::shared_ptr<::streaming::stream_acceptor> acceptor_;
        std::shared_ptr<rpc::service> service_;
        rpc::executor_ptr executor_;
        stream_callback callback_;
        std::shared_ptr<void> owner_;
        uint16_t port_{0};
        rpc::event stopped_;
        std::atomic<bool> running_{false};
        std::atomic<bool> stopping_{false};
    };

    struct stream_accept_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<stream_accept_handle> handle;
    };

    stream_accept_result accept_streams(
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        stream_callback callback,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {},
        uint16_t port = 0);

    class listener_handle
    {
    public:
        listener_handle(
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<::streaming::stream_acceptor> acceptor,
            std::unique_ptr<::streaming::listener> listener,
            std::shared_ptr<void> owner = {},
            uint16_t port = 0);
        ~listener_handle();

        listener_handle(const listener_handle&) = delete;
        auto operator=(const listener_handle&) -> listener_handle& = delete;

        CORO_TASK(void) stop();

        [[nodiscard]] std::shared_ptr<rpc::service> service() const;
        [[nodiscard]] uint16_t port() const noexcept;

    private:
        std::shared_ptr<rpc::service> service_;
        std::shared_ptr<::streaming::stream_acceptor> acceptor_;
        std::unique_ptr<::streaming::listener> listener_;
        std::shared_ptr<void> owner_;
        uint16_t port_{0};
    };

    struct listener_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<listener_handle> handle;
    };

    class rpc_connection_handle
    {
    public:
        rpc_connection_handle(
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<rpc::stream_transport::transport> transport,
            std::shared_ptr<void> owner = {});

        [[nodiscard]] std::shared_ptr<rpc::service> service() const;
        [[nodiscard]] std::shared_ptr<rpc::stream_transport::transport> transport() const;

    private:
        std::shared_ptr<rpc::service> service_;
        std::shared_ptr<rpc::stream_transport::transport> transport_;
        std::shared_ptr<void> owner_;
    };

    struct rpc_accept_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc_connection_handle> handle;
    };

    struct client_rpc_stream_transport_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc::service> service;
        std::shared_ptr<rpc::stream_transport::transport> transport;
    };

    client_rpc_stream_transport_result make_client_rpc_stream_transport(
        std::shared_ptr<::streaming::stream> stream,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(listener_result)
    start_rpc_listener(
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        ::streaming::listener::connection_callback on_connection,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {},
        uint16_t port = 0,
        ::streaming::listener::stream_transformer transform_stream = {});

    template<
        class Remote,
        class Local>
    rpc_factory<
        Remote,
        Local>
    fixed_factory(rpc::shared_ptr<Local> local_interface)
    {
        return [local_interface = std::move(local_interface)](
                   rpc::shared_ptr<Remote>,
                   std::shared_ptr<rpc::service>) mutable -> CORO_TASK(rpc::service_connect_result<Local>)
        { CO_RETURN rpc::service_connect_result<Local>{rpc::error::OK(), local_interface}; };
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc_stream(
        rpc::shared_ptr<In> input_interface,
        std::shared_ptr<::streaming::stream> stream,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        auto transport_result = make_client_rpc_stream_transport(std::move(stream), settings, std::move(service));
        if (transport_result.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{transport_result.error_code, {}};
        CO_RETURN CO_AWAIT transport_result.service->template connect_to_zone<In, Out>(
            service_proxy_name(settings.transport, "main child"),
            std::move(transport_result.transport),
            std::move(input_interface));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc_stream(
        rpc::shared_ptr<In> input_interface,
        std::shared_ptr<::streaming::stream> stream,
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_rpc_stream<In, Out>(
            std::move(input_interface), std::move(stream), make_connection_settings(settings), std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(listener_result)
    accept_rpc_listener(
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        rpc_factory<
            Remote,
            Local> factory,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {},
        uint16_t port = 0,
        rpc_transport_observer observe_transport = {},
        ::streaming::listener::stream_transformer transform_stream = {})
    {
        const auto stream_options = transport_options(settings.transport);
        CO_RETURN CO_AWAIT start_rpc_listener(
            std::move(acceptor),
            [factory = std::move(factory), stream_options, observe_transport = std::move(observe_transport)](
                const std::string& name,
                std::shared_ptr<rpc::service> svc,
                std::shared_ptr<::streaming::stream> stream) mutable -> CORO_TASK(void)
            {
                auto transport = rpc::stream_transport::create<Remote, Local>(
                    name, std::move(svc), std::move(stream), factory, stream_options);
                if (observe_transport)
                    observe_transport(transport);
                CO_RETURN;
            },
            settings,
            std::move(service),
            std::move(owner),
            port,
            std::move(transform_stream));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(listener_result)
    accept_rpc_listener(
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        rpc_factory<
            Remote,
            Local> factory,
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {},
        uint16_t port = 0,
        rpc_transport_observer observe_transport = {},
        ::streaming::listener::stream_transformer transform_stream = {})
    {
        CO_RETURN CO_AWAIT accept_rpc_listener<Remote, Local>(
            std::move(acceptor),
            std::move(factory),
            make_connection_settings(settings),
            std::move(service),
            std::move(owner),
            port,
            std::move(observe_transport),
            std::move(transform_stream));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(listener_result)
    accept_rpc_listener(
        rpc::shared_ptr<Local> local_interface,
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {},
        uint16_t port = 0,
        rpc_transport_observer observe_transport = {},
        ::streaming::listener::stream_transformer transform_stream = {})
    {
        CO_RETURN CO_AWAIT accept_rpc_listener<Remote, Local>(
            std::move(acceptor),
            fixed_factory<Remote, Local>(std::move(local_interface)),
            settings,
            std::move(service),
            std::move(owner),
            port,
            std::move(observe_transport),
            std::move(transform_stream));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc_accept_result)
    accept_rpc_stream(
        std::shared_ptr<::streaming::stream> stream,
        rpc_factory<
            Remote,
            Local> factory,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {})
    {
        auto resolved_service = ensure_service(settings, std::move(service), "rpc_accept_service");
        if (!resolved_service)
            CO_RETURN rpc_accept_result{rpc::error::INVALID_DATA(), {}};
        auto transport = std::static_pointer_cast<rpc::stream_transport::transport>(
            CO_AWAIT resolved_service->template make_acceptor<Remote, Local>(
                transport_name(settings.transport, "responder_transport"),
                rpc::stream_transport::transport_factory(std::move(stream), transport_options(settings.transport)),
                std::move(factory)));

        if (!transport)
            CO_RETURN rpc_accept_result{rpc::error::TRANSPORT_ERROR(), {}};

        const auto error_code = CO_AWAIT transport->accept();
        if (error_code != rpc::error::OK())
            CO_RETURN rpc_accept_result{error_code, {}};

        CO_RETURN rpc_accept_result{rpc::error::OK(),
            std::make_shared<rpc_connection_handle>(std::move(resolved_service), std::move(transport), std::move(owner))};
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc_accept_result)
    accept_rpc_stream(
        rpc::shared_ptr<Local> local_interface,
        std::shared_ptr<::streaming::stream> stream,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        std::shared_ptr<void> owner = {})
    {
        CO_RETURN CO_AWAIT accept_rpc_stream<Remote, Local>(
            std::move(stream),
            fixed_factory<Remote, Local>(std::move(local_interface)),
            settings,
            std::move(service),
            std::move(owner));
    }
} // namespace rpc::stream_transport
