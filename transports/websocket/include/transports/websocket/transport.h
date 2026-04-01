/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <functional>
#include <vector>
#include <atomic>

#include <rpc/rpc.h>
#include <streaming/stream.h>
#include <websocket_protocol/websocket_protocol.h>

namespace websocket_protocol
{
    // Transport from child zone to parent zone
    // Used by child to communicate with parent
    class transport : public rpc::transport
    {

    public:
        using connection_handler = rpc::connection_handler;

        struct activity_tracker
        {
            std::shared_ptr<transport> transport_;
            std::shared_ptr<rpc::service> svc_; // kept here to keep the service alive

            activity_tracker(
                std::shared_ptr<transport> t,
                std::shared_ptr<rpc::service> s)
                : transport_(std::move(t))
                , svc_(std::move(s))
            {
            }
            activity_tracker(activity_tracker&&) noexcept = default;
            activity_tracker(const activity_tracker&) = delete;
            activity_tracker& operator=(activity_tracker&&) = delete;
            activity_tracker& operator=(const activity_tracker&) = delete;

            ~activity_tracker()
            {
                if (svc_)
                    svc_->spawn(transport_->cleanup(transport_, svc_));
            }
        };

        // Server-side make_server: zone factory replaces the raw connection_handler.
        template<
            class Remote,
            class Local>
        static CORO_TASK(std::shared_ptr<transport>) make_server(
            const std::shared_ptr<rpc::service>& service,
            const std::shared_ptr<streaming::stream>& stream,
            std::function<CORO_TASK(rpc::service_connect_result<Local>)(
                const rpc::shared_ptr<Remote>&,
                const std::shared_ptr<rpc::service>&)> factory)
        {
            CO_RETURN CO_AWAIT make_server(
                service, stream, rpc::make_new_zone_connection_handler<Remote, Local>("websocket", std::move(factory)));
        }

        static CORO_TASK(std::shared_ptr<transport>) make_server(
            const std::shared_ptr<rpc::service>& service,
            const std::shared_ptr<streaming::stream>& stream,
            connection_handler&& handler);

        transport(
            const std::shared_ptr<rpc::service>& service,
            rpc::zone adjacent_zone_id,
            std::shared_ptr<streaming::stream> stream,
            connection_handler&& handler);

        ~transport() override CANOPY_DEFAULT_DESTRUCTOR;

        CORO_TASK(void) receive_consumer_loop(std::unique_ptr<activity_tracker> tracker);

        CORO_TASK(rpc::connect_result)
        inner_connect(
            std::shared_ptr<rpc::object_stub> stub,
            rpc::connection_settings input_descr) override
        {
            std::ignore = stub;
            std::ignore = input_descr;
            // Parent transport is connected immediately - no handshake needed
            CO_RETURN rpc::connect_result{rpc::error::OK(), {}};
        }
        CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

        // Outbound i_marshaller interface - sends from child to parent
        CORO_TASK(rpc::send_result) outbound_send(rpc::send_params params) override;
        CORO_TASK(void) outbound_post(rpc::post_params params) override;
        CORO_TASK(rpc::standard_result) outbound_try_cast(rpc::try_cast_params params) override;
        CORO_TASK(rpc::standard_result) outbound_add_ref(rpc::add_ref_params params) override;
        CORO_TASK(rpc::standard_result) outbound_release(rpc::release_params params) override;
        CORO_TASK(void) outbound_object_released(rpc::object_released_params params) override;
        CORO_TASK(void) outbound_transport_down(rpc::transport_down_params params) override;

        CORO_TASK(void) stub_handle_send(websocket_protocol::v1::envelope request);

        void set_local_object_id(rpc::object id) { local_object_id_ = id; }

        CORO_TASK(void)
        cleanup(
            std::shared_ptr<transport> transport,
            std::shared_ptr<rpc::service> svc);
        bool is_valid(coro::net::io_status status);

    private:
        std::shared_ptr<streaming::stream> stream_;
        rpc::object local_object_id_{0};
        connection_handler handler_;
    };
}
