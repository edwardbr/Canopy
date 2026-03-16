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
#include "websocket_demo/websocket_demo.h"

namespace websocket_demo
{
    namespace v1
    {
        // Transport from child zone to parent zone
        // Used by child to communicate with parent
        class transport : public rpc::transport
        {

        public:
            using connection_handler = rpc::connection_handler;

            // Server-side make_server: zone factory replaces the raw connection_handler.
            template<class Remote, class Local>
            static CORO_TASK(std::shared_ptr<transport>) make_server(const std::shared_ptr<rpc::service>& service,
                const std::shared_ptr<streaming::stream>& stream,
                std::function<CORO_TASK(int)(
                    const rpc::shared_ptr<Remote>&, rpc::shared_ptr<Local>&, const std::shared_ptr<rpc::service>&)> factory)
            {
                CO_RETURN CO_AWAIT make_server(service,
                    stream,
                    rpc::make_new_zone_connection_handler<Remote, Local>("websocket", std::move(factory)));
            }

            static CORO_TASK(std::shared_ptr<transport>) make_server(const std::shared_ptr<rpc::service>& service,
                const std::shared_ptr<streaming::stream>& stream,
                connection_handler&& handler);

            transport(const std::shared_ptr<rpc::service>& service,
                rpc::zone adjacent_zone_id,
                std::shared_ptr<streaming::stream> stream,
                connection_handler&& handler);

            ~transport() override CANOPY_DEFAULT_DESTRUCTOR;

            CORO_TASK(void) receive_consumer_loop();

            CORO_TASK(int)
            inner_connect(const std::shared_ptr<rpc::object_stub>& stub,
                rpc::connection_settings& input_descr,
                rpc::interface_descriptor& output_descr) override
            {
                std::ignore = stub;
                std::ignore = input_descr;
                std::ignore = output_descr;
                // Parent transport is connected immediately - no handshake needed
                CO_RETURN rpc::error::OK();
            }
            CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

            template<class in_param_type, class out_param_type>
            static std::function<CORO_TASK(int)(
                rpc::interface_descriptor input_descr, rpc::interface_descriptor& output_descr, std::shared_ptr<transport>& child)>
            bind(rpc::zone new_zone_id,
                std::function<CORO_TASK(int)(const rpc::shared_ptr<in_param_type>&,
                    rpc::shared_ptr<out_param_type>&,
                    const std::shared_ptr<rpc::child_service>&)>&& child_entry_point_fn);

            // Outbound i_marshaller interface - sends from child to parent
            CORO_TASK(int)
            outbound_send(uint64_t protocol_version,
                rpc::encoding encoding,
                uint64_t tag,
                rpc::caller_zone caller_zone_id,
                rpc::remote_object remote_object_id,
                rpc::interface_ordinal interface_id,
                rpc::method method_id,
                const rpc::byte_span& in_data,
                std::vector<char>& out_buf_,
                const std::vector<rpc::back_channel_entry>& in_back_channel,
                std::vector<rpc::back_channel_entry>& out_back_channel) override;

            CORO_TASK(void)
            outbound_post(uint64_t protocol_version,
                rpc::encoding encoding,
                uint64_t tag,
                rpc::caller_zone caller_zone_id,
                rpc::remote_object remote_object_id,
                rpc::interface_ordinal interface_id,
                rpc::method method_id,
                const rpc::byte_span& in_data,
                const std::vector<rpc::back_channel_entry>& back_channel) override;

            CORO_TASK(int)
            outbound_try_cast(uint64_t protocol_version,
                rpc::caller_zone caller_zone_id,
                rpc::remote_object remote_object_id,
                rpc::interface_ordinal interface_id,
                const std::vector<rpc::back_channel_entry>& in_back_channel,
                std::vector<rpc::back_channel_entry>& out_back_channel) override;

            CORO_TASK(int)
            outbound_add_ref(uint64_t protocol_version,
                rpc::remote_object remote_object_id,
                rpc::caller_zone caller_zone_id,
                rpc::requesting_zone requesting_zone_id,
                rpc::add_ref_options build_out_param_channel,
                const std::vector<rpc::back_channel_entry>& back_channel,
                std::vector<rpc::back_channel_entry>& out_back_channel) override;

            CORO_TASK(int)
            outbound_release(uint64_t protocol_version,
                rpc::remote_object remote_object_id,
                rpc::caller_zone caller_zone_id,
                rpc::release_options options,
                const std::vector<rpc::back_channel_entry>& back_channel,
                std::vector<rpc::back_channel_entry>& out_back_channel) override;

            // New methods from i_marshaller interface
            CORO_TASK(void)
            outbound_object_released(uint64_t protocol_version,
                rpc::remote_object remote_object_id,
                rpc::caller_zone caller_zone_id,
                const std::vector<rpc::back_channel_entry>& back_channel) override;

            CORO_TASK(void)
            outbound_transport_down(uint64_t protocol_version,
                rpc::destination_zone destination_zone_id,
                rpc::caller_zone caller_zone_id,
                const std::vector<rpc::back_channel_entry>& back_channel) override;

            CORO_TASK(void) stub_handle_send(websocket_demo::v1::envelope request);

            void set_local_object_id(rpc::object id) { local_object_id_ = id; }

        private:
            std::shared_ptr<streaming::stream> stream_;
            rpc::object local_object_id_{0};
            connection_handler handler_;
            stdex::member_ptr<transport> keep_alive_;
        };
    }
}
