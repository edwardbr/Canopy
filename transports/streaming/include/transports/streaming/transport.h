/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <queue>
#include <type_traits>
#include <unordered_map>

#include <coro/coro.hpp>
#include <rpc/rpc.h>
#include <stream_transport/stream_transport.h>
#include <streaming/stream.h>

namespace rpc::stream_transport
{
    class transport : public rpc::transport
    {
    public:
        enum class message_hook_result
        {
            unhandled,
            handled,
            rejected
        };

        using connection_handler = rpc::connection_handler;

    private:
        template<class ReceivePayload> struct peer_call_result
        {
            int error_code = rpc::error::OK();
            ReceivePayload payload;
        };

        struct result_listener
        {
            coro::event event;
            envelope_prefix prefix;
            envelope_payload payload;
            int error_code = rpc::error::OK();
            std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
            bool post_only = false;
        };

        struct activity_tracker;

        struct message_handler_context
        {
            std::shared_ptr<activity_tracker> tracker;
            envelope_prefix* prefix = nullptr;
            envelope_payload* payload = nullptr;
        };

        using custom_message_handler = std::function<CORO_TASK(message_hook_result)(message_handler_context context)>;

        using custom_outgoing_handler = std::function<bool(
            uint64_t protocol_version, message_direction direction, uint64_t sequence_number, uint64_t type_fingerprint, const void* typed_payload)>;

        std::unordered_map<uint64_t, result_listener*> pending_transmits_;
        std::mutex pending_transmits_mtx_;

        std::shared_ptr<streaming::stream> stream_;

        std::atomic<uint64_t> sequence_number_ = 0;

        std::queue<std::vector<uint8_t>> send_queue_;
        std::mutex send_queue_mtx_;

        connection_handler connection_handler_;
        std::vector<custom_message_handler> custom_message_handlers_;
        std::vector<custom_outgoing_handler> custom_outgoing_handlers_;
        stdex::member_ptr<transport> keep_alive_;

        std::shared_ptr<rpc::object_stub> stub_;

        std::atomic<bool> peer_requested_disconnection_ = false;
        std::atomic<bool> pumps_started_ = false;

        // Two-phase close protocol state
        std::atomic<bool> send_cleanup_done_ = false;
        std::chrono::steady_clock::time_point disconnecting_since_{};
        static constexpr uint32_t shutdown_timeout_ms_ = 5000;

        struct activity_tracker
        {
            std::shared_ptr<transport> transport;
            std::shared_ptr<rpc::service> svc; // kept here to keep the service alive
            ~activity_tracker() { svc->spawn(transport->cleanup(transport, svc)); }
        };

        transport(std::string name,
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<streaming::stream> stream,
            connection_handler handler);

        // Producer/consumer coroutines
        CORO_TASK(void) receive_consumer_loop(std::shared_ptr<activity_tracker> tracker);
        CORO_TASK(void) send_producer_loop(std::shared_ptr<activity_tracker> tracker);
        CORO_TASK(void) flush_send_queue();

        // Stub handlers (called when receiving messages)
        CORO_TASK(void)
        stub_handle_send(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_try_cast(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_add_ref(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_release(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_post(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_object_released(
            std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_transport_down(
            std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        create_stub(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(message_hook_result)
        run_custom_message_handlers(message_handler_context context);
        CORO_TASK(bool) dispatch_builtin_message(message_handler_context context);

        template<class SendPayload>
        void send_payload(
            std::uint64_t protocol_version, message_direction direction, SendPayload&& sendPayload, uint64_t sequence_number)
        {
            assert(direction);
            using payload_type = std::remove_cvref_t<SendPayload>;

            envelope_payload payload_envelope = {.payload_fingerprint = rpc::id<payload_type>::get(protocol_version),
                .payload = rpc::to_yas_binary(sendPayload)};
            auto payload = rpc::to_yas_binary(payload_envelope);

            auto prefix = envelope_prefix{.version = protocol_version,
                .direction = direction,
                .sequence_number = sequence_number,
                .payload_size = payload.size()};

            RPC_TRACE("send_payload {}\nprefix = {}\npayload = {}",
                get_service()->get_zone_id().get_subnet(),
                rpc::to_yas_json<std::string>(prefix),
                rpc::to_yas_json<std::string>(payload_envelope));

            std::scoped_lock g(send_queue_mtx_);
            send_queue_.push(rpc::to_yas_binary(prefix));
            send_queue_.push(payload);
        }

        template<class SendPayload, class ReceivePayload>
        CORO_TASK(peer_call_result<ReceivePayload>)
        call_peer(std::uint64_t protocol_version, SendPayload sendPayload)
        {
            if (get_status() != rpc::transport_status::CONNECTED)
            {
                RPC_ERROR("call_peer: transport is not connected");
                CO_RETURN peer_call_result<ReceivePayload>{rpc::error::CALL_CANCELLED(), {}};
            }

            auto sequence_number = ++sequence_number_;

            result_listener res_payload;

            {
                RPC_TRACE("call_peer started zone: {} sequence_number: {} id: {}",
                    get_service()->get_zone_id().get_subnet(),
                    sequence_number,
                    rpc::id<SendPayload>::get(rpc::get_version()));
                std::scoped_lock lock(pending_transmits_mtx_);
                auto [it, success] = pending_transmits_.try_emplace(sequence_number, &res_payload);
                if (!success)
                {
                    RPC_ERROR("call_peer: failed to insert sequence number {}", sequence_number);
                    CO_RETURN peer_call_result<ReceivePayload>{rpc::error::TRANSPORT_ERROR(), {}};
                }
            }

            send_payload(
                protocol_version, message_direction::send, std::forward<SendPayload>(sendPayload), sequence_number);

            CO_AWAIT res_payload.event; // wait for the reply

            RPC_TRACE("call_peer succeeded zone: {} sequence_number: {} id: {}",
                get_service()->get_zone_id().get_subnet(),
                sequence_number,
                rpc::id<SendPayload>::get(rpc::get_version()));

            if (res_payload.error_code == rpc::error::OBJECT_GONE())
            {
                CO_RETURN peer_call_result<ReceivePayload>{res_payload.error_code, {}};
            }
            if (rpc::error::is_critical(res_payload.error_code))
            {
                RPC_ERROR("call_peer returning cancelled error for zone: {} sequence_number: {}",
                    get_service()->get_zone_id().get_subnet(),
                    sequence_number);
                CO_RETURN peer_call_result<ReceivePayload>{res_payload.error_code, {}};
            }

            ReceivePayload receive_payload;
            auto str_err = rpc::from_yas_binary(rpc::byte_span(res_payload.payload.payload), receive_payload);
            if (!str_err.empty())
            {
                RPC_ERROR("failed call_peer send_payload from_yas_binary");
                CO_RETURN peer_call_result<ReceivePayload>{rpc::error::PROXY_DESERIALISATION_ERROR(), {}};
            }

            CO_RETURN peer_call_result<ReceivePayload>{rpc::error::OK(), std::move(receive_payload)};
        }

        CORO_TASK(void) cleanup(std::shared_ptr<transport> transport, std::shared_ptr<rpc::service> svc);

        template<class T>
        bool run_outgoing_handlers(
            uint64_t protocol_version, message_direction direction, uint64_t sequence_number, const T& payload)
        {
            auto fingerprint = rpc::id<T>::get(protocol_version);
            for (auto& handler : custom_outgoing_handlers_)
            {
                if (handler(protocol_version, direction, sequence_number, fingerprint, &payload))
                    return true;
            }
            return false;
        }

    protected:
        void on_destination_count_zero() override { set_status(rpc::transport_status::DISCONNECTING); }

        void set_status(rpc::transport_status new_status) override
        {
            if (new_status == rpc::transport_status::DISCONNECTING)
            {
                disconnecting_since_ = std::chrono::steady_clock::now();
            }
            rpc::transport::set_status(new_status);
        }

        virtual void send_payload_post_send(
            uint64_t protocol_version, message_direction direction, post_send&& payload, uint64_t sequence_number);
        virtual void send_payload_release_send(
            uint64_t protocol_version, message_direction direction, release_send&& payload, uint64_t sequence_number);
        virtual void send_payload_object_released_send(
            uint64_t protocol_version, message_direction direction, object_released_send&& payload, uint64_t sequence_number);
        virtual void send_payload_transport_down_send(
            uint64_t protocol_version, message_direction direction, transport_down_send&& payload, uint64_t sequence_number);
        virtual void send_payload_close_connection_ack(
            uint64_t protocol_version, message_direction direction, close_connection_ack&& payload, uint64_t sequence_number);
        virtual void send_payload_close_connection_send(uint64_t protocol_version,
            message_direction direction,
            close_connection_send&& payload,
            uint64_t sequence_number);
        virtual void send_payload_call_receive(
            uint64_t protocol_version, message_direction direction, call_receive&& payload, uint64_t sequence_number);
        virtual void send_payload_try_cast_receive(
            uint64_t protocol_version, message_direction direction, try_cast_receive&& payload, uint64_t sequence_number);
        virtual void send_payload_addref_receive(
            uint64_t protocol_version, message_direction direction, addref_receive&& payload, uint64_t sequence_number);
        virtual void send_payload_init_client_initial_channel_response(uint64_t protocol_version,
            message_direction direction,
            init_client_initial_channel_response&& payload,
            uint64_t sequence_number);
        virtual void send_payload_init_client_channel_response(uint64_t protocol_version,
            message_direction direction,
            init_client_channel_response&& payload,
            uint64_t sequence_number);

    public:
        ~transport() override { }

        void add_custom_message_handler(custom_message_handler handler)
        {
            custom_message_handlers_.push_back(std::move(handler));
        }

        template<class T, class Handler> void add_typed_message_handler(Handler&& handler)
        {
            custom_message_handlers_.push_back(
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [fn = std::forward<Handler>(handler)](message_handler_context context) -> CORO_TASK(message_hook_result)
                {
                    auto& prefix = *context.prefix;
                    auto& payload = *context.payload;
                    if (payload.payload_fingerprint != rpc::id<T>::get(prefix.version))
                        CO_RETURN message_hook_result::unhandled;

                    T request;
                    auto err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
                    if (!err.empty())
                    {
                        RPC_ERROR("failed custom message deserialisation");
                        CO_RETURN message_hook_result::rejected;
                    }

                    CO_RETURN CO_AWAIT fn(context.tracker, prefix, payload, request);
                });
        }

        template<class T, class Handler> void add_typed_outgoing_message_handler(Handler&& handler)
        {
            custom_outgoing_handlers_.push_back(
                [fn = std::forward<Handler>(handler)](uint64_t protocol_version,
                    message_direction direction,
                    uint64_t sequence_number,
                    uint64_t type_fingerprint,
                    const void* typed_payload) -> bool
                {
                    if (type_fingerprint != rpc::id<T>::get(protocol_version))
                        return false;
                    return fn(protocol_version, direction, sequence_number, *static_cast<const T*>(typed_payload));
                });
        }

        template<class T> void reject_message_type()
        {
            custom_message_handlers_.push_back(
                [](message_handler_context context) -> CORO_TASK(message_hook_result)
                {
                    auto& prefix = *context.prefix;
                    auto& payload = *context.payload;
                    if (payload.payload_fingerprint == rpc::id<T>::get(prefix.version))
                        CO_RETURN message_hook_result::rejected;

                    CO_RETURN message_hook_result::unhandled;
                });
        }

        CORO_TASK(void) pump_send_and_receive();

        CORO_TASK(rpc::connect_result)
        run_custom_connect(rpc::remote_object inbound_remote_object,
            rpc::interface_ordinal inbound_interface_id,
            rpc::interface_ordinal outbound_interface_id)
        {
            RPC_DEBUG("custom connect handler zone: {}", get_zone_id().get_subnet());

            rpc::connection_settings input_descr;
            input_descr.inbound_interface_id = inbound_interface_id;
            input_descr.outbound_interface_id = outbound_interface_id;
            input_descr.remote_object_id = inbound_remote_object;

            set_adjacent_zone_id(inbound_remote_object.as_zone());

            auto handler_result = CO_AWAIT connection_handler_(input_descr, get_service(), keep_alive_.get_nullable());
            connection_handler_ = nullptr;
            int ret = handler_result.error_code;
            if (ret != rpc::error::OK())
            {
                RPC_ERROR("failed custom connect to zone {}", ret);
                set_status(rpc::transport_status::DISCONNECTING);
            }

            CO_RETURN rpc::connect_result{handler_result.error_code, handler_result.output_descriptor};
        }

        template<class ResponsePayload>
        void send_custom_connect_response(uint64_t protocol_version,
            uint64_t sequence_number,
            rpc::zone caller_zone_id,
            rpc::remote_object outbound_remote_object)
        {
            send_payload(protocol_version,
                message_direction::receive,
                ResponsePayload{
                    .caller_zone_id = caller_zone_id,
                    .outbound_remote_object = outbound_remote_object,
                },
                sequence_number);
        }

        CORO_TASK(rpc::connect_result)
        inner_connect(std::shared_ptr<rpc::object_stub> stub, connection_settings input_descr) override;

        CORO_TASK(int) inner_accept() override;

        CORO_TASK(send_result) outbound_send(send_params params) override;
        CORO_TASK(void) outbound_post(post_params params) override;
        CORO_TASK(standard_result) outbound_try_cast(try_cast_params params) override;
        CORO_TASK(standard_result) outbound_add_ref(add_ref_params params) override;
        CORO_TASK(standard_result) outbound_release(release_params params) override;
        CORO_TASK(void) outbound_object_released(object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(transport_down_params params) override;

        friend std::shared_ptr<transport> make_server(std::string name,
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<streaming::stream> stream,
            transport::connection_handler handler);
    };

    std::shared_ptr<transport> make_server(std::string name,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<streaming::stream> stream,
        transport::connection_handler handler);

    // Server-side: creates a transport that waits for the client's init message.
    // Internally wraps factory with make_new_zone_connection_handler so connection protocol details
    // stay hidden from user code.
    template<class Remote, class Local>
    std::shared_ptr<transport> make_server(std::string name,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<streaming::stream> stream,
        std::function<CORO_TASK(rpc::service_connect_result<Local>)(rpc::shared_ptr<Remote>, std::shared_ptr<rpc::service>)> factory)
    {
        auto handler = rpc::make_new_zone_connection_handler<Remote, Local>(name.c_str(), std::move(factory));
        return make_server(std::move(name), std::move(service), std::move(stream), std::move(handler));
    }

    // Returns a transport_factory that creates a streaming server transport wrapping
    // the given stream. Pass the result to service::make_acceptor().
    inline rpc::transport_factory transport_factory(std::shared_ptr<streaming::stream> stream)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        return [stream = std::move(stream)](std::string name,
                   std::shared_ptr<rpc::service> svc,
                   rpc::connection_handler handler) -> CORO_TASK(std::shared_ptr<rpc::transport>)
        { CO_RETURN make_server(std::move(name), std::move(svc), stream, std::move(handler)); };
    }

    inline std::shared_ptr<transport> make_client(
        std::string name, std::shared_ptr<rpc::service> service, std::shared_ptr<streaming::stream> stream)
    {
        return make_server(std::move(name), std::move(service), std::move(stream), nullptr);
    }

    // Produces a connection_callback for use with streaming::listener.
    // Wraps a typed zone factory so the listener does not need to know about
    // connection_handler or make_new_zone_connection_handler.
    template<class Remote, class Local>
    std::function<CORO_TASK(void)(const std::string&, std::shared_ptr<rpc::service>, std::shared_ptr<streaming::stream>)>
    make_connection_callback(
        std::function<CORO_TASK(rpc::service_connect_result<Local>)(rpc::shared_ptr<Remote>, std::shared_ptr<rpc::service>)> factory)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        return [fn = std::move(factory)](std::string name,
                   std::shared_ptr<rpc::service> svc,
                   std::shared_ptr<streaming::stream> stm) -> CORO_TASK(void)
        {
            make_server<Remote, Local>(name, std::move(svc), std::move(stm), fn);
            CO_RETURN;
        };
    }
}
