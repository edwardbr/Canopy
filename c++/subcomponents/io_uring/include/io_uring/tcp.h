/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <utility>

#include <io_uring/controller.h>
#include <io_uring/direct_descriptor.h>

namespace rpc::io_uring
{
    class acceptor
    {
    public:
        explicit acceptor(std::shared_ptr<controller> controller) noexcept
            : controller_(std::move(controller))
        {
        }

        CORO_TASK(int)
        listen_loopback(
            uint16_t port,
            uint32_t backlog = 16)
        {
            if (!controller_)
            {
                CO_RETURN rpc::error::RESOURCE_CLOSED();
            }

            if (listen_descriptor_)
            {
                CO_AWAIT close();
            }

            auto socket_result = CO_AWAIT controller_->create_tcp_socket();
            if (socket_result.error_code != rpc::error::OK())
            {
                CO_RETURN socket_result.error_code;
            }

            try
            {
                listen_descriptor_ = std::make_shared<direct_descriptor>(controller_, socket_result.descriptor);
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while creating direct io_uring listen descriptor");
                std::terminate();
            }

            port_ = port;
            auto bind_result = CO_AWAIT controller_->bind_tcp_ipv4_loopback(listen_descriptor_->get(), port);
            if (bind_result.error_code != rpc::error::OK())
            {
                CO_AWAIT close();
                CO_RETURN bind_result.error_code;
            }

            auto listen_result = CO_AWAIT controller_->listen(listen_descriptor_->get(), backlog);
            if (listen_result.error_code != rpc::error::OK())
            {
                CO_AWAIT close();
                CO_RETURN listen_result.error_code;
            }

            listening_ = true;
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(direct_descriptor_result) accept_with_result()
        {
            auto listen_descriptor = listen_descriptor_;
            if (!controller_ || !listening_ || !listen_descriptor || !listen_descriptor->is_open())
            {
                CO_RETURN direct_descriptor_result{rpc::error::RESOURCE_CLOSED(), 0, 0, {}};
            }

            auto accept_result = CO_AWAIT controller_->accept(listen_descriptor->get());
            if (accept_result.error_code != rpc::error::OK())
            {
                CO_RETURN direct_descriptor_result{
                    accept_result.error_code, accept_result.native_result, accept_result.cqe_flags, {}};
            }

            std::shared_ptr<direct_descriptor> accepted_descriptor;
            try
            {
                accepted_descriptor = std::make_shared<direct_descriptor>(controller_, accept_result.descriptor);
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while creating accepted direct io_uring descriptor");
                std::terminate();
            }

            CO_RETURN direct_descriptor_result{
                rpc::error::OK(), accept_result.native_result, accept_result.cqe_flags, std::move(accepted_descriptor)};
        }

        CORO_TASK(std::shared_ptr<direct_descriptor>) accept()
        {
            auto result = CO_AWAIT accept_with_result();
            CO_RETURN std::move(result.descriptor);
        }

        CORO_TASK(void) close()
        {
            auto listen_descriptor = std::move(listen_descriptor_);
            if (listen_descriptor)
            {
                CO_AWAIT listen_descriptor->close();
            }
            listening_ = false;
            CO_RETURN;
        }

        [[nodiscard]] uint16_t port() const noexcept { return port_; }
        [[nodiscard]] bool is_listening() const noexcept
        {
            auto listen_descriptor = listen_descriptor_;
            return listening_ && listen_descriptor && listen_descriptor->is_open();
        }

    private:
        std::shared_ptr<controller> controller_;
        std::shared_ptr<direct_descriptor> listen_descriptor_;
        uint16_t port_{0};
        bool listening_{false};
    };

    class connector
    {
    public:
        explicit connector(std::shared_ptr<controller> controller) noexcept
            : controller_(std::move(controller))
        {
        }

        CORO_TASK(direct_descriptor_result) connect_loopback_with_result(uint16_t port)
        {
            if (!controller_)
            {
                CO_RETURN direct_descriptor_result{rpc::error::RESOURCE_CLOSED(), 0, 0, {}};
            }

            auto connect_result = CO_AWAIT controller_->connect_tcp_ipv4_loopback(port);
            if (connect_result.error_code != rpc::error::OK())
            {
                CO_RETURN direct_descriptor_result{
                    connect_result.error_code, connect_result.native_result, connect_result.cqe_flags, {}};
            }

            std::shared_ptr<direct_descriptor> descriptor;
            try
            {
                descriptor = std::make_shared<direct_descriptor>(controller_, connect_result.descriptor);
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while creating connected direct io_uring descriptor");
                std::terminate();
            }

            CO_RETURN direct_descriptor_result{
                rpc::error::OK(), connect_result.native_result, connect_result.cqe_flags, std::move(descriptor)};
        }

        CORO_TASK(std::shared_ptr<direct_descriptor>) connect_loopback(uint16_t port)
        {
            auto result = CO_AWAIT connect_loopback_with_result(port);
            CO_RETURN std::move(result.descriptor);
        }

    private:
        std::shared_ptr<controller> controller_;
    };

} // namespace rpc::io_uring
