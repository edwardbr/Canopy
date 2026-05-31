/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>

#include <canopy/http_server/http_client_connection.h>
#include <rpc/internal/serialiser.h>
#include <streaming/websocket/stream.h>
#include <transports/untrusted_web/factory.h>
#include <websocket/websocket_protocol.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <vector>

#include "gtest/gtest.h"

namespace
{
    class scripted_stream final : public streaming::stream
    {
    public:
        void push(std::vector<uint8_t> payload)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            incoming_.push_back(chunk{std::move(payload), 0});
        }

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

    auto random_bytes(
        std::mt19937_64& rng,
        size_t max_size) -> std::vector<uint8_t>
    {
        std::uniform_int_distribution<size_t> size_dist(0, max_size);
        std::uniform_int_distribution<unsigned> byte_dist(0, 255);

        std::vector<uint8_t> bytes(size_dist(rng));
        for (auto& byte : bytes)
            byte = static_cast<uint8_t>(byte_dist(rng));
        return bytes;
    }

    auto random_httpish_request(
        std::mt19937_64& rng,
        size_t index) -> std::vector<uint8_t>
    {
        std::uniform_int_distribution<unsigned> byte_dist(0, 255);
        std::uniform_int_distribution<int> header_count_dist(0, 24);
        std::uniform_int_distribution<int> value_size_dist(0, 512);

        std::string request;
        switch (index % 8)
        {
        case 0:
            request = "GET /rpc HTTP/1.1\r\n";
            break;
        case 1:
            request = "POST /rpc HTTP/1.1\r\n";
            break;
        case 2:
            request = "GET /api/status HTTP/1.1\r\n";
            break;
        case 3:
            request = "GET /rpc HTTP/1.0\r\n";
            break;
        default:
            request = "GET /";
            request.push_back(static_cast<char>('a' + (index % 26)));
            request.append(" HTTP/1.1\r\n");
            break;
        }

        for (int i = 0; i < header_count_dist(rng); ++i)
        {
            request += "X-Fuzz-";
            request += std::to_string(i);
            request += ": ";
            const auto value_size = value_size_dist(rng);
            for (int j = 0; j < value_size; ++j)
            {
                auto byte = static_cast<unsigned char>(byte_dist(rng));
                request.push_back(byte >= 32 && byte < 127 ? static_cast<char>(byte) : 'x');
            }
            request += "\r\n";
        }

        if (index % 5 == 0)
        {
            request += "Host: example.test\r\n";
            request += "Upgrade: websocket\r\n";
            request += "Connection: keep-alive, Upgrade\r\n";
            request += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
            request += "Sec-WebSocket-Version: 13\r\n";
        }

        if (index % 3 != 0)
            request += "\r\n";

        return std::vector<uint8_t>(request.begin(), request.end());
    }

    auto make_client_frame(
        uint8_t opcode,
        std::vector<uint8_t> payload,
        bool masked,
        bool fin) -> std::vector<uint8_t>
    {
        std::vector<uint8_t> frame;
        frame.push_back(static_cast<uint8_t>((fin ? 0x80 : 0x00) | (opcode & 0x0f)));
        const auto mask_bit = masked ? 0x80 : 0x00;
        if (payload.size() <= 125)
        {
            frame.push_back(static_cast<uint8_t>(mask_bit | payload.size()));
        }
        else if (payload.size() <= 0xffff)
        {
            frame.push_back(static_cast<uint8_t>(mask_bit | 126));
            frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xff));
            frame.push_back(static_cast<uint8_t>(payload.size() & 0xff));
        }
        else
        {
            frame.push_back(static_cast<uint8_t>(mask_bit | 127));
            for (int shift = 56; shift >= 0; shift -= 8)
                frame.push_back(static_cast<uint8_t>((static_cast<uint64_t>(payload.size()) >> shift) & 0xff));
        }

        const std::array<uint8_t, 4> mask{{0x12, 0x34, 0x56, 0x78}};
        if (!masked)
        {
            frame.insert(frame.end(), payload.begin(), payload.end());
            return frame;
        }

        frame.insert(frame.end(), mask.begin(), mask.end());
        for (size_t i = 0; i < payload.size(); ++i)
            frame.push_back(static_cast<uint8_t>(payload[i] ^ mask[i % mask.size()]));
        return frame;
    }

    constexpr rpc::encoding untrusted_web_test_encoding()
    {
#ifdef CANOPY_WEBSOCKET_ENCODING
        return CANOPY_WEBSOCKET_ENCODING;
#else
        return CANOPY_DEFAULT_ENCODING;
#endif
    }

    template<
        class OutputBlob = std::vector<uint8_t>,
        class T>
    OutputBlob encode_untrusted_web_message(const T& value)
    {
        return rpc::serialise<OutputBlob>(value, untrusted_web_test_encoding());
    }

    auto make_handshake_message() -> std::vector<uint8_t>
    {
        websocket_protocol::v1::connect_request request;
        request.inbound_interface_id = rpc::interface_ordinal{1};
        request.outbound_interface_id = rpc::interface_ordinal{2};
        request.remote_object_id.object_id = 1;

        websocket_protocol::v1::envelope envelope;
        envelope.id = 1;
        envelope.type = websocket_protocol::v1::message_type::handshake;
        envelope.data = encode_untrusted_web_message<std::vector<char>>(request);
        return encode_untrusted_web_message(envelope);
    }

    auto make_request_message(
        const rpc::remote_object& destination,
        std::vector<char> payload,
        uint64_t id) -> std::vector<uint8_t>
    {
        websocket_protocol::v1::request request;
        request.encoding = untrusted_web_test_encoding();
        request.tag = 77;
        request.destination_zone_id = rpc::to_zone_address_args(destination.get_address());
        request.interface_id = rpc::interface_ordinal{2};
        request.method_id = rpc::method{999999};
        request.data = std::move(payload);

        websocket_protocol::v1::envelope envelope;
        envelope.id = id;
        envelope.type
            = id % 3 == 0 ? websocket_protocol::v1::message_type::post : websocket_protocol::v1::message_type::send;
        envelope.data = encode_untrusted_web_message<std::vector<char>>(request);
        return encode_untrusted_web_message(envelope);
    }

    auto make_untrusted_web_handler() -> rpc::connection_handler
    {
        return [](rpc::connection_settings,
                   std::shared_ptr<rpc::service> service,
                   std::shared_ptr<rpc::transport>) -> CORO_TASK(rpc::connection_handler_result)
        {
            auto object = service->get_zone_id().with_object(rpc::object{1});
            if (!object)
                CO_RETURN rpc::connection_handler_result{rpc::error::INVALID_DATA(), {}};
            CO_RETURN rpc::connection_handler_result{rpc::error::OK(), *object};
        };
    }
} // namespace

TEST(
    UntrustedWebFuzz,
    HttpUpgradeParserRejectsRandomInputsWithoutHanging)
{
    std::mt19937_64 rng(0x71c0ffee5eedULL);

    for (size_t i = 0; i < 96; ++i)
    {
        auto stream = std::make_shared<scripted_stream>();
        if (i % 2 == 0)
            stream->push(random_bytes(rng, 4096));
        else
            stream->push(random_httpish_request(rng, i));

        std::atomic_bool upgrade_called{false};
        canopy::http_server::handler_set handlers;
        handlers.websocket_upgrade_handler
            = [&upgrade_called](
                  const canopy::http_server::request&,
                  std::shared_ptr<streaming::stream>) -> CORO_TASK(std::shared_ptr<rpc::transport>)
        {
            upgrade_called.store(true, std::memory_order_release);
            CO_RETURN nullptr;
        };

        canopy::http_server::client_connection_limits limits;
        limits.max_pending_input_bytes = 4096;
        limits.max_header_count = 16;
        limits.max_header_value_bytes = 256;
        limits.receive_poll_timeout_ms = 1;
        limits.header_timeout_ms = 1;
        limits.request_timeout_ms = 1;

        canopy::http_server::client_connection connection(stream, std::move(handlers), limits);
        std::ignore = coro::sync_wait(connection.handle());

        if (upgrade_called.load(std::memory_order_acquire))
        {
            auto sent = stream->sent_messages();
            ASSERT_FALSE(sent.empty());
        }
    }
}

TEST(
    UntrustedWebFuzz,
    WebSocketStreamRejectsRandomFramesWithoutHanging)
{
    std::mt19937_64 rng(0x51deca11ULL);
    std::uniform_int_distribution<int> opcode_dist(0, 15);
    std::bernoulli_distribution bool_dist(0.5);
    std::array<char, 256> receive_buffer{};

    for (size_t i = 0; i < 128; ++i)
    {
        auto underlying = std::make_shared<scripted_stream>();
        if (i % 3 == 0)
        {
            underlying->push(make_client_frame(
                static_cast<uint8_t>(opcode_dist(rng)), random_bytes(rng, 512), bool_dist(rng), bool_dist(rng)));
        }
        else
        {
            underlying->push(random_bytes(rng, 768));
        }

        rpc::websocket_stream::stream_settings settings;
        settings.role = rpc::websocket_stream::endpoint_role::server;
        settings.max_frame_payload_bytes = 256;
        settings.max_message_bytes = 512;
        settings.max_decoded_messages = 4;
        settings.keep_alive.enabled = false;

        streaming::websocket::stream websocket(underlying, settings);
        for (int attempt = 0; attempt < 3 && !websocket.is_closed(); ++attempt)
        {
            auto [status, span] = coro::sync_wait(websocket.receive(
                rpc::mutable_byte_span{receive_buffer.data(), receive_buffer.size()}, std::chrono::milliseconds{1}));
            std::ignore = status;
            std::ignore = span;
        }
    }
}

TEST(
    UntrustedWebFuzz,
    TransportRejectsRandomHandshakeAndEnvelopeInputsWithoutHanging)
{
    std::mt19937_64 rng(0x0badf00d1234ULL);

    for (size_t i = 0; i < 96; ++i)
    {
        auto scheduler = make_test_scheduler();
        auto service = rpc::root_service::create("untrusted_web_fuzz", rpc::DEFAULT_PREFIX, scheduler);
        auto stream = std::make_shared<scripted_stream>();

        if (i % 4 == 0)
        {
            stream->push(make_handshake_message());
            auto object = service->get_zone_id().with_object(rpc::object{1});
            ASSERT_TRUE(object);

            std::vector<char> payload;
            auto raw_payload = random_bytes(rng, 1024);
            payload.assign(
                reinterpret_cast<const char*>(raw_payload.data()),
                reinterpret_cast<const char*>(raw_payload.data() + raw_payload.size()));
            stream->push(make_request_message(*object, std::move(payload), i + 2));
        }
        else
        {
            stream->push(random_bytes(rng, 2048));
        }

        rpc::untrusted_web::transport_settings settings;
        settings.max_handshake_bytes = 1024;
        settings.max_envelope_bytes = 1024;
        settings.max_request_payload_bytes = 512;
        settings.max_decode_failures = 2;
        settings.close_on_protocol_error = (i % 2) == 0;
        settings.receive_poll_timeout_ms = 1;
        settings.handshake_timeout_ms = 10;
        settings.inactivity_timeout_ms = 10;

        auto accepted = coro::sync_wait(
            rpc::untrusted_web::accept_transport(stream, make_untrusted_web_handler(), settings, service));
        ASSERT_EQ(accepted.error_code, rpc::error::OK());
        ASSERT_TRUE(accepted.transport);

        ASSERT_TRUE(process_until(
            scheduler,
            [&]
            { return stream->is_closed() || accepted.transport->get_status() == rpc::transport_status::DISCONNECTED; },
            2000));

        service.reset();
    }
}
