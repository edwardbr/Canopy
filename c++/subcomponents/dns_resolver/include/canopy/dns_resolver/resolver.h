/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <sys/socket.h>

#include <rpc/rpc.h>

namespace canopy::dns_resolver
{
    enum class address_family
    {
        any,
        ipv4,
        ipv6
    };

    enum class socket_type
    {
        any,
        stream,
        datagram
    };

    struct endpoint
    {
        address_family family{address_family::any};
        std::string numeric_host;
        uint16_t port{0};
        int native_socket_type{0};
        int native_protocol{0};
        int ttl_seconds{0};
        std::array<uint8_t, 4> ipv4{};
        std::array<uint8_t, 16> ipv6{};
    };

    struct resolve_options
    {
        address_family family{address_family::any};
        socket_type type{socket_type::stream};
        int protocol{0};
        std::chrono::milliseconds timeout{std::chrono::milliseconds{5000}};
        int tries{2};
    };

    struct resolve_result
    {
        int error_code{rpc::error::OK()};
        int native_error{0};
        int timeouts{0};
        std::string error_message;
        std::string canonical_name;
        std::vector<endpoint> endpoints;
    };

    [[nodiscard]] resolve_options make_stream_resolve_options(
        bool ipv6 = false,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) noexcept;

    [[nodiscard]] bool sockaddr_from_endpoint(
        const endpoint& endpoint,
        sockaddr_storage& storage,
        socklen_t& storage_size) noexcept;

    [[nodiscard]] resolve_result resolve_service_blocking(
        const std::string& host,
        const std::string& service,
        resolve_options options = {});

    [[nodiscard]] resolve_result resolve_host_blocking(
        const std::string& host,
        uint16_t port,
        resolve_options options = {});

    [[nodiscard]] CORO_TASK(resolve_result) resolve_service(
        std::string host,
        std::string service,
        resolve_options options = {});

    [[nodiscard]] CORO_TASK(resolve_result) resolve_host(
        std::string host,
        uint16_t port,
        resolve_options options = {});
}
