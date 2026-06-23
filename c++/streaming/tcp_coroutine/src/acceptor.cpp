/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/tcp_coroutine/acceptor.h>

#include <streaming/tcp_coroutine/stream.h>

#include <exception>
#include <new>
#include <utility>

namespace streaming::coroutine::tcp
{
    acceptor::acceptor(
        std::shared_ptr<rpc::io_uring::controller> controller,
        stream::options stream_options) noexcept
        : controller_(std::move(controller))
        , stream_options_(stream_options)
    {
        if (controller_)
        {
            try
            {
                acceptor_ = std::make_shared<rpc::io_uring::acceptor>(controller_);
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while creating TCP coroutine acceptor");
                std::terminate();
            }
        }
    }

    CORO_TASK(int)
    acceptor::listen_loopback(
        uint16_t port,
        uint32_t backlog)
    {
        CO_RETURN CO_AWAIT listen_ipv4({127, 0, 0, 1}, port, backlog);
    }

    CORO_TASK(int)
    acceptor::listen_ipv4(
        const std::array<
            uint8_t,
            4>& bind_address,
        uint16_t port,
        uint32_t backlog)
    {
        if (!acceptor_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        stopping_.store(false, std::memory_order_release);
        CO_RETURN CO_AWAIT acceptor_->listen_ipv4(bind_address, port, backlog);
    }

    CORO_TASK(int)
    acceptor::listen_ipv6(
        const std::array<
            uint8_t,
            16>& bind_address,
        uint16_t port,
        uint32_t backlog)
    {
        if (!acceptor_)
        {
            CO_RETURN rpc::error::RESOURCE_CLOSED();
        }

        stopping_.store(false, std::memory_order_release);
        CO_RETURN CO_AWAIT acceptor_->listen_ipv6(bind_address, port, backlog);
    }

    bool acceptor::init(std::shared_ptr<rpc::coro::scheduler> scheduler)
    {
        scheduler_ = std::move(scheduler);
        return is_listening();
    }

    CORO_TASK(std::optional<std::shared_ptr<streaming::stream>>) acceptor::accept()
    {
        while (!stopping_.load(std::memory_order_acquire))
        {
            auto low_level_acceptor = acceptor_;
            if (!low_level_acceptor || !low_level_acceptor->is_listening())
            {
                CO_RETURN std::nullopt;
            }

            auto result = CO_AWAIT low_level_acceptor->accept_with_result();
            if (stopping_.load(std::memory_order_acquire))
            {
                if (result.descriptor)
                {
                    CO_AWAIT result.descriptor->close();
                }
                CO_RETURN std::nullopt;
            }

            auto stream_result = make_stream_result(result, low_level_acceptor->port(), stream_options_, scheduler_);
            if (stream_result.error_code == rpc::error::OK() && stream_result.connection)
            {
                CO_RETURN std::optional<std::shared_ptr<streaming::stream>>{std::move(stream_result.connection)};
            }

            if (stream_result.error_code == rpc::error::RESOURCE_CLOSED())
            {
                CO_RETURN std::nullopt;
            }

            if (stream_result.error_code == rpc::error::CALL_TIMEOUT())
            {
                continue;
            }

            RPC_ERROR(
                "tcp_coroutine::acceptor accept failed error_code={} native_result={}",
                stream_result.error_code,
                stream_result.native_result);
            CO_RETURN std::nullopt;
        }

        CO_RETURN std::nullopt;
    }

    void acceptor::stop()
    {
        stopping_.store(true, std::memory_order_release);

        auto scheduler = scheduler_;
        auto low_level_acceptor = acceptor_;
        if (!scheduler || !low_level_acceptor || scheduler->is_shutdown())
        {
            return;
        }

        (void)scheduler->spawn_detached(close_acceptor(std::move(low_level_acceptor)));
    }

    uint16_t acceptor::port() const noexcept
    {
        return acceptor_ ? acceptor_->port() : 0;
    }

    bool acceptor::is_listening() const noexcept
    {
        return acceptor_ && acceptor_->is_listening();
    }

    CORO_TASK(void) acceptor::close_acceptor(std::shared_ptr<rpc::io_uring::acceptor> acceptor)
    {
        if (acceptor)
        {
            CO_AWAIT acceptor->close();
        }
        CO_RETURN;
    }
} // namespace streaming::coroutine::tcp
