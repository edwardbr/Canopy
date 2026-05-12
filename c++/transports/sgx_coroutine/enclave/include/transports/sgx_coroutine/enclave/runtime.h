/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <io_uring/controller.h>
#include <memory>
#include <mutex>
#include <new>
#include <rpc/rpc.h>
#include <streaming/stream.h>
#include <string>
#include <transports/sgx_coroutine/enclave/io_uring_controller.h>
#include <transports/sgx_coroutine/enclave/service.h>
#include <transports/sgx_coroutine/enclave/host_transport.h>
#include <transports/streaming/transport.h>
#include <utility>

namespace rpc::sgx::coro::enclave
{
    using acceptor_factory = rpc::connection_handler;

    void register_acceptor_factory(acceptor_factory factory);
    void mark_runtime_connection_established();
    uint64_t runtime_ticks_per_millisecond() noexcept;
    uint64_t read_runtime_tick_counter() noexcept;
    uint64_t runtime_unix_epoch_milliseconds() noexcept;
    uint64_t runtime_ticks_to_microseconds(uint64_t ticks) noexcept;
    uint64_t runtime_ticks_to_nanoseconds(uint64_t ticks) noexcept;

    struct runtime_io_uring_controller_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc::io_uring::controller> controller;
    };

    CORO_TASK(runtime_io_uring_controller_result)
    get_or_create_runtime_io_uring_controller(
        std::shared_ptr<host_transport> transport,
        rpc::coro::scheduler* scheduler);

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::remote_object_result)
    create_child_enclave_zone(
        const char* name,
        std::shared_ptr<rpc::transport> parent_transport,
        rpc::connection_settings input_descr,
        std::function<CORO_TASK(rpc::service_connect_result<Local>)(
            rpc::shared_ptr<Remote>,
            std::shared_ptr<rpc::service>)> factory,
        std::shared_ptr<rpc::service> service)
    {
        (void)name;
        rpc::remote_object_result result{rpc::error::OK(), {}};
        if (input_descr.inbound_interface_id != rpc::sgx::coro::protocol::i_io_uring_control::get_id(rpc::get_version()))
        {
            RPC_ERROR("create_child_enclave_zone inbound interface id does not match i_io_uring_control");
            result.error_code = rpc::error::INVALID_INTERFACE_ID();
            CO_RETURN result;
        }
        if (input_descr.outbound_interface_id != Local::get_id(rpc::get_version()))
        {
            RPC_ERROR("create_child_enclave_zone outbound interface id does not match requested local interface");
            result.error_code = rpc::error::INVALID_INTERFACE_ID();
            CO_RETURN result;
        }
        if (!parent_transport || !factory)
        {
            result.error_code = rpc::error::INVALID_DATA();
            CO_RETURN result;
        }
        auto host_transport = std::dynamic_pointer_cast<rpc::sgx::coro::enclave::host_transport>(parent_transport);
        if (!host_transport)
        {
            result.error_code = rpc::error::INVALID_CAST();
            CO_RETURN result;
        }

        auto enclave_svc = std::dynamic_pointer_cast<rpc::enclave_service>(service);
        if (!enclave_svc)
        {
            result.error_code = rpc::error::INVALID_CAST();
            CO_RETURN result;
        }

        auto scheduler = enclave_svc->get_scheduler();
        if (!scheduler)
        {
            result.error_code = rpc::error::INVALID_DATA();
            CO_RETURN result;
        }

        auto adjacent_zone_id = parent_transport->get_adjacent_zone_id();

        // The runtime creates the single enclave_service and binds it to the
        // host transport before any connection request is dispatched. This
        // function only completes the normal child-zone wiring for the first
        // service connection coming from the host.
        enclave_svc->add_transport(input_descr.remote_object_id.as_zone(), parent_transport);
        rpc::transport_keep_alive ka(parent_transport, input_descr.remote_object_id.as_zone());

        rpc::transport_keep_alive adjacent_ka;
        if (input_descr.remote_object_id != adjacent_zone_id)
        {
            enclave_svc->add_transport(adjacent_zone_id, parent_transport);
            adjacent_ka = rpc::transport_keep_alive(parent_transport, adjacent_zone_id);
        }

        if (input_descr.get_object_id() == 0)
        {
            result.error_code = rpc::error::INVALID_DATA();
            CO_RETURN result;
        }

        auto parent_service_proxy
            = rpc::service_proxy::create("parent", enclave_svc, parent_transport, input_descr.remote_object_id.as_zone());
        enclave_svc->add_parent_zone_proxy(parent_service_proxy);

        bool new_proxy_added = true;
        auto proxy_result = CO_AWAIT parent_service_proxy->get_or_create_object_proxy(
            input_descr.get_object_id(),
            rpc::service_proxy::object_proxy_creation_rule::ADD_REF_IF_NEW,
            new_proxy_added,
            {adjacent_zone_id.get_address()},
            false);
        if (proxy_result.error_code != rpc::error::OK())
        {
            result.error_code = proxy_result.error_code;
            CO_RETURN result;
        }
        auto object_proxy = std::move(proxy_result.proxy);
        if (!object_proxy)
        {
            result.error_code = rpc::error::OBJECT_NOT_FOUND();
            CO_RETURN result;
        }

        auto control_query
            = CO_AWAIT object_proxy->template query_interface<rpc::sgx::coro::protocol::i_io_uring_control>(false);
        if (control_query.error_code != rpc::error::OK())
        {
            result.error_code = control_query.error_code;
            CO_RETURN result;
        }
        auto control = std::move(control_query.iface);
        if (!control)
        {
            result.error_code = rpc::error::INVALID_CAST();
            CO_RETURN result;
        }

        auto retain_error = CO_AWAIT host_transport->retain_io_uring_control_reference(control);
        if (retain_error != rpc::error::OK())
        {
            control = nullptr;
            result.error_code = retain_error;
            CO_RETURN result;
        }

        auto controller_result = CO_AWAIT get_or_create_runtime_io_uring_controller(host_transport, scheduler.get());
        if (controller_result.error_code != rpc::error::OK())
        {
            host_transport->release_io_uring_control_reference();
            control = nullptr;
            result.error_code = controller_result.error_code;
            CO_RETURN result;
        }
        auto controller = std::move(controller_result.controller);
        enclave_svc->set_io_uring_controller(controller);

        auto shutdown_controller = [&]()
        {
            if (controller)
                controller->request_shutdown();
            controller.reset();
        };

        rpc::shared_ptr<rpc::i_noop> transferred_interface;
        auto transfer_error = CO_AWAIT control->transfer_encapsulated_interface(transferred_interface);
        if (transfer_error != rpc::error::OK())
        {
            shutdown_controller();
            control = nullptr;
            result.error_code = transfer_error;
            CO_RETURN result;
        }

        rpc::shared_ptr<Remote> remote;
        if (transferred_interface)
        {
            remote = CO_AWAIT rpc::dynamic_pointer_cast<Remote>(std::move(transferred_interface));
            if (!remote)
            {
                shutdown_controller();
                control = nullptr;
                result.error_code = rpc::error::INVALID_CAST();
                CO_RETURN result;
            }
        }
        // OBJECT_NOT_FOUND means the host deliberately connected without a
        // user interface. Preserve the normal connect_to_zone nullable-input
        // behaviour and let the registered factory decide whether nullptr is
        // acceptable for this enclave entry point.

        auto child_result = CO_AWAIT factory(std::move(remote), std::static_pointer_cast<rpc::service>(enclave_svc));
        if (child_result.error_code != rpc::error::OK())
        {
            shutdown_controller();
            control = nullptr;
            result.error_code = child_result.error_code;
            CO_RETURN result;
        }

        control = nullptr;

        auto child_ptr = std::move(child_result.output_interface);
        if (!child_ptr)
        {
            mark_runtime_connection_established();
            CO_RETURN result;
        }

        auto bind_result
            = CO_AWAIT rpc::stub_bind_out_param(enclave_svc, rpc::get_version(), adjacent_zone_id, 0, child_ptr);
        result.error_code = bind_result.error_code;
        result.descriptor = bind_result.descriptor;
        if (result.error_code == rpc::error::OK())
            mark_runtime_connection_established();
        CO_RETURN result;
    }

    template<
        class Remote,
        class Local>
    void register_connection_factory(
        const char* name,
        std::function<CORO_TASK(rpc::service_connect_result<Local>)(
            rpc::shared_ptr<Remote>,
            std::shared_ptr<rpc::service>)> factory)
    {
        register_acceptor_factory(
            [name = std::string(name), factory = std::move(factory)](
                rpc::connection_settings input,
                std::shared_ptr<rpc::service> svc,
                std::shared_ptr<rpc::transport> transport) -> CORO_TASK(rpc::connection_handler_result)
            {
                auto result = CO_AWAIT create_child_enclave_zone<Remote, Local>(
                    name.c_str(), std::move(transport), std::move(input), factory, std::move(svc));
                CO_RETURN rpc::connection_handler_result{result.error_code, std::move(result.descriptor)};
            });
    }
}
