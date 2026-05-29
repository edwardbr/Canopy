/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include <io_uring/host_io_uring.h>
#include <connection_factory/io_uring_options.h>
#include <connection_factory/stream_rpc.h>
#include <streaming/io_uring/acceptor.h>
#include <streaming/io_uring/connector.h>

namespace rpc::io_uring
{
    // High-level stream/RPC factories for io_uring. JSON option normalisation
    // lives in connection_factory/io_uring_options.h so direct controller/stream
    // code can use typed io_uring options without pulling factory mechanics into
    // the same header.

    struct runtime
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc::io_uring::io_uring_scheduler> scheduler_owner;
        std::shared_ptr<rpc::io_uring::controller> controller;
    };

    inline runtime make_runtime(
        rpc::io_uring::host_controller::options controller_options = {},
        std::shared_ptr<rpc::service> service = {})
    {
        const bool borrowed_scheduler = service && service->get_executor();
        auto scheduler = borrowed_scheduler ? service->get_executor() : rpc::connection_factory::make_default_executor();
        if (!scheduler)
            return {rpc::error::TRANSPORT_ERROR(), {}, {}};

        if (borrowed_scheduler)
        {
            std::shared_ptr<rpc::io_uring::linux_io_uring_handle> handle;
            auto error_code = rpc::io_uring::linux_io_uring_handle::create(handle, controller_options, scheduler);
            if (error_code != rpc::error::OK())
                return {error_code, {}, {}};

            auto controller = std::make_shared<rpc::io_uring::controller>(
                std::move(handle), scheduler.get(), rpc::io_uring::default_controller_options());
            return {rpc::error::OK(), {}, std::move(controller)};
        }

        std::shared_ptr<rpc::io_uring::io_uring_scheduler> owner;
        auto error_code = rpc::io_uring::create_scheduler(owner, controller_options, scheduler);
        if (error_code != rpc::error::OK())
            return {error_code, {}, {}};

        auto controller = owner ? owner->get_controller() : nullptr;
        if (!controller)
            return {rpc::error::TRANSPORT_ERROR(), {}, {}};

        return {rpc::error::OK(), std::move(owner), std::move(controller)};
    }

    struct loopback_listen_result
    {
        int error_code{rpc::error::OK()};
        uint16_t port{0};
    };

    inline CORO_TASK(loopback_listen_result) listen_loopback(
        const std::shared_ptr<::streaming::io_uring::acceptor>& acceptor,
        loopback_listen_options listen_options)
    {
        if (listen_options.port != 0)
        {
            const auto listen_error = CO_AWAIT acceptor->listen_loopback(listen_options.port);
            CO_RETURN loopback_listen_result{listen_error, listen_options.port};
        }

        if (listen_options.port_range.first_port >= listen_options.port_range.last_port)
            CO_RETURN loopback_listen_result{rpc::error::INVALID_DATA(), 0};

        int listen_error = rpc::error::OK();
        for (uint16_t candidate = listen_options.port_range.first_port; candidate < listen_options.port_range.last_port;
            ++candidate)
        {
            listen_error = CO_AWAIT acceptor->listen_loopback(candidate);
            if (listen_error == rpc::error::OK())
                CO_RETURN loopback_listen_result{rpc::error::OK(), candidate};
        }

        CO_RETURN loopback_listen_result{listen_error, 0};
    }

    inline CORO_TASK(rpc::connection_factory::stream_result) connect_stream(
        uint16_t port,
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::io_uring::stream::options stream_options = ::streaming::io_uring::default_stream_options(),
        std::shared_ptr<rpc::service> service = {})
    {
        if (port == 0)
            CO_RETURN rpc::connection_factory::stream_result{rpc::error::INVALID_DATA(), {}};

        auto runtime = make_runtime(controller_options, std::move(service));
        if (runtime.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::stream_result{runtime.error_code, {}};

        auto result = CO_AWAIT ::streaming::io_uring::connect_loopback(runtime.controller, port, stream_options);
        if (result.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::stream_result{result.error_code, {}, result.native_result};

        CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(),
            rpc::connection_factory::keep_owner(std::move(result.connection), std::move(runtime.scheduler_owner)),
            result.native_result};
    }

    inline CORO_TASK(rpc::connection_factory::stream_result) connect_stream(
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        const auto runtime_options = detail::make_io_uring_runtime_options(options);
        CO_RETURN CO_AWAIT connect_stream(
            runtime_options.port, runtime_options.controller_options, runtime_options.stream_options, std::move(service));
    }

    inline CORO_TASK(rpc::connection_factory::stream_accept_result) accept_stream(
        rpc::connection_factory::stream_callback callback,
        loopback_listen_options listen_options = {},
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::io_uring::stream::options stream_options = ::streaming::io_uring::default_stream_options(),
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service = rpc::connection_factory::ensure_service(
            rpc::connection_factory_config::stream_factory_options{}, std::move(service), "io_uring_stream_accept");
        auto runtime = make_runtime(controller_options, resolved_service);
        if (runtime.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::stream_accept_result{runtime.error_code, {}};

        auto acceptor = std::make_shared<::streaming::io_uring::acceptor>(runtime.controller, stream_options);

        const auto listen_result = CO_AWAIT listen_loopback(acceptor, listen_options);

        if (listen_result.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::stream_accept_result{listen_result.error_code, {}};
        if (listen_result.port == 0)
            CO_RETURN rpc::connection_factory::stream_accept_result{rpc::error::TRANSPORT_ERROR(), {}};

        CO_RETURN rpc::connection_factory::accept_streams(
            acceptor,
            std::move(callback),
            rpc::connection_factory_config::stream_factory_options{},
            std::move(resolved_service),
            std::move(runtime.scheduler_owner),
            listen_result.port);
    }

    inline CORO_TASK(rpc::connection_factory::stream_accept_result) accept_stream(
        rpc::connection_factory::stream_callback callback,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        auto runtime_options = detail::make_io_uring_runtime_options(options);
        auto resolved_service = rpc::connection_factory::ensure_service(
            runtime_options.factory_options, std::move(service), "io_uring_stream_accept");
        if (!resolved_service)
            CO_RETURN rpc::connection_factory::stream_accept_result{rpc::error::INVALID_DATA(), {}};

        auto runtime = make_runtime(runtime_options.controller_options, resolved_service);
        if (runtime.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::stream_accept_result{runtime.error_code, {}};

        auto acceptor
            = std::make_shared<::streaming::io_uring::acceptor>(runtime.controller, runtime_options.stream_options);

        const auto listen_result = CO_AWAIT listen_loopback(acceptor, runtime_options.listen_options);
        if (listen_result.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::stream_accept_result{listen_result.error_code, {}};
        if (listen_result.port == 0)
            CO_RETURN rpc::connection_factory::stream_accept_result{rpc::error::TRANSPORT_ERROR(), {}};

        CO_RETURN rpc::connection_factory::accept_streams(
            acceptor,
            std::move(callback),
            runtime_options.factory_options,
            std::move(resolved_service),
            std::move(runtime.scheduler_owner),
            listen_result.port);
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        uint16_t port,
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::io_uring::stream::options stream_options = ::streaming::io_uring::default_stream_options(),
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service = rpc::connection_factory::ensure_service(
            rpc::connection_factory_config::stream_factory_options{}, std::move(service), "io_uring_rpc_client");
        auto stream = CO_AWAIT connect_stream(port, controller_options, stream_options, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};
        CO_RETURN CO_AWAIT rpc::connection_factory::connect_rpc_stream<In, Out>(
            std::move(input_interface),
            std::move(stream.stream),
            rpc::connection_factory_config::stream_factory_options{},
            std::move(resolved_service));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        auto runtime_options = detail::make_io_uring_runtime_options(options);
        auto resolved_service = rpc::connection_factory::ensure_service(
            runtime_options.factory_options, std::move(service), "io_uring_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        auto stream = CO_AWAIT connect_stream(
            runtime_options.port, runtime_options.controller_options, runtime_options.stream_options, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};

        CO_RETURN CO_AWAIT rpc::connection_factory::connect_rpc_stream<In, Out>(
            std::move(input_interface), std::move(stream.stream), runtime_options.factory_options, std::move(resolved_service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::connection_factory::rpc_factory<
            Remote,
            Local> factory,
        loopback_listen_options listen_options,
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::io_uring::stream::options stream_options = ::streaming::io_uring::default_stream_options(),
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        auto resolved_service = rpc::connection_factory::ensure_service(
            rpc::connection_factory_config::stream_factory_options{}, std::move(service), "io_uring_rpc_accept");
        auto runtime = make_runtime(controller_options, resolved_service);
        if (runtime.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::listener_result{runtime.error_code, {}};

        auto acceptor = std::make_shared<::streaming::io_uring::acceptor>(runtime.controller, stream_options);

        const auto listen_result = CO_AWAIT listen_loopback(acceptor, listen_options);
        if (listen_result.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::listener_result{listen_result.error_code, {}};
        if (listen_result.port == 0)
            CO_RETURN rpc::connection_factory::listener_result{rpc::error::TRANSPORT_ERROR(), {}};

        CO_RETURN CO_AWAIT rpc::connection_factory::accept_rpc_listener<Remote, Local>(
            acceptor,
            std::move(factory),
            rpc::connection_factory_config::stream_factory_options{},
            std::move(resolved_service),
            std::move(runtime.scheduler_owner),
            listen_result.port,
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::connection_factory::rpc_factory<
            Remote,
            Local> factory,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        auto runtime_options = detail::make_io_uring_runtime_options(options);
        auto resolved_service = rpc::connection_factory::ensure_service(
            runtime_options.factory_options, std::move(service), "io_uring_rpc_accept");
        if (!resolved_service)
            CO_RETURN rpc::connection_factory::listener_result{rpc::error::INVALID_DATA(), {}};

        auto runtime = make_runtime(runtime_options.controller_options, resolved_service);
        if (runtime.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::listener_result{runtime.error_code, {}};

        auto acceptor
            = std::make_shared<::streaming::io_uring::acceptor>(runtime.controller, runtime_options.stream_options);

        const auto listen_result = CO_AWAIT listen_loopback(acceptor, runtime_options.listen_options);
        if (listen_result.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::listener_result{listen_result.error_code, {}};
        if (listen_result.port == 0)
            CO_RETURN rpc::connection_factory::listener_result{rpc::error::TRANSPORT_ERROR(), {}};

        CO_RETURN CO_AWAIT rpc::connection_factory::accept_rpc_listener<Remote, Local>(
            acceptor,
            std::move(factory),
            runtime_options.factory_options,
            std::move(resolved_service),
            std::move(runtime.scheduler_owner),
            listen_result.port,
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        loopback_listen_options listen_options,
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::io_uring::stream::options stream_options = ::streaming::io_uring::default_stream_options(),
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::connection_factory::fixed_factory<Remote, Local>(std::move(local_interface)),
            listen_options,
            controller_options,
            stream_options,
            std::move(service),
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::connection_factory::fixed_factory<Remote, Local>(std::move(local_interface)),
            options,
            std::move(service),
            std::move(observe_transport));
    }

} // namespace rpc::io_uring
