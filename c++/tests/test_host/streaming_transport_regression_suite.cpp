/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <common/tests.h>
#include <array>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include "gtest/gtest.h"
#include "test_host.h"
#include "test_globals.h"

#ifdef CANOPY_BUILD_COROUTINE
#  include <streaming/spsc_queue/factory.h>
#  include <transport/tests/streaming_layered_spsc/setup.h>
#  include <transport/tests/streaming_layered_tcp_coroutine/setup.h>
#  include <transport/tests/streaming_tcp_coroutine/setup.h>
#  include <transport/tests/streaming_spsc/setup.h>
#  ifdef CANOPY_BUILD_WEBSOCKET
#    ifdef CANOPY_BUILD_HTTP_SERVER
#      include <canopy/http_server/http_client_connection.h>
#    endif
#    include <common/foo_impl.h>
#    include <rpc/internal/serialiser.h>
#    include <streaming/websocket/stream.h>
#    include <transports/untrusted_web/factory.h>
#    include <websocket/websocket_protocol.h>
#  endif
#endif

#include "type_test_fixture.h"

using namespace marshalled_tests;

template<class T> using streaming_transport_regression_test = type_test<T>;

#ifdef CANOPY_BUILD_COROUTINE
namespace
{
    template<class Buffer> rpc::byte_span as_byte_span(const Buffer& buffer)
    {
        return rpc::byte_span{reinterpret_cast<const char*>(buffer.data()), buffer.size()};
    }

#  ifdef CANOPY_BUILD_WEBSOCKET
    class scripted_stream final : public streaming::stream
    {
    public:
        void push(std::vector<uint8_t> payload)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            incoming_.push_back(chunk{std::move(payload), 0});
        }

        void push(std::vector<char> payload) { push(std::vector<uint8_t>(payload.begin(), payload.end())); }

        [[nodiscard]] auto sent_messages() const -> std::vector<std::vector<uint8_t>>
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return sent_;
        }

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> CORO_TASK(::streaming::receive_result) override
        {
            std::ignore = timeout;
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_)
                CO_RETURN std::make_pair(rpc::io_status{FLD(type) rpc::io_status::kind::closed}, rpc::mutable_byte_span{});
            if (incoming_.empty())
                CO_RETURN std::make_pair(rpc::io_status{FLD(type) rpc::io_status::kind::timeout}, rpc::mutable_byte_span{});

            auto& front = incoming_.front();
            auto available = front.payload.size() - front.offset;
            auto to_copy = std::min(available, buffer.size());
            std::memcpy(buffer.data(), front.payload.data() + front.offset, to_copy);
            front.offset += to_copy;
            if (front.offset >= front.payload.size())
                incoming_.pop_front();

            CO_RETURN std::make_pair(rpc::io_status{FLD(type) rpc::io_status::kind::ok}, buffer.subspan(0, to_copy));
        }

        auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_)
                CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::closed};
            const auto* begin = reinterpret_cast<const uint8_t*>(buffer.data());
            sent_.emplace_back(begin, begin + buffer.size());
            CO_RETURN rpc::io_status{FLD(type) rpc::io_status::kind::ok};
        }

        [[nodiscard]] bool is_closed() const override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return closed_;
        }

        auto set_closed() -> CORO_TASK(void) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            CO_RETURN;
        }

        [[nodiscard]] auto get_peer_info() const -> streaming::peer_info override { return {}; }

    private:
        struct chunk
        {
            std::vector<uint8_t> payload;
            size_t offset{0};
        };

        mutable std::mutex mutex_;
        std::deque<chunk> incoming_;
        std::vector<std::vector<uint8_t>> sent_;
        bool closed_{false};
    };

    auto make_test_scheduler() -> std::shared_ptr<coro::scheduler>
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
                .pool = coro::thread_pool::options{.thread_count = 1}}));
    }

    template<class Predicate>
    bool process_until(
        const std::shared_ptr<coro::scheduler>& scheduler,
        Predicate predicate,
        int iterations = 3000)
    {
        for (int i = 0; i < iterations && !predicate(); ++i)
            scheduler->process_events(std::chrono::milliseconds{1});
        return predicate();
    }

    auto make_client_frame(
        uint8_t opcode,
        const std::vector<uint8_t>& payload,
        bool masked = true,
        bool fin = true) -> std::vector<uint8_t>
    {
        std::vector<uint8_t> frame;
        frame.push_back(static_cast<uint8_t>((fin ? 0x80 : 0x00) | (opcode & 0x0f)));
        const auto mask_bit = masked ? 0x80 : 0x00;
        if (payload.size() <= 125)
        {
            frame.push_back(static_cast<uint8_t>(mask_bit | payload.size()));
        }
        else
        {
            frame.push_back(static_cast<uint8_t>(mask_bit | 126));
            frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xff));
            frame.push_back(static_cast<uint8_t>(payload.size() & 0xff));
        }

        const std::array<uint8_t, 4> mask{{0x12, 0x34, 0x56, 0x78}};
        if (masked)
        {
            frame.insert(frame.end(), mask.begin(), mask.end());
            for (size_t i = 0; i < payload.size(); ++i)
                frame.push_back(static_cast<uint8_t>(payload[i] ^ mask[i % mask.size()]));
        }
        else
        {
            frame.insert(frame.end(), payload.begin(), payload.end());
        }
        return frame;
    }

    constexpr rpc::encoding untrusted_web_test_encoding()
    {
#    ifdef CANOPY_WEBSOCKET_ENCODING
        return CANOPY_WEBSOCKET_ENCODING;
#    else
        return CANOPY_DEFAULT_ENCODING;
#    endif
    }

    template<
        class OutputBlob = std::vector<char>,
        class T>
    OutputBlob encode_untrusted_web_message(const T& value)
    {
        return rpc::serialise<OutputBlob>(value, untrusted_web_test_encoding());
    }

    template<class T>
    bool decode_untrusted_web_message(
        const std::vector<uint8_t>& payload,
        T& value)
    {
        return rpc::deserialise(untrusted_web_test_encoding(), rpc::byte_span{payload}, value).empty();
    }

    auto make_handshake_message() -> std::vector<uint8_t>
    {
        websocket_protocol::v1::connect_request request;
        request.inbound_interface_id = yyy::i_host::get_id(rpc::get_version());
        request.outbound_interface_id = yyy::i_example::get_id(rpc::get_version());
        request.remote_object_id.object_id = 1;

        websocket_protocol::v1::envelope envelope;
        envelope.id = 1;
        envelope.type = websocket_protocol::v1::message_type::handshake;
        envelope.data = encode_untrusted_web_message(request);
        auto bytes = encode_untrusted_web_message<std::vector<uint8_t>>(envelope);
        return bytes;
    }

    auto make_envelope_message(
        websocket_protocol::v1::message_type type,
        std::vector<char> data = {},
        uint64_t id = 2) -> std::vector<uint8_t>
    {
        websocket_protocol::v1::envelope envelope;
        envelope.id = id;
        envelope.type = type;
        envelope.data = std::move(data);
        return encode_untrusted_web_message<std::vector<uint8_t>>(envelope);
    }

    auto make_untrusted_web_request_message(
        const rpc::zone_address_args& destination,
        websocket_protocol::v1::message_type type,
        bool inject_back_channel,
        size_t payload_bytes = 0,
        uint64_t id = 2) -> std::vector<uint8_t>
    {
        websocket_protocol::v1::request request;
        request.encoding = untrusted_web_test_encoding();
        request.tag = 77;
        request.destination_zone_id = destination;
        request.interface_id = yyy::i_example::get_id(rpc::get_version());
        request.method_id = 999999;
        request.data.resize(payload_bytes, 0x42);

        if (inject_back_channel)
        {
            rpc::back_channel_entry injected;
            injected.type_id = 0x1234;
            injected.payload = {0xde, 0xad, 0xbe, 0xef};
            request.back_channel.push_back(std::move(injected));
        }

        return make_envelope_message(type, encode_untrusted_web_message(request), id);
    }

    auto make_back_channel_injection_message(const rpc::zone_address_args& destination) -> std::vector<uint8_t>
    {
        return make_untrusted_web_request_message(destination, websocket_protocol::v1::message_type::send, true);
    }

    auto parse_handshake_complete_destination(const std::vector<std::vector<uint8_t>>& sent)
        -> std::optional<rpc::zone_address_args>
    {
        for (const auto& raw : sent)
        {
            websocket_protocol::v1::envelope envelope;
            if (!decode_untrusted_web_message(raw, envelope))
                continue;
            if (envelope.type != websocket_protocol::v1::message_type::handshake_complete)
                continue;

            websocket_protocol::v1::connect_response response;
            if (!rpc::deserialise(untrusted_web_test_encoding(), rpc::byte_span{envelope.data}, response).empty())
                return std::nullopt;
            return response.outbound_remote_object;
        }
        return std::nullopt;
    }

    auto parse_response_with_id(
        const std::vector<std::vector<uint8_t>>& sent,
        uint64_t id) -> std::optional<websocket_protocol::v1::response>
    {
        for (const auto& raw : sent)
        {
            websocket_protocol::v1::envelope envelope;
            if (!decode_untrusted_web_message(raw, envelope))
                continue;
            if (envelope.type != websocket_protocol::v1::message_type::response || envelope.id != id)
                continue;

            websocket_protocol::v1::response response;
            if (!rpc::deserialise(untrusted_web_test_encoding(), rpc::byte_span{envelope.data}, response).empty())
                return std::nullopt;
            return response;
        }
        return std::nullopt;
    }

    template<class Factory>
    auto run_untrusted_web_accept(
        std::shared_ptr<scripted_stream> stream,
        rpc::untrusted_web::transport_settings settings,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<std::atomic_bool> done,
        std::shared_ptr<rpc::untrusted_web::accept_result> accepted,
        Factory factory) -> CORO_TASK(void)
    {
        *accepted = CO_AWAIT rpc::untrusted_web::accept_rpc<yyy::i_host, yyy::i_example>(
            stream, std::move(factory), settings, service);
        done->store(true, std::memory_order_release);
        CO_RETURN;
    }

    template<class Factory>
    auto start_untrusted_web_accept(
        const std::shared_ptr<coro::scheduler>& scheduler,
        const std::shared_ptr<scripted_stream>& stream,
        rpc::untrusted_web::transport_settings settings,
        std::shared_ptr<std::atomic_bool> done,
        std::shared_ptr<rpc::untrusted_web::accept_result> accepted,
        Factory factory) -> std::shared_ptr<rpc::service>
    {
        auto service = rpc::root_service::create("untrusted_web_penetration", rpc::DEFAULT_PREFIX, scheduler);
        RPC_ASSERT(scheduler->spawn_detached(run_untrusted_web_accept(
            stream, std::move(settings), service, std::move(done), std::move(accepted), std::move(factory))));
        return service;
    }

    auto make_untrusted_web_example_factory(std::shared_ptr<std::atomic_bool> handler_called)
    {
        return [handler_called](
                   const rpc::shared_ptr<yyy::i_host>& remote_host,
                   const std::shared_ptr<rpc::service>& service) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
        {
            handler_called->store(true, std::memory_order_release);
            auto example = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(service, remote_host));
            CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(example)};
        };
    }

#    ifdef CANOPY_BUILD_HTTP_SERVER
    auto websocket_upgrade_request(
        std::string method = "GET",
        std::string key = "dGhlIHNhbXBsZSBub25jZQ==",
        std::string version = "13",
        std::string connection = "keep-alive, Upgrade") -> std::string
    {
        return method + " /rpc HTTP/1.1\r\n" + "Host: example.test\r\n" + "Upgrade: websocket\r\n"
               + "Connection: " + connection + "\r\n" + "Sec-WebSocket-Key: " + key + "\r\n"
               + "Sec-WebSocket-Version: " + version + "\r\n" + "\r\n";
    }

    auto run_http_upgrade_request(
        const std::string& wire_request,
        std::shared_ptr<std::atomic_bool> upgrade_handler_called) -> std::vector<std::vector<uint8_t>>
    {
        auto stream = std::make_shared<scripted_stream>();
        stream->push(std::vector<uint8_t>(wire_request.begin(), wire_request.end()));

        canopy::http_server::handler_set handlers;
        handlers.websocket_upgrade_handler
            = [upgrade_handler_called](
                  const canopy::http_server::request& request,
                  std::shared_ptr<streaming::stream> websocket_stream) -> CORO_TASK(std::shared_ptr<rpc::transport>)
        {
            std::ignore = request;
            std::ignore = websocket_stream;
            upgrade_handler_called->store(true, std::memory_order_release);
            CO_RETURN nullptr;
        };

        canopy::http_server::client_connection connection(stream, std::move(handlers));
        (void)coro::sync_wait(connection.handle());
        return stream->sent_messages();
    }

    auto sent_http_text(const std::vector<std::vector<uint8_t>>& sent) -> std::string
    {
        std::string output;
        for (const auto& message : sent)
            output.append(reinterpret_cast<const char*>(message.data()), message.size());
        return output;
    }
#    endif
#  endif

    CORO_TASK(bool) coro_malformed_init_message_disconnects_transport(std::shared_ptr<coro::scheduler> scheduler)
    {
        auto zone_id = rpc::DEFAULT_PREFIX;
        (void)zone_id.set_subnet(zone_id.get_subnet() + 4096);
        auto service = rpc::root_service::create("bad_init_transport_test", zone_id, scheduler);

        auto queues = rpc::spsc_queue::queue_pair::create();
        auto peer_stream = std::make_shared<::streaming::spsc_queue::stream>(
            queues.connect_to_accept, queues.accept_to_connect, scheduler);
        auto transport_stream = std::make_shared<::streaming::spsc_queue::stream>(
            queues.accept_to_connect, queues.connect_to_accept, scheduler);

        auto handler_called = std::make_shared<std::atomic_bool>(false);
        auto transport = rpc::stream_transport::create(
            "bad_init_responder",
            service,
            std::move(transport_stream),
            [handler_called](rpc::connection_settings, std::shared_ptr<rpc::service>, std::shared_ptr<rpc::transport>)
                -> CORO_TASK(rpc::connection_handler_result)
            {
                handler_called->store(true, std::memory_order_release);
                CO_RETURN rpc::connection_handler_result{rpc::error::OK(), {}};
            },
            rpc::stream_transport::stream_transport_options{
                .call_timeout = std::chrono::milliseconds{0},
                .call_timeout_sweep = std::chrono::milliseconds{0},
                .shutdown_timeout = std::chrono::milliseconds{1},
            });

        CO_AWAIT service->schedule();

        rpc::stream_transport::envelope_payload payload{
            FLD(payload_fingerprint) rpc::id<rpc::stream_transport::init_client_channel_send>::get(rpc::get_version()),
            FLD(payload) std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef},
        };
        auto payload_bytes = rpc::to_yas_binary(payload);
        rpc::stream_transport::envelope_prefix prefix{
            FLD(version) rpc::get_version(),
            FLD(direction) rpc::stream_transport::message_direction::send,
            FLD(sequence_number) uint64_t{1},
            FLD(payload_size) payload_bytes.size(),
        };
        auto prefix_bytes = rpc::to_yas_binary(prefix);

        auto send_status = CO_AWAIT peer_stream->send(as_byte_span(prefix_bytes));
        CORO_ASSERT_EQ(send_status.is_ok(), true);
        send_status = CO_AWAIT peer_stream->send(as_byte_span(payload_bytes));
        CORO_ASSERT_EQ(send_status.is_ok(), true);

        bool saw_disconnect_state = false;
        for (int i = 0; i < 2000 && transport->get_status() != rpc::transport_status::DISCONNECTED; ++i)
        {
            if (transport->get_status() >= rpc::transport_status::DISCONNECTING)
                saw_disconnect_state = true;
            CO_AWAIT service->schedule();
        }
        saw_disconnect_state = saw_disconnect_state || transport->get_status() >= rpc::transport_status::DISCONNECTING;

        CORO_ASSERT_EQ(handler_called->load(std::memory_order_acquire), false);
        CORO_ASSERT_EQ(saw_disconnect_state, true);
        CORO_ASSERT_EQ(transport->get_status(), rpc::transport_status::DISCONNECTED);

        for (int i = 0; i < 8; ++i)
            CO_AWAIT service->schedule();

        transport.reset();
        peer_stream.reset();
        service.reset();
        CO_RETURN true;
    }
}

TEST(
    StreamingTransportBadMessage,
    MalformedInitClientChannelSendDisconnects)
{
    auto scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
        coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::manual,
            .pool = coro::thread_pool::options{.thread_count = 1}}));

    std::atomic_bool done{false};
    bool passed = false;
    auto runner = [&]() -> coro::task<void>
    {
        passed = CO_AWAIT coro_malformed_init_message_disconnects_transport(scheduler);
        done.store(true, std::memory_order_release);
        CO_RETURN;
    };

    ASSERT_TRUE(scheduler->spawn_detached(runner()));

    for (int i = 0; i < 3000 && !done.load(std::memory_order_acquire); ++i)
        scheduler->process_events(std::chrono::milliseconds{1});

    EXPECT_TRUE(done.load(std::memory_order_acquire));
    EXPECT_TRUE(passed);

    for (int i = 0; i < 100 && !scheduler->empty(); ++i)
        scheduler->process_events(std::chrono::milliseconds{1});
}

#  ifdef CANOPY_BUILD_WEBSOCKET
TEST(
    UntrustedWebFactory,
    RejectsMissingStreamAndHandler)
{
    auto result = coro::sync_wait(rpc::untrusted_web::accept_transport(nullptr, {}));
    EXPECT_EQ(result.error_code, rpc::error::INVALID_DATA());
    EXPECT_FALSE(result.service);
    EXPECT_FALSE(result.transport);
}

TEST(
    WebSocketPenetration,
    MaskedClientBinaryFrameDeliversPayload)
{
    auto underlying = std::make_shared<scripted_stream>();
    const std::string payload = "valid binary";
    underlying->push(make_client_frame(0x2, std::vector<uint8_t>(payload.begin(), payload.end())));

    auto websocket
        = std::make_shared<streaming::websocket::stream>(underlying, rpc::websocket_stream::endpoint_role::server);

    std::array<uint8_t, 64> buffer{};
    auto [status, received]
        = coro::sync_wait(websocket->receive(rpc::mutable_byte_span{buffer}, std::chrono::milliseconds{10}));

    ASSERT_TRUE(status.is_ok());
    ASSERT_EQ(received.size(), payload.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(received.data()), received.size()), payload);
    EXPECT_FALSE(websocket->is_closed());
}

TEST(
    WebSocketPenetration,
    UnmaskedClientFrameClosesServerStream)
{
    auto underlying = std::make_shared<scripted_stream>();
    underlying->push(make_client_frame(0x2, {}, false));

    auto websocket
        = std::make_shared<streaming::websocket::stream>(underlying, rpc::websocket_stream::endpoint_role::server);

    std::array<uint8_t, 8> buffer{};
    auto [status, received]
        = coro::sync_wait(websocket->receive(rpc::mutable_byte_span{buffer}, std::chrono::milliseconds{10}));

    EXPECT_TRUE(status.is_closed());
    EXPECT_TRUE(received.empty());
    EXPECT_TRUE(websocket->is_closed());
}

TEST(
    WebSocketPenetration,
    OversizedControlFrameClosesServerStream)
{
    auto underlying = std::make_shared<scripted_stream>();
    underlying->push(make_client_frame(0x9, std::vector<uint8_t>(126, 0x41)));

    auto websocket
        = std::make_shared<streaming::websocket::stream>(underlying, rpc::websocket_stream::endpoint_role::server);

    std::array<uint8_t, 8> buffer{};
    auto [status, received]
        = coro::sync_wait(websocket->receive(rpc::mutable_byte_span{buffer}, std::chrono::milliseconds{10}));

    EXPECT_TRUE(status.is_closed());
    EXPECT_TRUE(received.empty());
    EXPECT_TRUE(websocket->is_closed());
}

TEST(
    WebSocketPenetration,
    OversizedMessageClosesServerStream)
{
    auto underlying = std::make_shared<scripted_stream>();
    underlying->push(make_client_frame(0x2, std::vector<uint8_t>(9, 0x41)));

    rpc::websocket_stream::stream_settings options;
    options.role = rpc::websocket_stream::endpoint_role::server;
    options.max_message_bytes = 8;
    auto websocket = std::make_shared<streaming::websocket::stream>(underlying, options);

    std::array<uint8_t, 16> buffer{};
    auto [status, received]
        = coro::sync_wait(websocket->receive(rpc::mutable_byte_span{buffer}, std::chrono::milliseconds{10}));

    EXPECT_TRUE(status.is_closed());
    EXPECT_TRUE(received.empty());
    EXPECT_TRUE(websocket->is_closed());
}

TEST(
    WebSocketPenetration,
    DecodedQueueLimitClosesServerStream)
{
    auto underlying = std::make_shared<scripted_stream>();
    auto first = make_client_frame(0x2, std::vector<uint8_t>{0x01});
    auto second = make_client_frame(0x2, std::vector<uint8_t>{0x02});
    first.insert(first.end(), second.begin(), second.end());
    underlying->push(std::move(first));

    rpc::websocket_stream::stream_settings options;
    options.role = rpc::websocket_stream::endpoint_role::server;
    options.max_decoded_messages = 1;
    auto websocket = std::make_shared<streaming::websocket::stream>(underlying, options);

    std::array<uint8_t, 16> buffer{};
    auto [status, received]
        = coro::sync_wait(websocket->receive(rpc::mutable_byte_span{buffer}, std::chrono::milliseconds{10}));

    EXPECT_TRUE(status.is_closed());
    EXPECT_TRUE(received.empty());
    EXPECT_TRUE(websocket->is_closed());
}

#    ifdef CANOPY_BUILD_HTTP_SERVER
TEST(
    HttpWebSocketUpgradePenetration,
    ValidUpgradeUsesHandler)
{
    auto called = std::make_shared<std::atomic_bool>(false);
    auto response = sent_http_text(run_http_upgrade_request(websocket_upgrade_request(), called));

    EXPECT_TRUE(called->load(std::memory_order_acquire));
    EXPECT_EQ(response.find("HTTP/1.1 101 Switching Protocols\r\n"), size_t{0});
    EXPECT_NE(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
}

TEST(
    HttpWebSocketUpgradePenetration,
    InvalidClientKeyRejectsBeforeHandler)
{
    auto called = std::make_shared<std::atomic_bool>(false);
    auto response = sent_http_text(run_http_upgrade_request(websocket_upgrade_request("GET", "not-a-valid-key"), called));

    EXPECT_FALSE(called->load(std::memory_order_acquire));
    EXPECT_EQ(response.find("HTTP/1.1 400 Bad Request\r\n"), size_t{0});
}

TEST(
    HttpWebSocketUpgradePenetration,
    MissingVersionRejectsBeforeHandler)
{
    auto called = std::make_shared<std::atomic_bool>(false);
    auto request = std::string("GET /rpc HTTP/1.1\r\n") + "Host: example.test\r\n" + "Upgrade: websocket\r\n"
                   + "Connection: Upgrade\r\n" + "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" + "\r\n";
    auto response = sent_http_text(run_http_upgrade_request(request, called));

    EXPECT_FALSE(called->load(std::memory_order_acquire));
    EXPECT_EQ(response.find("HTTP/1.1 400 Bad Request\r\n"), size_t{0});
}

TEST(
    HttpWebSocketUpgradePenetration,
    PostUpgradeRejectsBeforeHandler)
{
    auto called = std::make_shared<std::atomic_bool>(false);
    auto response = sent_http_text(run_http_upgrade_request(websocket_upgrade_request("POST"), called));

    EXPECT_FALSE(called->load(std::memory_order_acquire));
    EXPECT_EQ(response.find("HTTP/1.1 400 Bad Request\r\n"), size_t{0});
}

TEST(
    HttpWebSocketUpgradePenetration,
    BodyBearingUpgradeRejectsBeforeHandler)
{
    auto called = std::make_shared<std::atomic_bool>(false);
    auto request = std::string("GET /rpc HTTP/1.1\r\n") + "Host: example.test\r\n" + "Upgrade: websocket\r\n"
                   + "Connection: Upgrade\r\n" + "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                   + "Sec-WebSocket-Version: 13\r\n" + "Content-Length: 1\r\n" + "\r\n" + "x";
    auto response = sent_http_text(run_http_upgrade_request(request, called));

    EXPECT_FALSE(called->load(std::memory_order_acquire));
    EXPECT_EQ(response.find("HTTP/1.1 400 Bad Request\r\n"), size_t{0});
}

TEST(
    HttpWebSocketUpgradePenetration,
    OversizedHeaderRejectsBeforeHandler)
{
    auto called = std::make_shared<std::atomic_bool>(false);
    auto request = std::string("GET /rpc HTTP/1.1\r\n") + "Host: example.test\r\n" + "Upgrade: websocket\r\n"
                   + "Connection: Upgrade\r\n" + "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                   + "Sec-WebSocket-Version: 13\r\n" + "X-Large: " + std::string(9000, 'a') + "\r\n" + "\r\n";
    auto response = sent_http_text(run_http_upgrade_request(request, called));

    EXPECT_FALSE(called->load(std::memory_order_acquire));
    EXPECT_EQ(response.find("HTTP/1.1 400 Bad Request\r\n"), size_t{0});
}

TEST(
    HttpWebSocketUpgradePenetration,
    OversizedPendingInputRejectsBeforeHandler)
{
    auto called = std::make_shared<std::atomic_bool>(false);
    auto response = sent_http_text(run_http_upgrade_request(std::string(70 * 1024, 'a'), called));

    EXPECT_FALSE(called->load(std::memory_order_acquire));
    EXPECT_EQ(response.find("HTTP/1.1 400 Bad Request\r\n"), size_t{0});
}
#    endif

TEST(
    UntrustedWebPenetration,
    MalformedHandshakeClosesWithoutCallingHandler)
{
    auto scheduler = make_test_scheduler();
    auto stream = std::make_shared<scripted_stream>();
    stream->push(std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef});

    rpc::untrusted_web::transport_settings settings;
    settings.receive_poll_timeout_ms = 1;
    settings.handshake_timeout_ms = 50;

    auto accepted = std::make_shared<rpc::untrusted_web::accept_result>();
    auto accepted_done = std::make_shared<std::atomic_bool>(false);
    auto handler_called = std::make_shared<std::atomic_bool>(false);
    auto service = start_untrusted_web_accept(
        scheduler, stream, settings, accepted_done, accepted, make_untrusted_web_example_factory(handler_called));

    ASSERT_TRUE(process_until(scheduler, [&] { return accepted_done->load(std::memory_order_acquire); }));
    ASSERT_EQ(accepted->error_code, rpc::error::OK());
    ASSERT_TRUE(accepted->transport);

    EXPECT_TRUE(process_until(
        scheduler, [&] { return accepted->transport->get_status() == rpc::transport_status::DISCONNECTED; }));
    EXPECT_FALSE(handler_called->load(std::memory_order_acquire));
    EXPECT_TRUE(stream->is_closed());
    service.reset();
}

TEST(
    UntrustedWebPenetration,
    OversizedHandshakeClosesWithoutCallingHandler)
{
    auto scheduler = make_test_scheduler();
    auto stream = std::make_shared<scripted_stream>();
    stream->push(std::vector<uint8_t>(32, 0x7f));

    rpc::untrusted_web::transport_settings settings;
    settings.max_handshake_bytes = 8;
    settings.receive_poll_timeout_ms = 1;
    settings.handshake_timeout_ms = 50;

    auto accepted = std::make_shared<rpc::untrusted_web::accept_result>();
    auto accepted_done = std::make_shared<std::atomic_bool>(false);
    auto handler_called = std::make_shared<std::atomic_bool>(false);
    auto service = start_untrusted_web_accept(
        scheduler, stream, settings, accepted_done, accepted, make_untrusted_web_example_factory(handler_called));

    ASSERT_TRUE(process_until(scheduler, [&] { return accepted_done->load(std::memory_order_acquire); }));
    ASSERT_EQ(accepted->error_code, rpc::error::OK());
    ASSERT_TRUE(accepted->transport);

    EXPECT_TRUE(process_until(
        scheduler, [&] { return accepted->transport->get_status() == rpc::transport_status::DISCONNECTED; }));
    EXPECT_FALSE(handler_called->load(std::memory_order_acquire));
    EXPECT_TRUE(stream->is_closed());
    service.reset();
}

TEST(
    UntrustedWebPenetration,
    BackChannelInjectionIsNotReflected)
{
    auto scheduler = make_test_scheduler();
    auto stream = std::make_shared<scripted_stream>();
    stream->push(make_handshake_message());

    rpc::untrusted_web::transport_settings settings;
    settings.receive_poll_timeout_ms = 1;
    settings.handshake_timeout_ms = 50;
    settings.inactivity_timeout_ms = 0;

    auto accepted = std::make_shared<rpc::untrusted_web::accept_result>();
    auto accepted_done = std::make_shared<std::atomic_bool>(false);
    auto handler_called = std::make_shared<std::atomic_bool>(false);
    auto service = start_untrusted_web_accept(
        scheduler, stream, settings, accepted_done, accepted, make_untrusted_web_example_factory(handler_called));

    ASSERT_TRUE(process_until(scheduler, [&] { return accepted_done->load(std::memory_order_acquire); }));
    ASSERT_EQ(accepted->error_code, rpc::error::OK());
    ASSERT_TRUE(accepted->transport);

    ASSERT_TRUE(process_until(
        scheduler,
        [&]
        {
            return handler_called->load(std::memory_order_acquire)
                   && parse_handshake_complete_destination(stream->sent_messages()).has_value();
        }));

    auto destination = parse_handshake_complete_destination(stream->sent_messages());
    ASSERT_TRUE(destination.has_value());

    stream->push(make_back_channel_injection_message(*destination));
    ASSERT_TRUE(process_until(scheduler, [&] { return parse_response_with_id(stream->sent_messages(), 2).has_value(); }));

    auto response = parse_response_with_id(stream->sent_messages(), 2);
    ASSERT_TRUE(response.has_value());
    EXPECT_TRUE(response->back_channel.empty());

    coro::sync_wait(stream->set_closed());
    process_until(
        scheduler, [&] { return accepted->transport->get_status() == rpc::transport_status::DISCONNECTED; }, 1000);
    service.reset();
}

TEST(
    UntrustedWebPenetration,
    OversizedPostHandshakeEnvelopeCloses)
{
    auto scheduler = make_test_scheduler();
    auto stream = std::make_shared<scripted_stream>();
    stream->push(make_handshake_message());

    rpc::untrusted_web::transport_settings settings;
    settings.max_envelope_bytes = 8;
    settings.receive_poll_timeout_ms = 1;
    settings.handshake_timeout_ms = 50;
    settings.inactivity_timeout_ms = 0;

    auto accepted = std::make_shared<rpc::untrusted_web::accept_result>();
    auto accepted_done = std::make_shared<std::atomic_bool>(false);
    auto handler_called = std::make_shared<std::atomic_bool>(false);
    auto service = start_untrusted_web_accept(
        scheduler, stream, settings, accepted_done, accepted, make_untrusted_web_example_factory(handler_called));

    ASSERT_TRUE(process_until(scheduler, [&] { return accepted_done->load(std::memory_order_acquire); }));
    ASSERT_TRUE(process_until(
        scheduler,
        [&]
        {
            return handler_called->load(std::memory_order_acquire)
                   && parse_handshake_complete_destination(stream->sent_messages()).has_value();
        }));

    stream->push(std::vector<uint8_t>(settings.max_envelope_bytes + 1, 0x55));
    EXPECT_TRUE(process_until(
        scheduler, [&] { return accepted->transport->get_status() == rpc::transport_status::DISCONNECTED; }));
    service.reset();
}

TEST(
    UntrustedWebPenetration,
    OversizedInnerPayloadCloses)
{
    auto scheduler = make_test_scheduler();
    auto stream = std::make_shared<scripted_stream>();
    stream->push(make_handshake_message());

    rpc::untrusted_web::transport_settings settings;
    settings.max_request_payload_bytes = 8;
    settings.receive_poll_timeout_ms = 1;
    settings.handshake_timeout_ms = 50;
    settings.inactivity_timeout_ms = 0;

    auto accepted = std::make_shared<rpc::untrusted_web::accept_result>();
    auto accepted_done = std::make_shared<std::atomic_bool>(false);
    auto handler_called = std::make_shared<std::atomic_bool>(false);
    auto service = start_untrusted_web_accept(
        scheduler, stream, settings, accepted_done, accepted, make_untrusted_web_example_factory(handler_called));

    ASSERT_TRUE(process_until(scheduler, [&] { return accepted_done->load(std::memory_order_acquire); }));
    ASSERT_TRUE(process_until(
        scheduler,
        [&]
        {
            return handler_called->load(std::memory_order_acquire)
                   && parse_handshake_complete_destination(stream->sent_messages()).has_value();
        }));

    auto destination = parse_handshake_complete_destination(stream->sent_messages());
    ASSERT_TRUE(destination.has_value());

    stream->push(make_untrusted_web_request_message(
        *destination, websocket_protocol::v1::message_type::send, false, settings.max_request_payload_bytes + 1));
    EXPECT_TRUE(process_until(
        scheduler, [&] { return accepted->transport->get_status() == rpc::transport_status::DISCONNECTED; }));
    service.reset();
}

TEST(
    UntrustedWebPenetration,
    UnexpectedEnvelopeTypeAfterHandshakeCloses)
{
    auto scheduler = make_test_scheduler();
    auto stream = std::make_shared<scripted_stream>();
    stream->push(make_handshake_message());

    rpc::untrusted_web::transport_settings settings;
    settings.receive_poll_timeout_ms = 1;
    settings.handshake_timeout_ms = 50;
    settings.inactivity_timeout_ms = 0;

    auto accepted = std::make_shared<rpc::untrusted_web::accept_result>();
    auto accepted_done = std::make_shared<std::atomic_bool>(false);
    auto handler_called = std::make_shared<std::atomic_bool>(false);
    auto service = start_untrusted_web_accept(
        scheduler, stream, settings, accepted_done, accepted, make_untrusted_web_example_factory(handler_called));

    ASSERT_TRUE(process_until(scheduler, [&] { return accepted_done->load(std::memory_order_acquire); }));
    ASSERT_TRUE(process_until(
        scheduler,
        [&]
        {
            return handler_called->load(std::memory_order_acquire)
                   && parse_handshake_complete_destination(stream->sent_messages()).has_value();
        }));

    stream->push(make_envelope_message(websocket_protocol::v1::message_type::handshake, {}, 3));
    EXPECT_TRUE(process_until(
        scheduler, [&] { return accepted->transport->get_status() == rpc::transport_status::DISCONNECTED; }));
    service.reset();
}

TEST(
    UntrustedWebPenetration,
    DecodeFailureLimitClosesAfterConfiguredFailures)
{
    auto scheduler = make_test_scheduler();
    auto stream = std::make_shared<scripted_stream>();
    stream->push(make_handshake_message());

    rpc::untrusted_web::transport_settings settings;
    settings.max_decode_failures = 2;
    settings.close_on_protocol_error = false;
    settings.receive_poll_timeout_ms = 1;
    settings.handshake_timeout_ms = 50;
    settings.inactivity_timeout_ms = 0;

    auto accepted = std::make_shared<rpc::untrusted_web::accept_result>();
    auto accepted_done = std::make_shared<std::atomic_bool>(false);
    auto handler_called = std::make_shared<std::atomic_bool>(false);
    auto service = start_untrusted_web_accept(
        scheduler, stream, settings, accepted_done, accepted, make_untrusted_web_example_factory(handler_called));

    ASSERT_TRUE(process_until(scheduler, [&] { return accepted_done->load(std::memory_order_acquire); }));
    ASSERT_TRUE(process_until(
        scheduler,
        [&]
        {
            return handler_called->load(std::memory_order_acquire)
                   && parse_handshake_complete_destination(stream->sent_messages()).has_value();
        }));

    stream->push(std::vector<uint8_t>{0xde, 0xad});
    for (int i = 0; i < 100; ++i)
        scheduler->process_events(std::chrono::milliseconds{1});
    EXPECT_NE(accepted->transport->get_status(), rpc::transport_status::DISCONNECTED);

    stream->push(std::vector<uint8_t>{0xbe, 0xef});
    EXPECT_TRUE(process_until(
        scheduler, [&] { return accepted->transport->get_status() == rpc::transport_status::DISCONNECTED; }));
    service.reset();
}

TEST(
    UntrustedWebPenetration,
    InactivityTimeoutCloses)
{
    auto scheduler = make_test_scheduler();
    auto stream = std::make_shared<scripted_stream>();
    stream->push(make_handshake_message());

    rpc::untrusted_web::transport_settings settings;
    settings.receive_poll_timeout_ms = 1;
    settings.handshake_timeout_ms = 50;
    settings.inactivity_timeout_ms = 5;

    auto accepted = std::make_shared<rpc::untrusted_web::accept_result>();
    auto accepted_done = std::make_shared<std::atomic_bool>(false);
    auto handler_called = std::make_shared<std::atomic_bool>(false);
    auto service = start_untrusted_web_accept(
        scheduler, stream, settings, accepted_done, accepted, make_untrusted_web_example_factory(handler_called));

    ASSERT_TRUE(process_until(scheduler, [&] { return accepted_done->load(std::memory_order_acquire); }));
    ASSERT_TRUE(process_until(
        scheduler,
        [&]
        {
            return handler_called->load(std::memory_order_acquire)
                   && parse_handshake_complete_destination(stream->sent_messages()).has_value();
        }));

    EXPECT_TRUE(process_until(
        scheduler, [&] { return accepted->transport->get_status() == rpc::transport_status::DISCONNECTED; }, 6000));
    service.reset();
}

TEST(
    UntrustedWebPenetration,
    PostBackChannelInjectionDoesNotRespondOrClose)
{
    auto scheduler = make_test_scheduler();
    auto stream = std::make_shared<scripted_stream>();
    stream->push(make_handshake_message());

    rpc::untrusted_web::transport_settings settings;
    settings.receive_poll_timeout_ms = 1;
    settings.handshake_timeout_ms = 50;
    settings.inactivity_timeout_ms = 0;

    auto accepted = std::make_shared<rpc::untrusted_web::accept_result>();
    auto accepted_done = std::make_shared<std::atomic_bool>(false);
    auto handler_called = std::make_shared<std::atomic_bool>(false);
    auto service = start_untrusted_web_accept(
        scheduler, stream, settings, accepted_done, accepted, make_untrusted_web_example_factory(handler_called));

    ASSERT_TRUE(process_until(scheduler, [&] { return accepted_done->load(std::memory_order_acquire); }));
    ASSERT_TRUE(process_until(
        scheduler,
        [&]
        {
            return handler_called->load(std::memory_order_acquire)
                   && parse_handshake_complete_destination(stream->sent_messages()).has_value();
        }));

    auto destination = parse_handshake_complete_destination(stream->sent_messages());
    ASSERT_TRUE(destination.has_value());

    stream->push(make_untrusted_web_request_message(*destination, websocket_protocol::v1::message_type::post, true, 0, 44));
    for (int i = 0; i < 200; ++i)
        scheduler->process_events(std::chrono::milliseconds{1});

    EXPECT_FALSE(parse_response_with_id(stream->sent_messages(), 44).has_value());
    EXPECT_NE(accepted->transport->get_status(), rpc::transport_status::DISCONNECTED);

    coro::sync_wait(stream->set_closed());
    process_until(
        scheduler, [&] { return accepted->transport->get_status() == rpc::transport_status::DISCONNECTED; }, 1000);
    service.reset();
}
#  endif

// Keep this suite on the active TCP coroutine and SPSC streaming paths.
using streaming_transport_regression_implementations = ::testing::Types<
    streaming_tcp_coroutine_setup<false, false, false>,
    streaming_spsc_setup<false, false, false>,
    streaming_layered_tcp_coroutine_setup<false, false, false>,
    streaming_layered_spsc_setup<false, false, false>>;

TYPED_TEST_SUITE(
    streaming_transport_regression_test,
    streaming_transport_regression_implementations);

template<class T> CORO_TASK(bool) coro_large_blob_round_trip_progress(T& lib)
{
    rpc::shared_ptr<xxx::i_baz> baz_ptr;
    CORO_ASSERT_EQ(CO_AWAIT lib.get_example()->create_baz(baz_ptr), rpc::error::OK());
    CORO_ASSERT_NE(baz_ptr, nullptr);

    std::vector<uint8_t> input(256 * 1024);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<uint8_t>(i & 0xFF);

    constexpr int rounds = 4;
    for (int round = 0; round < rounds; ++round)
    {
        std::vector<uint8_t> output;
        CORO_ASSERT_EQ(CO_AWAIT baz_ptr->blob_test(input, output), rpc::error::OK());
        CORO_ASSERT_EQ(output, input);
    }

    baz_ptr = nullptr;
    CO_RETURN true;
}

TYPED_TEST(
    streaming_transport_regression_test,
    large_blob_round_trip_progress)
{
    run_coro_test(*this, [](auto& lib) { return coro_large_blob_round_trip_progress<TypeParam>(lib); });
}

// One concurrent add() call. All parameters are moved into the coroutine frame
// by value so there is no dangling reference to a temporary lambda closure.
static CORO_TASK(void) do_add_call(
    rpc::shared_ptr<yyy::i_example> example,
    int a,
    std::atomic<int>* completed,
    std::atomic<int>* errors)
{
    int result = 0;
    int ret = CO_AWAIT example->add(a, a + 1, result);
    if (ret == rpc::error::OK() && result == a + a + 1)
        completed->fetch_add(1, std::memory_order_relaxed);
    else
        errors->fetch_add(1, std::memory_order_relaxed);
    CO_RETURN;
}

// Exercises the send_queue_ready_ wakeup path in send_producer_loop.
// Spawning many concurrent calls on a single-threaded cooperative scheduler
// causes multiple outbound messages to queue before the send loop drains them;
// the loop must wake from send_queue_ready_ and process all of them without
// dropping or deadlocking.
template<class T> CORO_TASK(bool) coro_concurrent_queued_sends(T& lib)
{
    constexpr int kConcurrentCalls = 16;
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};

    auto example = lib.get_example();
    auto svc = lib.get_root_service();

    for (int i = 0; i < kConcurrentCalls; ++i)
        svc->spawn(do_add_call(example, i, &completed, &errors));

    while (completed.load(std::memory_order_acquire) + errors.load(std::memory_order_acquire) < kConcurrentCalls)
        CO_AWAIT svc->schedule();

    CORO_ASSERT_EQ(errors.load(), 0);
    CORO_ASSERT_EQ(completed.load(), kConcurrentCalls);
    CO_RETURN true;
}

TYPED_TEST(
    streaming_transport_regression_test,
    concurrent_queued_sends)
{
    run_coro_test(*this, [](auto& lib) { return coro_concurrent_queued_sends<TypeParam>(lib); });
}
#endif
