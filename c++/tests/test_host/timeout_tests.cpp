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
 *     tcp          — streaming::tcp::stream over TCP loopback (port 8090)
 *     spsc         — streaming::spsc_queue::stream (in-process)
 *     io_uring     — streaming::io_uring::stream (Linux, port 8091)
 *     tls          — streaming::tls::stream over TCP+SPSC (port 8092)
 *     websocket    — streaming::websocket::stream over TCP (port 8093)
 *                    requires CANOPY_BUILD_WEBSOCKET
 */

#include <rpc/rpc.h>
#include <common/tests.h>
#include <vector>

#include "gtest/gtest.h"
#include "test_host.h"
#include "test_globals.h"

#ifdef CANOPY_BUILD_COROUTINE

#  include <filesystem>

#  include <streaming/listener.h>
#  include <streaming/spsc_queue/stream.h>
#  include <streaming/spsc_wrapping/stream.h>
#  include <streaming/tcp/acceptor.h>
#  include <streaming/tcp/stream.h>
#  include <streaming/tls/stream.h>
#  include <transports/streaming/transport.h>

#  ifdef __linux__
#    include <streaming/io_uring/acceptor.h>
#    include <streaming/io_uring/stream.h>
#  endif

#  ifdef CANOPY_BUILD_WEBSOCKET
#    include <streaming/websocket/stream.h>
#    include <wslay/wslay.h>
#  endif

// TLS cert generation
#  include <openssl/evp.h>
#  include <openssl/pem.h>
#  include <openssl/x509.h>
#  include <cstdio>

using namespace marshalled_tests;

// ---------------------------------------------------------------------------
// TLS certificate helper
// ---------------------------------------------------------------------------

namespace
{
    static bool generate_test_tls_cert(
        const std::string& cert_path,
        const std::string& key_path)
    {
        if (std::filesystem::exists(cert_path) && std::filesystem::exists(key_path))
            return true;

        EVP_PKEY* pkey = nullptr;
        {
            EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
            if (!kctx)
                return false;
            EVP_PKEY_keygen_init(kctx);
            EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
            EVP_PKEY_keygen(kctx, &pkey);
            EVP_PKEY_CTX_free(kctx);
        }
        if (!pkey)
            return false;

        X509* x509 = X509_new();
        if (!x509)
        {
            EVP_PKEY_free(pkey);
            return false;
        }

        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
        X509_gmtime_adj(X509_get_notBefore(x509), 0);
        X509_gmtime_adj(X509_get_notAfter(x509), 10L * 365 * 24 * 60 * 60);
        X509_set_pubkey(x509, pkey);

        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("timeout_test"), -1, -1, 0);
        X509_set_issuer_name(x509, name);
        X509_sign(x509, pkey, EVP_sha256());

        FILE* cf = fopen(cert_path.c_str(), "wb");
        if (!cf)
        {
            X509_free(x509);
            EVP_PKEY_free(pkey);
            return false;
        }
        PEM_write_X509(cf, x509);
        fclose(cf);

        FILE* kf = fopen(key_path.c_str(), "wb");
        if (!kf)
        {
            X509_free(x509);
            EVP_PKEY_free(pkey);
            return false;
        }
        PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        fclose(kf);

        X509_free(x509);
        EVP_PKEY_free(pkey);
        return true;
    }

    static std::string test_cert_dir()
    {
        auto dir = std::filesystem::temp_directory_path() / "canopy_timeout_tests";
        std::filesystem::create_directories(dir);
        return dir.string();
    }
} // namespace

// ---------------------------------------------------------------------------
// WebSocket client-mode stream
// Mirrors the server-side streaming::websocket::stream but uses
// wslay_event_context_client_init so outgoing frames are masked (RFC 6455).
// ---------------------------------------------------------------------------

#  ifdef CANOPY_BUILD_WEBSOCKET
namespace
{
    class ws_client_stream : public streaming::stream
    {
    public:
        explicit ws_client_stream(std::shared_ptr<::streaming::stream> underlying)
            : underlying_(std::move(underlying))
            , raw_recv_buffer_(
                  io_chunk_size_,
                  '\0')
        {
            wslay_event_callbacks cbs{};
            cbs.recv_callback = recv_cb;
            cbs.send_callback = send_cb;
            cbs.on_msg_recv_callback = on_msg_cb;
            cbs.genmask_callback = genmask_cb;
            if (wslay_event_context_client_init(&ctx_, &cbs, this) != 0)
                throw std::runtime_error("ws_client_stream: wslay client init failed");
        }

        ~ws_client_stream() override
        {
            if (ctx_)
                wslay_event_context_free(ctx_);
        }

        ws_client_stream(const ws_client_stream&) = delete;
        auto operator=(const ws_client_stream&) -> ws_client_stream& = delete;
        ws_client_stream(ws_client_stream&&) = delete;
        auto operator=(ws_client_stream&&) -> ws_client_stream& = delete;

        auto receive(
            rpc::mutable_byte_span buf,
            std::chrono::milliseconds /*timeout*/ = std::chrono::milliseconds{0})
            -> coro::task<std::pair<
                coro::net::io_status,
                rpc::mutable_byte_span>> override
        {
            if (!decoded_.empty())
                co_return serve_decoded(buf);

            while (true)
            {
                if (!co_await do_send())
                    co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};

                auto [status, span] = co_await underlying_->receive(
                    rpc::mutable_byte_span(raw_recv_buffer_.data(), raw_recv_buffer_.size()));
                if (status.is_closed())
                {
                    closed_ = true;
                    co_return {status, {}};
                }
                if (status.is_ok() && !span.empty())
                {
                    raw_recv_pos_ = 0;
                    raw_recv_size_ = span.size();
                    wslay_event_recv(ctx_);
                    if (!co_await do_send())
                        co_return {coro::net::io_status{.type = coro::net::io_status::kind::closed}, {}};
                    if (!decoded_.empty())
                        co_return serve_decoded(buf);
                }
                else
                {
                    co_return {status, {}};
                }
            }
        }

        auto send(rpc::byte_span buf) -> coro::task<coro::net::io_status> override
        {
            if (closed_)
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};

            wslay_event_msg msg{};
            msg.opcode = WSLAY_BINARY_FRAME;
            msg.msg = reinterpret_cast<const uint8_t*>(buf.data());
            msg.msg_length = buf.size();
            if (wslay_event_queue_msg(ctx_, &msg) != 0)
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
            if (!co_await do_send())
                co_return coro::net::io_status{.type = coro::net::io_status::kind::closed};
            co_return coro::net::io_status{.type = coro::net::io_status::kind::ok};
        }

        [[nodiscard]] bool is_closed() const override { return closed_; }

        auto set_closed() -> coro::task<void> override
        {
            closed_ = true;
            if (underlying_)
                co_await underlying_->set_closed();
            co_return;
        }

        auto get_peer_info() const -> streaming::peer_info override { return underlying_->get_peer_info(); }

    private:
        static constexpr size_t io_chunk_size_ = 8192;

        std::pair<
            coro::net::io_status,
            rpc::mutable_byte_span>
        serve_decoded(rpc::mutable_byte_span buf)
        {
            auto& msg = decoded_.front();
            size_t avail = msg.size() - msg_offset_;
            size_t n = std::min(avail, buf.size());
            std::memcpy(buf.data(), msg.data() + msg_offset_, n);
            msg_offset_ += n;
            if (msg_offset_ >= msg.size())
            {
                decoded_.pop();
                msg_offset_ = 0;
            }
            return {coro::net::io_status{.type = coro::net::io_status::kind::ok}, buf.subspan(0, n)};
        }

        auto do_send() -> coro::task<bool>
        {
            while (wslay_event_want_write(ctx_))
            {
                outgoing_raw_.clear();
                wslay_event_send(ctx_);
                size_t offset = 0;
                while (offset < outgoing_raw_.size())
                {
                    size_t chunk = std::min(io_chunk_size_, outgoing_raw_.size() - offset);
                    auto st = co_await underlying_->send(
                        rpc::byte_span(reinterpret_cast<const char*>(outgoing_raw_.data() + offset), chunk));
                    if (!st.is_ok())
                        co_return false;
                    offset += chunk;
                }
                outgoing_raw_.clear();
            }
            co_return true;
        }

        static int genmask_cb(
            wslay_event_context_ptr,
            uint8_t* buf,
            size_t len,
            void*)
        {
            for (size_t i = 0; i < len; ++i)
                buf[i] = static_cast<uint8_t>(std::rand() & 0xff); // NOLINT(cert-msc50-cpp)
            return 0;
        }

        static ssize_t send_cb(
            wslay_event_context_ptr,
            const uint8_t* data,
            size_t len,
            int,
            void* ud)
        {
            auto* self = static_cast<ws_client_stream*>(ud);
            self->outgoing_raw_.insert(self->outgoing_raw_.end(), data, data + len);
            return static_cast<ssize_t>(len);
        }

        static ssize_t recv_cb(
            wslay_event_context_ptr ctx,
            uint8_t* out,
            size_t len,
            int,
            void* ud)
        {
            auto* self = static_cast<ws_client_stream*>(ud);
            size_t avail = self->raw_recv_size_ - self->raw_recv_pos_;
            if (avail == 0)
            {
                wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
                return -1;
            }
            size_t n = std::min(avail, len);
            std::memcpy(out, self->raw_recv_buffer_.data() + self->raw_recv_pos_, n);
            self->raw_recv_pos_ += n;
            return static_cast<ssize_t>(n);
        }

        static void on_msg_cb(
            wslay_event_context_ptr,
            const wslay_event_on_msg_recv_arg* arg,
            void* ud)
        {
            auto* self = static_cast<ws_client_stream*>(ud);
            if (arg->opcode == WSLAY_BINARY_FRAME || arg->opcode == WSLAY_TEXT_FRAME)
            {
                self->decoded_.emplace(
                    reinterpret_cast<const uint8_t*>(arg->msg),
                    reinterpret_cast<const uint8_t*>(arg->msg) + arg->msg_length);
                self->msg_offset_ = 0;
            }
        }

        std::shared_ptr<::streaming::stream> underlying_;
        wslay_event_context_ptr ctx_{nullptr};
        std::string raw_recv_buffer_;
        size_t raw_recv_size_{0};
        size_t raw_recv_pos_{0};
        std::queue<std::vector<uint8_t>> decoded_;
        size_t msg_offset_{0};
        std::vector<uint8_t> outgoing_raw_;
        bool closed_{false};
    };
} // namespace
#  endif // CANOPY_BUILD_WEBSOCKET

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
// TCP setup (port 8090)
// ---------------------------------------------------------------------------

class timeout_tcp_setup : public timeout_setup_base
{
protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = rpc::DEFAULT_PREFIX;
        std::ignore = peer_zone_id.set_subnet(peer_zone_id.get_subnet() + 1);

        peer_service_ = rpc::root_service::create("peer", peer_zone_id, io_scheduler_);
        root_service_ = rpc::root_service::create("host", root_zone_id, io_scheduler_);

        listener_ = std::make_unique<streaming::listener>(
            "responder",
            std::make_shared<streaming::tcp::acceptor>(coro::net::socket_address{"127.0.0.1", 8090}),
            rpc::stream_transport::make_connection_callback<yyy::i_host, yyy::i_example>(
                make_hanging_factory(), call_timeout_, call_timeout_sweep));

        if (!listener_->start_listening(peer_service_))
        {
            RPC_ERROR("timeout_tcp_setup: failed to start listener");
            CO_RETURN false;
        }

        auto scheduler = root_service_->get_scheduler();
        coro::net::tcp::client client(scheduler, coro::net::socket_address{"127.0.0.1", 8090});
        auto conn_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
        if (conn_status != coro::net::connect_status::connected)
        {
            RPC_ERROR("timeout_tcp_setup: TCP connect failed");
            CO_RETURN false;
        }

        auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(client), scheduler);
        auto initiator = rpc::stream_transport::make_client(
            "initiator", root_service_, std::move(tcp_stm), call_timeout_, call_timeout_sweep);

        rpc::shared_ptr<yyy::i_host> local_host(new host());
        auto connect_result
            = CO_AWAIT root_service_->connect_to_zone<yyy::i_host, yyy::i_example>("child", initiator, local_host);
        i_example_ptr_ = std::move(connect_result.output_interface);
        if (connect_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("timeout_tcp_setup: connect_to_zone failed: {}", connect_result.error_code);
            CO_RETURN false;
        }
        CO_RETURN true;
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
    ~timeout_tcp_setup() override = default;
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
                rpc::stream_transport::transport_factory(std::move(peer_stream), call_timeout_, call_timeout_sweep),
                make_hanging_factory()));
        CO_AWAIT responder->accept();

        auto client_stream = std::make_shared<streaming::spsc_queue::stream>(&send_queue_, &recv_queue_, io_scheduler_);
        auto initiator = rpc::stream_transport::make_client(
            "initiator", root_service_, std::move(client_stream), call_timeout_, call_timeout_sweep);

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

// ---------------------------------------------------------------------------
// io_uring setup (Linux, port 8091)
// ---------------------------------------------------------------------------

#  ifdef __linux__
class timeout_iouring_setup : public timeout_setup_base
{
protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = rpc::DEFAULT_PREFIX;
        std::ignore = peer_zone_id.set_subnet(peer_zone_id.get_subnet() + 1);

        peer_service_ = rpc::root_service::create("peer", peer_zone_id, io_scheduler_);
        root_service_ = rpc::root_service::create("host", root_zone_id, io_scheduler_);

        canopy::network_config::ip_address addr{};
        addr[0] = 127;
        addr[3] = 1;

        listener_ = std::make_unique<streaming::listener>(
            "responder",
            std::make_shared<streaming::io_uring::acceptor>(addr, 8091),
            rpc::stream_transport::make_connection_callback<yyy::i_host, yyy::i_example>(
                make_hanging_factory(), call_timeout_, call_timeout_sweep));

        if (!listener_->start_listening(peer_service_))
        {
            RPC_ERROR("timeout_iouring_setup: failed to start listener");
            CO_RETURN false;
        }

        auto scheduler = root_service_->get_scheduler();
        coro::net::tcp::client client(scheduler, coro::net::socket_address{"127.0.0.1", 8091});
        auto conn_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
        if (conn_status != coro::net::connect_status::connected)
        {
            RPC_ERROR("timeout_iouring_setup: connect failed");
            CO_RETURN false;
        }

        auto io_stm = std::make_shared<streaming::io_uring::stream>(std::move(client), scheduler);
        auto initiator = rpc::stream_transport::make_client(
            "initiator", root_service_, std::move(io_stm), call_timeout_, call_timeout_sweep);

        rpc::shared_ptr<yyy::i_host> local_host(new host());
        auto connect_result
            = CO_AWAIT root_service_->connect_to_zone<yyy::i_host, yyy::i_example>("child", initiator, local_host);
        i_example_ptr_ = std::move(connect_result.output_interface);
        if (connect_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("timeout_iouring_setup: connect_to_zone failed: {}", connect_result.error_code);
            CO_RETURN false;
        }
        CO_RETURN true;
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
    ~timeout_iouring_setup() override = default;
};
#  endif // __linux__

// ---------------------------------------------------------------------------
// TLS setup — TCP → SPSC wrapping → TLS (port 8092)
// ---------------------------------------------------------------------------

class timeout_tls_setup : public timeout_setup_base
{
protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = rpc::DEFAULT_PREFIX;
        std::ignore = peer_zone_id.set_subnet(peer_zone_id.get_subnet() + 1);

        peer_service_ = rpc::root_service::create("peer", peer_zone_id, io_scheduler_);
        root_service_ = rpc::root_service::create("host", root_zone_id, io_scheduler_);

        auto cert_dir = test_cert_dir();
        auto cert_path = cert_dir + "/cert.pem";
        auto key_path = cert_dir + "/key.pem";
        if (!generate_test_tls_cert(cert_path, key_path))
        {
            RPC_ERROR("timeout_tls_setup: failed to generate TLS cert");
            CO_RETURN false;
        }

        auto tls_ctx = std::make_shared<streaming::tls::context>(cert_path, key_path);
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
            auto spsc_stm = streaming::spsc_wrapping::stream::create(tcp_stm, io_sched);
            auto tls_stm = std::make_shared<streaming::tls::stream>(spsc_stm, tls_ctx);
            if (!CO_AWAIT tls_stm->handshake())
                CO_RETURN std::nullopt;
            CO_RETURN tls_stm;
        };

        listener_ = std::make_unique<streaming::listener>(
            "responder",
            std::make_shared<streaming::tcp::acceptor>(coro::net::socket_address{"127.0.0.1", 8092}),
            rpc::stream_transport::make_connection_callback<yyy::i_host, yyy::i_example>(
                make_hanging_factory(), call_timeout_, call_timeout_sweep),
            std::move(tls_transformer));

        if (!listener_->start_listening(peer_service_))
        {
            RPC_ERROR("timeout_tls_setup: failed to start listener");
            CO_RETURN false;
        }

        auto scheduler = root_service_->get_scheduler();
        coro::net::tcp::client client(scheduler, coro::net::socket_address{"127.0.0.1", 8092});
        auto conn_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
        if (conn_status != coro::net::connect_status::connected)
        {
            RPC_ERROR("timeout_tls_setup: TCP connect failed");
            CO_RETURN false;
        }

        auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(client), scheduler);
        auto spsc_stm = streaming::spsc_wrapping::stream::create(tcp_stm, scheduler);

        auto tls_client_ctx = std::make_shared<streaming::tls::client_context>(/*verify_peer=*/false);
        if (!tls_client_ctx->is_valid())
        {
            RPC_ERROR("timeout_tls_setup: TLS client context failed");
            CO_RETURN false;
        }
        auto tls_stm = std::make_shared<streaming::tls::stream>(spsc_stm, tls_client_ctx);
        if (!CO_AWAIT tls_stm->client_handshake())
        {
            RPC_ERROR("timeout_tls_setup: TLS handshake failed");
            CO_RETURN false;
        }

        auto initiator
            = rpc::stream_transport::make_client("initiator", root_service_, tls_stm, call_timeout_, call_timeout_sweep);

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
    ~timeout_tls_setup() override = default;
};

// ---------------------------------------------------------------------------
// WebSocket setup — TCP → WebSocket framing (port 8093)
// ---------------------------------------------------------------------------

#  ifdef CANOPY_BUILD_WEBSOCKET
class timeout_websocket_setup : public timeout_setup_base
{
protected:
    CORO_TASK(bool) do_coro_setup() override
    {
        auto root_zone_id = rpc::DEFAULT_PREFIX;
        auto peer_zone_id = rpc::DEFAULT_PREFIX;
        std::ignore = peer_zone_id.set_subnet(peer_zone_id.get_subnet() + 1);

        peer_service_ = rpc::root_service::create("peer", peer_zone_id, io_scheduler_);
        root_service_ = rpc::root_service::create("host", root_zone_id, io_scheduler_);

        // Server wraps accepted TCP streams with server-mode WebSocket framing.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        auto ws_transformer =
            [](std::shared_ptr<streaming::stream> tcp_stm) -> CORO_TASK(std::optional<std::shared_ptr<streaming::stream>>)
        { CO_RETURN std::make_shared<streaming::websocket::stream>(tcp_stm); };

        listener_ = std::make_unique<streaming::listener>(
            "responder",
            std::make_shared<streaming::tcp::acceptor>(coro::net::socket_address{"127.0.0.1", 8093}),
            rpc::stream_transport::make_connection_callback<yyy::i_host, yyy::i_example>(
                make_hanging_factory(), call_timeout_, call_timeout_sweep),
            std::move(ws_transformer));

        if (!listener_->start_listening(peer_service_))
        {
            RPC_ERROR("timeout_websocket_setup: failed to start listener");
            CO_RETURN false;
        }

        auto scheduler = root_service_->get_scheduler();
        coro::net::tcp::client client(scheduler, coro::net::socket_address{"127.0.0.1", 8093});
        auto conn_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
        if (conn_status != coro::net::connect_status::connected)
        {
            RPC_ERROR("timeout_websocket_setup: TCP connect failed");
            CO_RETURN false;
        }

        auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(client), scheduler);
        auto ws_stm = std::make_shared<ws_client_stream>(tcp_stm);
        auto initiator = rpc::stream_transport::make_client(
            "initiator", root_service_, std::move(ws_stm), call_timeout_, call_timeout_sweep);

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
    ~timeout_websocket_setup() override = default;
};
#  endif // CANOPY_BUILD_WEBSOCKET

#endif // CANOPY_BUILD_COROUTINE

// ---------------------------------------------------------------------------
// Test fixture and suite
// ---------------------------------------------------------------------------

#include "type_test_fixture.h"

template<class T> using timeout_test = type_test<T>;

#ifdef CANOPY_BUILD_COROUTINE

using timeout_implementations = ::testing::Types<
    timeout_tcp_setup,
    timeout_spsc_setup
#  ifdef __linux__
    ,
    timeout_iouring_setup
#  endif
    ,
    timeout_tls_setup
#  ifdef CANOPY_BUILD_WEBSOCKET
    ,
    timeout_websocket_setup
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
