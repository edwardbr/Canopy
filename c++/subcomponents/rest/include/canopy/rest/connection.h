/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rpc/rpc.h>
#include <streaming/http_client/client.h>
#include <streaming/stream.h>

namespace canopy::rest
{
    struct connection_settings;

    struct name_value
    {
        std::string name;
        std::string value;
    };

    using query_parameter = name_value;
    using cookie = name_value;
    using request_mutator = std::function<int(streaming::http_client::request&)>;

    struct stream_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<streaming::stream> stream;
    };

    using stream_connector
        = std::function<CORO_TASK(stream_result)(const connection_settings&, std::shared_ptr<rpc::service>)>;

    struct authority
    {
        std::string scheme{"https"};
        std::string host;
        uint16_t port{0};
        std::string base_path;
        bool ipv6{false};
    };

    struct tls_options
    {
        bool verify_peer{true};
        std::string trust_anchor;
        std::string trust_anchor_file;
    };

    struct connection_settings
    {
        authority endpoint;
        tls_options tls;
        std::vector<streaming::http_client::header> default_headers;
        std::vector<query_parameter> default_query_parameters;
        std::vector<cookie> default_cookies;
        request_mutator before_send;
        stream_connector connector;
        std::chrono::milliseconds connect_timeout{10000};
        std::chrono::milliseconds receive_timeout{10000};
        std::size_t max_response_bytes{2U * 1024U * 1024U};
    };

    template<class Interface> struct connect_result
    {
        int error_code{rpc::error::OK()};
        rpc::shared_ptr<Interface> object;
        std::shared_ptr<streaming::stream> stream;
    };

    [[nodiscard]] bool uses_tls(const authority& endpoint) noexcept;
    [[nodiscard]] uint16_t effective_port(const authority& endpoint) noexcept;
    [[nodiscard]] std::string http_host(const authority& endpoint);

    void apply_request_defaults(
        streaming::http_client::request& request,
        const connection_settings& settings);
    [[nodiscard]] int prepare_request(
        streaming::http_client::request& request,
        const connection_settings& settings);

    CORO_TASK(stream_result)
    connect_stream(
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service = {});
} // namespace canopy::rest
