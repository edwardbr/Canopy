/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// for future coroutine deliniation
#define NAMESPACE_INLINE_BEGIN                                                                                         \
    inline namespace synchronous                                                                                       \
    {
#define NAMESPACE_INLINE_END }

#include <rpc/internal/version.h>
#include <rpc/internal/build_modifiers.h>

// needed for uint128_t and int128_t serialisation support protobuffers are sending pairs of uint64_t's
#include <rpc/internal/polyfill/int128.h>
#include <rpc/internal/polyfill/expected.h>
#include <rpc/internal/polyfill/format.h>
#include <chrono>
#include <exception>
#include <shared_mutex>
#ifdef CANOPY_BUILD_COROUTINE
#  include <coroutine>
#  include <rpc/internal/coro_runtime/runtime.h>
#endif

namespace rpc
{
    using shared_mutex = std::shared_mutex;
    template<typename Mutex> using shared_lock = std::shared_lock<Mutex>;
}
#include <rpc/internal/coroutine_support.h>

// byte-span type used throughout the RPC layer
#include <rpc/internal/span.h>
#include <rpc/internal/polyfill/event.h>

#include <rpc/rpc_types.h>
#include <rpc/internal/zone_authenticator.h>
#include <rpc/internal/address_utils.h>
#ifdef CANOPY_USE_TELEMETRY
#  include <rpc/internal/telemetry_fwd.h>
#endif

#include <rpc/internal/error_codes.h>
#include <rpc/internal/logger.h>
#include <rpc/internal/assert.h>

#include <rpc/internal/types.h>
#include <rpc/internal/serialiser.h>
#include <rpc/internal/member_ptr.h>

// synchronous/coroutine sensitive headers

// parameter/result bundles used by marshalling interfaces
#include <rpc/internal/marshaller_params.h>

// the key interzone communication definiton that all services and service_proxies need to implement
#include <rpc/internal/marshaller.h>

// all remoteable objects need to implement this interface
#include <rpc/internal/casting_interface.h>

// RPC-aware pointer implementation
#include <rpc/internal/remote_pointer.h>

// remote proxy of an object
#include <rpc/internal/object_proxy.h>

// some helper forward declarations
#include <rpc/internal/bindings_fwd.h>

// transport base class
#include <rpc/internal/transport.h>

// root services allocate new zone ids
#include <rpc/internal/zone_id_allocator.h>

// the base class that all remoteable objects should inherit from
#include <rpc/internal/base.h>

// the remote proxy to another zone
#include <rpc/internal/service_proxy.h>

// the link between transports and marshallers for routing
#include <rpc/internal/pass_through.h>

// the deserialisation logic to an object
#include <rpc/internal/stub.h>

// services manage the logical zones between which data is marshalled
#include <rpc/internal/service.h>

// internal plumbing
#include <rpc/internal/bindings.h>
