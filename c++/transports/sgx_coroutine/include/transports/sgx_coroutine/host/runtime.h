/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <functional>
#include <memory>
#include <rpc/rpc.h>
#include <streaming/stream.h>
#include <string>
#include <transports/streaming/transport.h>

namespace rpc::sgx::coro::host
{
    using acceptor_factory = std::function<std::shared_ptr<rpc::stream_transport::transport>(
        const std::string&, const std::shared_ptr<rpc::root_service>&, std::shared_ptr<streaming::stream>)>;
    using runtime_cleanup_handler = std::function<void()>;

    void register_connection_handler(rpc::connection_handler handler);
    rpc::connection_handler get_connection_handler();
    void register_acceptor_factory(acceptor_factory factory);
    acceptor_factory get_acceptor_factory();
    // Registers enclave-scoped cleanup work that must run before the final
    // scheduler drain. This is intended for shared enclave resources such as a
    // single io_uring controller used by multiple child zones.
    int register_runtime_cleanup_handler(runtime_cleanup_handler handler);

    template<
        class Remote,
        class Local>
    void register_connection_factory(
        const char* name,
        std::function<CORO_TASK(rpc::service_connect_result<Local>)(
            rpc::shared_ptr<Remote>,
            std::shared_ptr<rpc::service>)> factory)
    {
        register_connection_handler(rpc::make_new_zone_connection_handler<Remote, Local>(name, factory));
        register_acceptor_factory(
            [name = std::string(name), factory = std::move(factory)](
                const std::string&,
                const std::shared_ptr<rpc::root_service>& service,
                std::shared_ptr<streaming::stream> stream) -> std::shared_ptr<rpc::stream_transport::transport>
            {
                auto handler = rpc::connection_handler(
                    [name, factory](
                        rpc::connection_settings input,
                        std::shared_ptr<rpc::service> svc,
                        std::shared_ptr<rpc::transport> transport) -> CORO_TASK(rpc::connection_handler_result)
                    {
                        auto child_factory = std::function<CORO_TASK(rpc::service_connect_result<Local>)(
                            rpc::shared_ptr<Remote>, std::shared_ptr<rpc::child_service>)>(
                            [factory](rpc::shared_ptr<Remote> remote, std::shared_ptr<rpc::child_service> child_service)
                                -> CORO_TASK(rpc::service_connect_result<Local>)
                            {
                                CO_RETURN CO_AWAIT factory(
                                    std::move(remote), std::static_pointer_cast<rpc::service>(std::move(child_service)));
                            });

                        auto result = CO_AWAIT rpc::child_service::create_child_zone<Remote, Local>(
                            name.c_str(),
                            std::move(transport),
                            std::move(input),
                            std::move(child_factory),
                            svc->get_scheduler());
                        CO_RETURN rpc::connection_handler_result{result.error_code, std::move(result.descriptor)};
                    });
                return rpc::stream_transport::make_server(name, service, std::move(stream), std::move(handler));
            });
    }
}
