/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/streaming/factory.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

#include <streaming/listener.h>
#include <streaming/stream.h>
#include <streaming/stream_acceptor.h>

namespace rpc::stream_transport
{
    namespace
    {
        class owning_stream final : public ::streaming::stream
        {
        public:
            owning_stream(
                std::shared_ptr<::streaming::stream> inner,
                std::shared_ptr<void> owner)
                : inner_(std::move(inner))
                , owner_(std::move(owner))
            {
            }

            auto receive(
                rpc::mutable_byte_span buffer,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
                -> CORO_TASK(::streaming::receive_result) override
            {
                CO_RETURN CO_AWAIT inner_->receive(buffer, timeout);
            }

            auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) override
            {
                CO_RETURN CO_AWAIT inner_->send(buffer);
            }

            [[nodiscard]] bool is_closed() const override { return inner_->is_closed(); }

            auto set_closed() -> CORO_TASK(void) override
            {
                CO_AWAIT inner_->set_closed();
                CO_RETURN;
            }

            [[nodiscard]] ::streaming::peer_info get_peer_info() const override { return inner_->get_peer_info(); }

        private:
            std::shared_ptr<::streaming::stream> inner_;
            std::shared_ptr<void> owner_;
        };
    } // namespace

    std::shared_ptr<::streaming::stream> keep_owner(
        std::shared_ptr<::streaming::stream> stream,
        std::shared_ptr<void> owner)
    {
        if (!owner)
            return stream;
        return std::make_shared<owning_stream>(std::move(stream), std::move(owner));
    }

    rpc::executor_ptr make_default_executor()
    {
#ifdef CANOPY_BUILD_COROUTINE
        auto options = rpc::coro::scheduler::options{};
        options.thread_strategy = rpc::coro::scheduler::thread_strategy_t::spawn;
        options.pool.thread_count = std::max(1U, std::thread::hardware_concurrency());
        return rpc::coro::make_shared_scheduler(options);
#else
        return std::make_shared<rpc::executor>();
#endif
    }

    std::optional<rpc::encoding> encoding_option(const transport_settings& settings)
    {
        if (!settings.encoding)
            return std::nullopt;
        if (settings.encoding.value() == rpc::encoding::not_set)
            return std::nullopt;
        return settings.encoding.value();
    }

    std::string configured_name(
        const rpc::optional<std::string>& configured,
        std::string fallback)
    {
        if (!configured)
            return fallback;
        return configured.value();
    }

    std::string service_name(
        const service_settings& settings,
        std::string fallback)
    {
        return configured_name(settings.name, std::move(fallback));
    }

    std::string transport_name(
        const transport_settings& settings,
        std::string fallback)
    {
        return configured_name(settings.name, std::move(fallback));
    }

    std::string service_proxy_name(
        const transport_settings& settings,
        std::string fallback)
    {
        if (settings.service_proxy_name)
            return settings.service_proxy_name.value();
        return configured_name(settings.name, std::move(fallback));
    }

    std::string listener_name(
        const listener_settings& settings,
        std::string fallback)
    {
        return configured_name(settings.name, std::move(fallback));
    }

    stream_transport_options transport_options(const transport_settings& settings)
    {
        stream_transport_options result;
        if (settings.call_timeout)
            result.call_timeout = std::chrono::milliseconds(settings.call_timeout.value());
        if (settings.call_timeout_sweep)
            result.call_timeout_sweep = std::chrono::milliseconds(settings.call_timeout_sweep.value());
        if (settings.shutdown_timeout)
            result.shutdown_timeout = std::chrono::milliseconds(settings.shutdown_timeout.value());
        return result;
    }

    int configure_service(
        const std::shared_ptr<rpc::service>& service,
        const transport_settings& settings)
    {
        if (!service)
            return rpc::error::OK();
        if (auto enc = encoding_option(settings))
            service->set_default_encoding(*enc);
        return rpc::error::OK();
    }

    std::shared_ptr<rpc::service> ensure_service(
        const service_settings& settings,
        const transport_settings& transport_settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name)
    {
        if (service)
        {
            configure_service(service, transport_settings);
            return service;
        }

        rpc::service_config config;
        const auto name = service_name(settings, std::move(default_name));
        auto created = rpc::root_service::create(name.c_str(), config, make_default_executor());
        configure_service(created, transport_settings);
        return created;
    }

    std::shared_ptr<rpc::service> ensure_service(
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name)
    {
        return ensure_service(settings.service, settings.transport, std::move(service), std::move(default_name));
    }

    std::shared_ptr<rpc::service> ensure_service(
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name)
    {
        return ensure_service({}, settings, std::move(service), std::move(default_name));
    }

    connection_settings make_connection_settings(
        transport_settings transport,
        service_settings service,
        listener_settings listener)
    {
        return {std::move(service), std::move(transport), std::move(listener)};
    }

    stream_accept_handle::stream_accept_handle(
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        std::shared_ptr<rpc::service> service,
        stream_callback callback,
        std::shared_ptr<void> owner,
        uint16_t port)
        : acceptor_(std::move(acceptor))
        , service_(std::move(service))
        , executor_(service_ ? service_->get_executor() : make_default_executor())
        , callback_(std::move(callback))
        , owner_(std::move(owner))
        , port_(port)
    {
    }

    bool stream_accept_handle::start()
    {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
            return false;

        stopping_.store(false, std::memory_order_release);
        stopped_.reset();

        if (!acceptor_ || !executor_ || !callback_ || !acceptor_->init(executor_))
        {
            running_.store(false, std::memory_order_release);
            stopped_.set();
            return false;
        }

        if (!executor_->SPAWN_DETACHED(run(shared_from_this())))
        {
            acceptor_->stop();
            running_.store(false, std::memory_order_release);
            stopped_.set();
            return false;
        }

        return true;
    }

    CORO_TASK(void) stream_accept_handle::stop()
    {
        if (!running_.load(std::memory_order_acquire))
        {
            stopped_.set();
            service_.reset();
            callback_ = {};
            CO_RETURN;
        }

        stopping_.store(true, std::memory_order_release);
        if (acceptor_)
            acceptor_->stop();

        CO_AWAIT stopped_.wait();
        service_.reset();
        acceptor_.reset();
        callback_ = {};
        owner_.reset();
        CO_RETURN;
    }

    uint16_t stream_accept_handle::port() const noexcept
    {
        return port_;
    }

    CORO_TASK(void) stream_accept_handle::run(std::shared_ptr<stream_accept_handle> self)
    {
        (void)self;

#ifdef CANOPY_BUILD_COROUTINE
        CO_AWAIT executor_->schedule();
#endif

        while (true)
        {
            auto maybe_stream = CO_AWAIT acceptor_->accept();
            if (!maybe_stream)
                break;

            auto stream = keep_owner(std::move(*maybe_stream), owner_);
            if (stopping_.load(std::memory_order_acquire))
            {
                CO_AWAIT stream->set_closed();
                break;
            }

            CO_AWAIT callback_(std::move(stream));
        }

        running_.store(false, std::memory_order_release);
        stopped_.set();
        CO_RETURN;
    }

    stream_accept_result accept_streams(
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        stream_callback callback,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<void> owner,
        uint16_t port)
    {
        if (!acceptor || !callback)
            return {rpc::error::INVALID_DATA(), {}};

        auto resolved_service = ensure_service(settings, std::move(service), "stream_accept_service");
        if (!resolved_service)
            return {rpc::error::INVALID_DATA(), {}};

        auto handle = std::make_shared<stream_accept_handle>(
            std::move(acceptor), std::move(resolved_service), std::move(callback), std::move(owner), port);
        if (!handle->start())
            return {rpc::error::TRANSPORT_ERROR(), {}};

        return {rpc::error::OK(), std::move(handle)};
    }

    listener_handle::listener_handle(
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        std::unique_ptr<::streaming::listener> listener,
        std::shared_ptr<void> owner,
        uint16_t port)
        : service_(std::move(service))
        , acceptor_(std::move(acceptor))
        , listener_(std::move(listener))
        , owner_(std::move(owner))
        , port_(port)
    {
    }

    listener_handle::~listener_handle() = default;

    CORO_TASK(void) listener_handle::stop()
    {
        if (listener_)
            CO_AWAIT listener_->stop_listening();
        listener_.reset();
        acceptor_.reset();
        service_.reset();
        owner_.reset();
        CO_RETURN;
    }

    std::shared_ptr<rpc::service> listener_handle::service() const
    {
        return service_;
    }

    uint16_t listener_handle::port() const noexcept
    {
        return port_;
    }

    rpc_connection_handle::rpc_connection_handle(
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<rpc::stream_transport::transport> transport,
        std::shared_ptr<void> owner)
        : service_(std::move(service))
        , transport_(std::move(transport))
        , owner_(std::move(owner))
    {
    }

    std::shared_ptr<rpc::service> rpc_connection_handle::service() const
    {
        return service_;
    }

    std::shared_ptr<rpc::stream_transport::transport> rpc_connection_handle::transport() const
    {
        return transport_;
    }

    client_rpc_stream_transport_result make_client_rpc_stream_transport(
        std::shared_ptr<::streaming::stream> stream,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service)
    {
        auto resolved_service = ensure_service(settings, std::move(service), "rpc_client_service");
        if (!resolved_service)
            return {rpc::error::INVALID_DATA(), {}, {}};

        auto transport = rpc::stream_transport::make_client(
            transport_name(settings.transport, "initiator_transport"),
            resolved_service,
            std::move(stream),
            transport_options(settings.transport));
        if (!transport)
            return {rpc::error::TRANSPORT_ERROR(), {}, {}};

        return {rpc::error::OK(), std::move(resolved_service), std::move(transport)};
    }

    CORO_TASK(listener_result)
    start_rpc_listener(
        std::shared_ptr<::streaming::stream_acceptor> acceptor,
        ::streaming::listener::connection_callback on_connection,
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<void> owner,
        uint16_t port,
        ::streaming::listener::stream_transformer transform_stream)
    {
        auto resolved_service = ensure_service(settings, std::move(service), "rpc_accept_service");
        if (!resolved_service)
            CO_RETURN listener_result{rpc::error::INVALID_DATA(), {}};

        auto listener = std::make_unique<::streaming::listener>(
            listener_name(settings.listener, "responder_listener"),
            acceptor,
            std::move(on_connection),
            std::move(transform_stream));

        if (!CO_AWAIT listener->start_listening_async(resolved_service))
            CO_RETURN listener_result{rpc::error::TRANSPORT_ERROR(), {}};

        CO_RETURN listener_result{rpc::error::OK(),
            std::make_shared<listener_handle>(
                std::move(resolved_service), std::move(acceptor), std::move(listener), std::move(owner), port)};
    }
} // namespace rpc::stream_transport
