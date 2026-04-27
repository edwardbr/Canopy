/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <chrono>
#include <exception>
#include <shared_mutex>

namespace rpc
{
    using shared_mutex = std::shared_mutex;
    template<typename Mutex> using shared_lock = std::shared_lock<Mutex>;
}

#include <rpc/internal/version.h>
#include <rpc/internal/build_modifiers.h>

#include <rpc/internal/polyfill/int128.h>
#include <rpc/internal/polyfill/expected.h>
#include <rpc/internal/polyfill/format.h>

// synchronous/coroutine sensitive headers
#include <rpc/internal/coroutine_support.h>
#include <rpc/internal/span.h>
#include <rpc/internal/polyfill/event.h>

// machine generated code
#include <rpc/rpc_types.h>
#include <rpc/logging.h>

// internal headers
#include <rpc/internal/zone_authenticator.h>
#include <rpc/internal/address_utils.h>
#include <rpc/internal/telemetry_fwd.h>
#include <rpc/internal/error_codes.h>
#include <rpc/internal/logger.h>
#include <rpc/internal/assert.h>
#include <rpc/internal/types.h>
#include <rpc/internal/serialiser.h>
#include <rpc/internal/member_ptr.h>

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
