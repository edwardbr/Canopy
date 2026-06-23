/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Timeout tests — verifies that CALL_TIMEOUT fires for every supported
 *   streaming transport type when the server-side implementation never
 *   responds.
 *
 *   Setup pattern:
 *     - call_timeout      = 500 ms  (how long before a pending RPC is timed out)
 *     - call_timeout_sweep = 100 ms  (how often the sweep loop checks)
 *     - Server installs a hanging_example whose add() awaits a shared
 *       unblock_event that is only set during teardown.
 *     - The client calls add(); the sweep fires CALL_TIMEOUT before the
 *       server ever responds.
 *
 *   Transport variants covered:
 *     tcp_coroutine — streaming::coroutine::tcp::stream over TCP loopback (port 8091)
 *     spsc         — streaming::spsc_queue::stream (in-process)
 *     tls          — selected secure stream backend over TCP coroutine + SPSC (port 8092)
 *     websocket    — streaming::websocket::stream over TCP coroutine (port 8093)
 *                    requires CANOPY_BUILD_WEBSOCKET
 */

#include <rpc/rpc.h>
#include <common/tests.h>
#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test_host.h"
#include "test_globals.h"

#ifdef CANOPY_BUILD_COROUTINE

#  include <filesystem>

#  include <streaming/listener.h>
#  include <streaming/spsc_queue/stream.h>
#  include <streaming/spsc_buffered_stream/stream.h>
#  include <streaming/secure_stream.h>
#  include <transports/streaming/transport.h>

#  ifdef __linux__
#    include <io_uring/host_io_uring.h>
#    include <io_uring/tcp.h>
#    include <streaming/tcp_coroutine/acceptor.h>
#    include <streaming/tcp_coroutine/connector.h>
#    include <streaming/tcp_coroutine/stream.h>
#  endif

#  ifdef CANOPY_BUILD_WEBSOCKET
#    include <streaming/websocket/stream.h>
#  endif

using namespace marshalled_tests;

// ---------------------------------------------------------------------------
// TLS certificate helper
// ---------------------------------------------------------------------------

#  ifndef CANOPY_TIMEOUT_TEST_CERT_DIR
#    error "CANOPY_TIMEOUT_TEST_CERT_DIR must point at the timeout_test certificate fixtures"
#  endif

namespace
{
    static auto timeout_test_cert_dir() -> std::filesystem::path
    {
        return std::filesystem::path(CANOPY_TIMEOUT_TEST_CERT_DIR);
    }
} // namespace

#  ifdef CANOPY_BUILD_WEBSOCKET
namespace
{
    auto make_websocket_test_scheduler() -> std::shared_ptr<coro::scheduler>
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{
                .thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                .pool = coro::thread_pool::options{.thread_count = 2},
                .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool,
            }));
    }

    struct websocket_spsc_pair
    {
        std::shared_ptr<streaming::spsc_queue::queue_type> client_to_server;
        std::shared_ptr<streaming::spsc_queue::queue_type> server_to_client;
        std::shared_ptr<streaming::spsc_queue::stream> client_base;
        std::shared_ptr<streaming::spsc_queue::stream> server_base;
    };

    auto make_websocket_spsc_pair(std::shared_ptr<coro::scheduler> scheduler) -> websocket_spsc_pair
    {
        websocket_spsc_pair pair;
        pair.client_to_server = std::make_shared<streaming::spsc_queue::queue_type>();
        pair.server_to_client = std::make_shared<streaming::spsc_queue::queue_type>();
        pair.client_base
            = std::make_shared<streaming::spsc_queue::stream>(pair.client_to_server, pair.server_to_client, scheduler);
        pair.server_base = std::make_shared<streaming::spsc_queue::stream>(
            pair.server_to_client, pair.client_to_server, std::move(scheduler));
        return pair;
    }
} // namespace
#  endif

// ---------------------------------------------------------------------------
// Server-side implementation that never responds
// ---------------------------------------------------------------------------

namespace
{
    // Inherits the full marshalled_tests::example implementation but overrides
    // add() to block until unblock_event is set (from the test's tear_down).
    class hanging_example : public marshalled_tests::example
    {
        std::shared_ptr<rpc::event> unblock_;

    public:
        hanging_example(
            std::shared_ptr<rpc::service> svc,
            rpc::shared_ptr<yyy::i_host> host,
            std::shared_ptr<rpc::event> unblock)
            : example(
                  svc,
                  host)
            , unblock_(std::move(unblock))
        {
        }

        CORO_TASK(error_code)
        add(int a,
            int b,
            int& c) override
        {
            std::ignore = a;
            std::ignore = b;
            std::ignore = c;
            // Block until the test teardown fires the event.
            CO_AWAIT unblock_->wait();
            CO_RETURN rpc::error::OK();
        }
    };
} // namespace

// ---------------------------------------------------------------------------
// Shared timeout constants
// ---------------------------------------------------------------------------

namespace
{
    constexpr auto call_timeout_ = std::chrono::milliseconds{500};
    constexpr auto call_timeout_sweep = std::chrono::milliseconds{100};

    rpc::stream_transport::stream_transport_options timeout_transport_options()
    {
        return rpc::stream_transport::stream_transport_options{
            .call_timeout = call_timeout_,
            .call_timeout_sweep = call_timeout_sweep,
        };
    }
} // namespace

// ---------------------------------------------------------------------------
// Base class shared by all timeout setup variants
// ---------------------------------------------------------------------------

class timeout_setup_base
{
protected:
    std::shared_ptr<coro::scheduler> io_scheduler_;
    bool error_has_occurred_ = false;

    std::shared_ptr<rpc::event> unblock_event_;
    std::shared_ptr<rpc::root_service> root_service_;
    std::shared_ptr<rpc::root_service> peer_service_;
    std::unique_ptr<streaming::listener> listener_;
    rpc::shared_ptr<yyy::i_example> i_example_ptr_;

    virtual CORO_TASK(bool) do_coro_setup() = 0;
    virtual CORO_TASK(void) do_coro_teardown() { CO_RETURN; }

    // Returns a zone factory that creates a hanging_example server side.
    auto make_hanging_factory()
    {
        auto unblock = unblock_event_;
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        return [unblock](
                   rpc::shared_ptr<yyy::i_host> host,
                   std::shared_ptr<rpc::service> svc) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
        {
            CO_RETURN rpc::service_connect_result<yyy::i_example>{
                rpc::error::OK(), rpc::shared_ptr<yyy::i_example>(new hanging_example(svc, host, unblock))};
        };
    }

public:
    virtual ~timeout_setup_base() = default;

    [[nodiscard]] std::shared_ptr<coro::scheduler> get_scheduler() const { return io_scheduler_; }
    [[nodiscard]] bool error_has_occurred() const { return error_has_occurred_; }
    [[nodiscard]] rpc::shared_ptr<yyy::i_example> get_example() const { return i_example_ptr_; }

    CORO_TASK(void) check_for_error(CORO_TASK(bool) task)
    {
        auto ret = CO_AWAIT task;
        if (!ret)
        {
            error_has_occurred_ = true;
        }
        CO_RETURN;
    }

    void set_up()
    {
        error_has_occurred_ = false;
        unblock_event_ = std::make_shared<rpc::event>();

        io_scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{
                .thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1},
            }));

        bool setup_complete = false;
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        auto setup_task = [this, &setup_complete]() -> coro::task<void>
        {
            CO_AWAIT check_for_error(do_coro_setup());
            setup_complete = true;
            CO_RETURN;
        };
        RPC_ASSERT(io_scheduler_->spawn_detached(setup_task()));
        while (!setup_complete)
            io_scheduler_->process_events(std::chrono::milliseconds(1));
        ASSERT_EQ(error_has_occurred_, false);
    }

    void tear_down()
    {
        // Unblock any hanging server-side coroutines so teardown can proceed.
        unblock_event_->set();
        i_example_ptr_ = nullptr;

        bool shutdown_complete = false;
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        auto shutdown_task = [this, &shutdown_complete]() -> coro::task<void>
        {
            CO_AWAIT do_coro_teardown();
            CO_AWAIT io_scheduler_->schedule();
            CO_AWAIT io_scheduler_->schedule();
            shutdown_complete = true;
            CO_RETURN;
        };
        RPC_ASSERT(io_scheduler_->spawn_detached(shutdown_task()));
        while (!shutdown_complete)
            io_scheduler_->process_events(std::chrono::milliseconds(1));

        // Drain events to let transport cleanup finish.
        for (int i = 0; i < 2000; ++i)
            io_scheduler_->process_events(std::chrono::milliseconds(1));

        peer_service_.reset();
        root_service_.reset();
    }
};

// ---------------------------------------------------------------------------
// SPSC setup (in-process)
// ---------------------------------------------------------------------------

class timeout_spsc_setup : public timeout_setup_base
{
    streaming::spsc_queue::queue_type send_queue_;
    streaming::spsc_queue::queue_type recv_queue_;

protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = rpc::DEFAULT_PREFIX;
        std::ignore = peer_zone_id.set_subnet(peer_zone_id.get_subnet() + 1);

        root_service_ = rpc::root_service::create("host", root_zone_id, io_scheduler_);
        peer_service_ = rpc::root_service::create("peer", peer_zone_id, io_scheduler_);

        auto peer_stream = std::make_shared<streaming::spsc_queue::stream>(&recv_queue_, &send_queue_, io_scheduler_);
        auto responder = std::static_pointer_cast<rpc::stream_transport::transport>(
            CO_AWAIT peer_service_->make_acceptor<yyy::i_host, yyy::i_example>(
                "responder",
                rpc::stream_transport::transport_factory(std::move(peer_stream), timeout_transport_options()),
                make_hanging_factory()));
        CO_AWAIT responder->accept();

        auto client_stream = std::make_shared<streaming::spsc_queue::stream>(&send_queue_, &recv_queue_, io_scheduler_);
        auto initiator = rpc::stream_transport::make_client(
            "initiator", root_service_, std::move(client_stream), timeout_transport_options());

        rpc::shared_ptr<yyy::i_host> local_host(new host());
        auto connect_result
            = CO_AWAIT root_service_->connect_to_zone<yyy::i_host, yyy::i_example>("child", initiator, local_host);
        i_example_ptr_ = std::move(connect_result.output_interface);
        if (connect_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("timeout_spsc_setup: connect_to_zone failed: {}", connect_result.error_code);
            CO_RETURN false;
        }
        CO_RETURN true;
    }

public:
    ~timeout_spsc_setup() override = default;
};

#  ifdef __linux__
// ---------------------------------------------------------------------------
// TCP coroutine setup (Linux, port 8091)
// ---------------------------------------------------------------------------

class timeout_tcp_coroutine_setup_base : public timeout_setup_base
{
protected:
    static rpc::io_uring::linux_io_uring_handle::options make_io_uring_options()
    {
        rpc::io_uring::linux_io_uring_handle::options options;
        options.queue_depth = 256;
        options.use_sqpoll = true;
        options.buffer_count = 256;
        options.buffer_size = 4096;
        options.register_buffers = false;
        options.fixed_file_count = 128;
        options.register_fixed_files = true;
        return options;
    }

    auto make_tcp_coroutine_controller(const char* setup_name) -> std::shared_ptr<rpc::io_uring::controller>
    {
        auto ret = rpc::io_uring::create_scheduler(io_uring_scheduler_owner_, make_io_uring_options(), io_scheduler_);
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("{}: failed to create io_uring scheduler: {}", setup_name, ret);
            return {};
        }

        auto controller = io_uring_scheduler_owner_->get_controller();
        if (!controller)
        {
            RPC_ERROR("{}: missing io_uring controller", setup_name);
            return {};
        }
        return controller;
    }

    CORO_TASK(void) do_coro_teardown() override
    {
        if (listener_)
        {
            CO_AWAIT listener_->stop_listening();
            listener_.reset();
        }
        CO_RETURN;
    }

public:
    void tear_down()
    {
        timeout_setup_base::tear_down();
        if (io_uring_scheduler_owner_)
        {
            io_uring_scheduler_owner_->shutdown();
            io_uring_scheduler_owner_.reset();
        }
    }

private:
    std::shared_ptr<rpc::io_uring::io_uring_scheduler> io_uring_scheduler_owner_;
};

class timeout_tcp_coroutine_setup : public timeout_tcp_coroutine_setup_base
{
protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = rpc::DEFAULT_PREFIX;
        std::ignore = peer_zone_id.set_subnet(peer_zone_id.get_subnet() + 1);

        peer_service_ = rpc::root_service::create("peer", peer_zone_id, io_scheduler_);
        root_service_ = rpc::root_service::create("host", root_zone_id, io_scheduler_);

        auto controller = make_tcp_coroutine_controller("timeout_tcp_coroutine_setup");
        if (!controller)
            CO_RETURN false;

        auto acceptor = std::make_shared<streaming::coroutine::tcp::acceptor>(controller);
        auto listen_result = CO_AWAIT acceptor->listen_loopback(8091);
        if (listen_result != rpc::error::OK())
        {
            RPC_ERROR("timeout_tcp_coroutine_setup: listen failed: {}", listen_result);
            CO_RETURN false;
        }

        listener_ = std::make_unique<streaming::listener>(
            "responder",
            std::move(acceptor),
            rpc::stream_transport::make_connection_callback<yyy::i_host, yyy::i_example>(
                make_hanging_factory(), timeout_transport_options()));

        if (!listener_->start_listening(peer_service_))
        {
            RPC_ERROR("timeout_tcp_coroutine_setup: failed to start listener");
            CO_RETURN false;
        }

        rpc::io_uring::connector connector(controller);
        auto descriptor_result = CO_AWAIT connector.connect_loopback_with_result(8091);
        auto stream_result = streaming::coroutine::tcp::make_stream_result(
            descriptor_result, 8091, streaming::coroutine::tcp::default_stream_options(), root_service_->get_scheduler());
        if (stream_result.error_code != rpc::error::OK() || !stream_result.connection)
        {
            RPC_ERROR("timeout_tcp_coroutine_setup: connect failed");
            CO_RETURN false;
        }

        auto initiator = rpc::stream_transport::make_client(
            "initiator", root_service_, std::move(stream_result.connection), timeout_transport_options());

        rpc::shared_ptr<yyy::i_host> local_host(new host());
        auto connect_result
            = CO_AWAIT root_service_->connect_to_zone<yyy::i_host, yyy::i_example>("child", initiator, local_host);
        i_example_ptr_ = std::move(connect_result.output_interface);
        if (connect_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("timeout_tcp_coroutine_setup: connect_to_zone failed: {}", connect_result.error_code);
            CO_RETURN false;
        }
        CO_RETURN true;
    }

public:
    ~timeout_tcp_coroutine_setup() override = default;
};
#  endif // __linux__

// ---------------------------------------------------------------------------
// TLS setup — TCP → SPSC buffered stream → TLS (port 8092)
// ---------------------------------------------------------------------------

#  ifdef __linux__
class timeout_tls_setup : public timeout_tcp_coroutine_setup_base
{
protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = rpc::DEFAULT_PREFIX;
        std::ignore = peer_zone_id.set_subnet(peer_zone_id.get_subnet() + 1);

        peer_service_ = rpc::root_service::create("peer", peer_zone_id, io_scheduler_);
        root_service_ = rpc::root_service::create("host", root_zone_id, io_scheduler_);

        auto controller = make_tcp_coroutine_controller("timeout_tls_setup");
        if (!controller)
            CO_RETURN false;

        const auto cert_dir = timeout_test_cert_dir();
        const auto cert_path = cert_dir / "server.crt";
        const auto key_path = cert_dir / "server.key";
        if (!std::filesystem::exists(cert_path) || !std::filesystem::exists(key_path))
        {
            RPC_ERROR("timeout_tls_setup: missing TLS fixture credentials in {}", cert_dir.string());
            CO_RETURN false;
        }

        auto tls_ctx = std::make_shared<streaming::secure::context>(cert_path.string(), key_path.string());
        if (!tls_ctx->is_valid())
        {
            RPC_ERROR("timeout_tls_setup: TLS context init failed");
            CO_RETURN false;
        }

        auto io_sched = io_scheduler_;
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        auto tls_transformer = [tls_ctx, io_sched](std::shared_ptr<streaming::stream> tcp_stm)
            -> CORO_TASK(std::optional<std::shared_ptr<streaming::stream>>)
        {
            auto spsc_stm = streaming::spsc_buffered_stream::stream::create(tcp_stm, io_sched);
            auto tls_stm = std::make_shared<streaming::secure::stream>(spsc_stm, tls_ctx, io_sched);
            if (!CO_AWAIT tls_stm->handshake())
                CO_RETURN std::nullopt;
            CO_RETURN tls_stm;
        };

        auto acceptor = std::make_shared<streaming::coroutine::tcp::acceptor>(controller);
        auto listen_result = CO_AWAIT acceptor->listen_loopback(8092);
        if (listen_result != rpc::error::OK())
        {
            RPC_ERROR("timeout_tls_setup: listen failed: {}", listen_result);
            CO_RETURN false;
        }

        listener_ = std::make_unique<streaming::listener>(
            "responder",
            std::move(acceptor),
            rpc::stream_transport::make_connection_callback<yyy::i_host, yyy::i_example>(
                make_hanging_factory(), timeout_transport_options()),
            std::move(tls_transformer));

        if (!listener_->start_listening(peer_service_))
        {
            RPC_ERROR("timeout_tls_setup: failed to start listener");
            CO_RETURN false;
        }

        auto scheduler = root_service_->get_scheduler();
        auto tcp_result = CO_AWAIT streaming::coroutine::tcp::connect_loopback(
            controller, 8092, streaming::coroutine::tcp::default_stream_options(), std::chrono::milliseconds{5000}, scheduler);
        if (tcp_result.error_code != rpc::error::OK() || !tcp_result.connection)
        {
            RPC_ERROR("timeout_tls_setup: TCP coroutine connect failed: {}", tcp_result.error_code);
            CO_RETURN false;
        }

        auto spsc_stm = streaming::spsc_buffered_stream::stream::create(std::move(tcp_result.connection), scheduler);

        auto tls_client_ctx = std::make_shared<streaming::secure::client_context>(/*verify_peer=*/false);
        if (!tls_client_ctx->is_valid())
        {
            RPC_ERROR("timeout_tls_setup: TLS client context failed");
            CO_RETURN false;
        }
        auto tls_stm = std::make_shared<streaming::secure::stream>(spsc_stm, tls_client_ctx, scheduler);
        if (!CO_AWAIT tls_stm->client_handshake())
        {
            RPC_ERROR("timeout_tls_setup: TLS handshake failed");
            CO_RETURN false;
        }

        auto initiator
            = rpc::stream_transport::make_client("initiator", root_service_, tls_stm, timeout_transport_options());

        rpc::shared_ptr<yyy::i_host> local_host(new host());
        auto connect_result
            = CO_AWAIT root_service_->connect_to_zone<yyy::i_host, yyy::i_example>("child", initiator, local_host);
        i_example_ptr_ = std::move(connect_result.output_interface);
        if (connect_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("timeout_tls_setup: connect_to_zone failed: {}", connect_result.error_code);
            CO_RETURN false;
        }
        CO_RETURN true;
    }

public:
    ~timeout_tls_setup() override = default;
};

// ---------------------------------------------------------------------------
// WebSocket setup — TCP → WebSocket framing (port 8093)
// ---------------------------------------------------------------------------

#    ifdef CANOPY_BUILD_WEBSOCKET
class timeout_websocket_setup : public timeout_tcp_coroutine_setup_base
{
protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = rpc::DEFAULT_PREFIX;
        std::ignore = peer_zone_id.set_subnet(peer_zone_id.get_subnet() + 1);

        peer_service_ = rpc::root_service::create("peer", peer_zone_id, io_scheduler_);
        root_service_ = rpc::root_service::create("host", root_zone_id, io_scheduler_);

        auto controller = make_tcp_coroutine_controller("timeout_websocket_setup");
        if (!controller)
            CO_RETURN false;

        // Server wraps accepted TCP streams with server-mode WebSocket framing.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        auto ws_transformer =
            [](std::shared_ptr<streaming::stream> tcp_stm) -> CORO_TASK(std::optional<std::shared_ptr<streaming::stream>>)
        { CO_RETURN std::make_shared<streaming::websocket::stream>(tcp_stm); };

        auto acceptor = std::make_shared<streaming::coroutine::tcp::acceptor>(controller);
        auto listen_result = CO_AWAIT acceptor->listen_loopback(8093);
        if (listen_result != rpc::error::OK())
        {
            RPC_ERROR("timeout_websocket_setup: listen failed: {}", listen_result);
            CO_RETURN false;
        }

        listener_ = std::make_unique<streaming::listener>(
            "responder",
            std::move(acceptor),
            rpc::stream_transport::make_connection_callback<yyy::i_host, yyy::i_example>(
                make_hanging_factory(), timeout_transport_options()),
            std::move(ws_transformer));

        if (!listener_->start_listening(peer_service_))
        {
            RPC_ERROR("timeout_websocket_setup: failed to start listener");
            CO_RETURN false;
        }

        auto scheduler = root_service_->get_scheduler();
        auto tcp_result = CO_AWAIT streaming::coroutine::tcp::connect_loopback(
            controller, 8093, streaming::coroutine::tcp::default_stream_options(), std::chrono::milliseconds{5000}, scheduler);
        if (tcp_result.error_code != rpc::error::OK() || !tcp_result.connection)
        {
            RPC_ERROR("timeout_websocket_setup: TCP coroutine connect failed: {}", tcp_result.error_code);
            CO_RETURN false;
        }

        auto ws_stm = std::make_shared<streaming::websocket::stream>(
            std::move(tcp_result.connection), rpc::websocket_stream::endpoint_role::client);
        auto initiator = rpc::stream_transport::make_client(
            "initiator", root_service_, std::move(ws_stm), timeout_transport_options());

        rpc::shared_ptr<yyy::i_host> local_host(new host());
        auto connect_result
            = CO_AWAIT root_service_->connect_to_zone<yyy::i_host, yyy::i_example>("child", initiator, local_host);
        i_example_ptr_ = std::move(connect_result.output_interface);
        if (connect_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("timeout_websocket_setup: connect_to_zone failed: {}", connect_result.error_code);
            CO_RETURN false;
        }
        CO_RETURN true;
    }

public:
    ~timeout_websocket_setup() override = default;
};
#    endif // CANOPY_BUILD_WEBSOCKET
#  endif   // __linux__

#endif // CANOPY_BUILD_COROUTINE

// ---------------------------------------------------------------------------
// Test fixture and suite
// ---------------------------------------------------------------------------

#include "type_test_fixture.h"

#ifdef CANOPY_BUILD_WEBSOCKET
TEST(
    WebSocketStream,
    KeepAlivePingPongDoesNotSurfaceApplicationData)
{
    auto scheduler = make_websocket_test_scheduler();
    auto pair = make_websocket_spsc_pair(scheduler);

    rpc::websocket_stream::stream_settings client_options;
    client_options.role = rpc::websocket_stream::endpoint_role::client;
    client_options.keep_alive.enabled = true;
    client_options.keep_alive.interval_ms = 1;
    client_options.keep_alive.timeout_ms = 250;

    auto client = std::make_shared<streaming::websocket::stream>(pair.client_base, client_options);
    auto server
        = std::make_shared<streaming::websocket::stream>(pair.server_base, rpc::websocket_stream::endpoint_role::server);

    bool client_timed_out = false;
    bool server_timed_out = false;
    size_t client_payload_size = 1;
    size_t server_payload_size = 1;

    coro::sync_wait(
        coro::when_all(
            [&]() -> coro::task<void>
            {
                std::array<uint8_t, 64> buffer{};
                auto [status, bytes]
                    = CO_AWAIT client->receive(rpc::mutable_byte_span{buffer}, std::chrono::milliseconds{100});
                client_timed_out = status.is_timeout();
                client_payload_size = bytes.size();
                CO_RETURN;
            }(),
            [&]() -> coro::task<void>
            {
                std::array<uint8_t, 64> buffer{};
                auto [status, bytes]
                    = CO_AWAIT server->receive(rpc::mutable_byte_span{buffer}, std::chrono::milliseconds{100});
                server_timed_out = status.is_timeout();
                server_payload_size = bytes.size();
                CO_RETURN;
            }()));

    EXPECT_TRUE(client_timed_out);
    EXPECT_TRUE(server_timed_out);
    EXPECT_EQ(client_payload_size, 0u);
    EXPECT_EQ(server_payload_size, 0u);
    EXPECT_FALSE(client->is_closed());
    EXPECT_FALSE(server->is_closed());

    const std::string payload = "after keep alive";
    bool send_ok = false;
    bool receive_ok = false;
    std::string received;

    coro::sync_wait(
        coro::when_all(
            [&]() -> coro::task<void>
            {
                auto status = CO_AWAIT client->send(rpc::byte_span{payload});
                send_ok = status.is_ok();
                CO_RETURN;
            }(),
            [&]() -> coro::task<void>
            {
                std::array<uint8_t, 128> buffer{};
                auto [status, bytes]
                    = CO_AWAIT server->receive(rpc::mutable_byte_span{buffer}, std::chrono::milliseconds{500});
                receive_ok = status.is_ok();
                received.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                CO_RETURN;
            }()));

    EXPECT_TRUE(send_ok);
    EXPECT_TRUE(receive_ok);
    EXPECT_EQ(received, payload);

    coro::sync_wait(client->set_closed());
    coro::sync_wait(server->set_closed());
    scheduler->shutdown();
}
#endif

template<class T> using timeout_test = type_test<T>;

#ifdef CANOPY_BUILD_COROUTINE

using timeout_implementations = ::testing::Types<
    timeout_spsc_setup
#  ifdef __linux__
    ,
    timeout_tcp_coroutine_setup,
    timeout_tls_setup
#    ifdef CANOPY_BUILD_WEBSOCKET
    ,
    timeout_websocket_setup
#    endif
#  endif
    >;

TYPED_TEST_SUITE(
    timeout_test,
    timeout_implementations);

template<class T> CORO_TASK(bool) coro_call_times_out(T& lib)
{
    int c = 0;
    auto err = CO_AWAIT lib.get_example()->add(1, 2, c);
    CORO_ASSERT_EQ(err, rpc::error::CALL_TIMEOUT());
    CO_RETURN true;
}

TYPED_TEST(
    timeout_test,
    call_times_out)
{
    run_coro_test(*this, [](auto& lib) { return coro_call_times_out<TypeParam>(lib); });
}

#endif // CANOPY_BUILD_COROUTINE
