/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rpc/rpc.h>
#include <streaming/stream.h>
#include <streaming/stream_layers.h>

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_ATTESTATION
#  include <security/attestation/service.h>
#endif

namespace streaming::layer_factory
{
    // Applies implementation-owned stream layer settings to an existing
    // streaming::stream. This factory is intentionally transport-neutral: it is
    // usable by the host connection factory, protected runtimes, and
    // application services that construct streams inside a protected runtime.
    //
    // Security-sensitive layers must decide locally which settings are trusted.
    // Protected runtimes should treat host-provided JSON as routing hints only
    // and source key material or policy from provisioned or compile-time values.
    enum class layer_direction
    {
        connect,
        accept,
    };

    struct stream_layer_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<::streaming::stream> stream;
    };

    struct layer_context
    {
        // Scheduler/executor retained by stream layers that need background
        // work after construction. spsc_buffered_stream uses it for proxy
        // loops; OpenSSL TLS uses it for destructor-time close cleanup.
        rpc::executor_ptr stream_scheduler;

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_ATTESTATION
        std::shared_ptr<canopy::security::attestation::attestation_service> attestation_service;
        std::unordered_map<std::string, std::shared_ptr<canopy::security::attestation::attestation_service>> named_attestation_services;
#endif
    };

    // Synchronous construction for layers that do not perform a handshake.
    auto apply_stream_layer(
        std::shared_ptr<::streaming::stream> stream,
        const rpc::stream_layers::stream_layer_settings& layer,
        layer_direction direction) -> stream_layer_result;

    auto apply_stream_layers(
        std::shared_ptr<::streaming::stream> stream,
        const std::vector<rpc::stream_layers::stream_layer_settings>& layers,
        size_t first_layer,
        layer_direction direction) -> stream_layer_result;

    // Full construction path for wrappers that may need async handshakes or
    // runtime-supplied services. The connection factory uses this path.
    auto apply_stream_layer_async(
        std::shared_ptr<::streaming::stream> stream,
        const rpc::stream_layers::stream_layer_settings& layer,
        layer_direction direction,
        const layer_context& context = {}) -> CORO_TASK(stream_layer_result);

    auto apply_stream_layers_async(
        std::shared_ptr<::streaming::stream> stream,
        const std::vector<rpc::stream_layers::stream_layer_settings>& layers,
        size_t first_layer,
        layer_direction direction,
        const layer_context& context = {}) -> CORO_TASK(stream_layer_result);
} // namespace streaming::layer_factory
