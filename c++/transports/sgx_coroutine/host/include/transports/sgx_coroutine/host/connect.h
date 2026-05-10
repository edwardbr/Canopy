/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <io_uring/host_controller.h>
#include <rpc/rpc.h>
#include <transports/sgx_coroutine/common/io_uring_data_conversion.h>

#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace rpc::sgx::coro::host
{
    namespace detail
    {
        class enclave_io_uring_control
            : public rpc::base<enclave_io_uring_control, rpc::sgx::coro::protocol::i_io_uring_control>
        {
        public:
            enclave_io_uring_control(
                std::unique_ptr<rpc::io_uring::host_controller> controller,
                rpc::shared_ptr<rpc::i_noop> encapsulated_interface) noexcept
                : controller_(std::move(controller))
                , encapsulated_interface_(std::move(encapsulated_interface))
            {
            }

            CORO_TASK(int) transfer_encapsulated_interface(rpc::shared_ptr<rpc::i_noop>& iface) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (encapsulated_interface_)
                {
                    // This object is a bootstrap envelope. Once the enclave has
                    // received the user's real interface, the envelope must stop
                    // owning it so long-running host lifetime is carried by the
                    // normal RPC reference path instead.
                    iface = std::move(encapsulated_interface_);
                }
                CO_RETURN rpc::error::OK();
            }

            CORO_TASK(int) wake_iouring() override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!controller_)
                    CO_RETURN rpc::error::RESOURCE_CLOSED();

                CO_RETURN controller_->wake_iouring();
            }

            CORO_TASK(int) get_iouring_data(rpc::sgx::coro::protocol::io_uring_data& ring_data) override
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!controller_)
                    CO_RETURN rpc::error::RESOURCE_CLOSED();

                rpc::io_uring::data native_data;
                auto err = controller_->get_iouring_data(native_data);
                if (err == rpc::error::OK())
                    rpc::sgx::coro::protocol::copy_to_wire(native_data, ring_data);
                CO_RETURN err;
            }

        private:
            std::mutex mutex_;
            std::unique_ptr<rpc::io_uring::host_controller> controller_;
            rpc::shared_ptr<rpc::i_noop> encapsulated_interface_;
        };
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::service_connect_result<Local>)
    connect_to_enclave_zone(
        const std::shared_ptr<rpc::service>& service,
        const char* name,
        std::shared_ptr<rpc::transport> enclave_transport,
        rpc::shared_ptr<Remote> input_interface,
        rpc::io_uring::host_controller::options controller_options = {})
    {
        rpc::service_connect_result<Local> result{rpc::error::OK(), {}};
        if (!service || !enclave_transport)
        {
            result.error_code = rpc::error::INVALID_DATA();
            CO_RETURN result;
        }

        rpc::shared_ptr<rpc::i_noop> erased_interface;
        if (input_interface)
        {
            erased_interface = CO_AWAIT rpc::dynamic_pointer_cast<rpc::i_noop>(input_interface);
            if (!erased_interface)
            {
                result.error_code = rpc::error::INVALID_CAST();
                CO_RETURN result;
            }
        }

        std::unique_ptr<rpc::io_uring::host_controller> controller;
        auto controller_error
            = rpc::io_uring::host_controller::create(controller, controller_options, service->get_scheduler());
        if (controller_error != rpc::error::OK())
        {
            result.error_code = controller_error;
            CO_RETURN result;
        }

        rpc::shared_ptr<rpc::sgx::coro::protocol::i_io_uring_control> control;
        try
        {
            control = rpc::shared_ptr<rpc::sgx::coro::protocol::i_io_uring_control>(
                new detail::enclave_io_uring_control(std::move(controller), std::move(erased_interface)));
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating enclave io_uring control interface");
            std::terminate();
        }

        CO_RETURN CO_AWAIT service->template connect_to_zone<rpc::sgx::coro::protocol::i_io_uring_control, Local>(
            name, std::move(enclave_transport), std::move(control));
    }
}
